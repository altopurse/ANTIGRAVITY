// Antigravity Voice Engine - license server
//
// Flow:
//   GET  /            -> landing page: download button + pricing
//   GET  /download    -> serves the Windows installer (server/public/downloads/)
//   GET  /buy         -> creates a Mollie payment for the chosen plan, redirects to checkout
//   GET  /key?pid=... -> Mollie redirects the buyer here; if paid, shows their license key
//   GET  /api/verify?key=... -> the desktop app calls this; returns { valid: true|false }
//   POST /webhook     -> Mollie status pings (acknowledged, nothing stored)
//
// Stateless by design: a license key is "ANTI-<paymentId>-<HMAC>" signed with
// LICENSE_SECRET, so no database is needed and Render free-tier restarts are harmless.
//
// Required environment variables (set in the Render dashboard):
//   MOLLIE_API_KEY  - your Mollie API key (test_... or live_...)
//   LICENSE_SECRET  - long random string; NEVER change it or all sold keys break
//   BASE_URL        - public URL of this service, e.g. https://antigravity-license.onrender.com

const express = require("express");
const crypto = require("crypto");
const fs = require("fs");
const path = require("path");

const app = express();
// Render terminates TLS behind exactly one proxy hop; trusting it makes
// req.ip the real client address (the proxy-appended X-Forwarded-For entry,
// which a client can't spoof) - that's what the rate limiter keys on.
app.set("trust proxy", 1);
app.use(express.urlencoded({ extended: true }));
app.use(express.json());
// Static assets: og.png (social share preview) and favicon.ico live here.
app.use(express.static(path.join(__dirname, "public"), { index: false }));

const { MOLLIE_API_KEY, LICENSE_SECRET, BASE_URL } = process.env;
const PORT = process.env.PORT || 3000;
const PRODUCT = "Antigravity Voice Engine license";

// Two plans: one-time lifetime key, or a monthly subscription whose key
// expires unless Mollie keeps reporting successful recurring payments.
const PLANS = {
  life:    { amount: { currency: "GBP", value: "10.00" }, label: "Lifetime" },
  // Intro subscription: £0.80 charged now (first month), then £3.00/month
  // recurring starting a month later.
  monthly: {
    amount:     { currency: "GBP", value: "0.80" }, // first payment (this month)
    recurring:  { currency: "GBP", value: "3.00" }, // every month after
    label: "Monthly",
  },
};
// Streamer referral codes: viewers get lifetime at £8 (20% off); the streamer
// earns £2.50 per lifetime sale. Promos never stack - with a code, monthly has
// NO 80p intro (plain £3.00 from month one) and earns no commission, so a
// commission can never exceed what the buyer actually paid.
const REF_LIFE_PRICE = "8.00";
const REF_COMMISSION = "2.50";
// Each paid month grants 35 days so retries/bank delays never lock a payer out.
const SUB_GRACE_MS = 35 * 24 * 3600 * 1000;

// App update check: bump these env vars in Render when you release a new build.
const LATEST_VERSION = process.env.LATEST_VERSION || "";
const DOWNLOAD_URL = process.env.DOWNLOAD_URL || "";
// Optional anti-tamper: set to the current build's self-hash (printed by
// package.ps1) so the dashboard can flag binaries reporting a different hash.
const EXPECTED_HASH = process.env.EXPECTED_HASH || "";

// Device binding (anti key-sharing). Optional: if the Upstash vars are not
// set, the server falls back to signature-only checks so it still runs.
const { UPSTASH_REDIS_REST_URL, UPSTASH_REDIS_REST_TOKEN } = process.env;
const DEVICE_LIMIT = parseInt(process.env.DEVICE_LIMIT || "2", 10);

// Analytics throttle. Page-view tracking is the biggest Redis spender (many
// commands per view), and the Upstash free tier caps monthly requests. This
// fraction (0..1) of page views get the full per-visitor treatment; the rest
// only bump a couple of cheap aggregate counters. Lower it in Render (no
// deploy needed) if you approach the quota; 1 = track every view in full.
// Purchases/downloads/activations are NEVER sampled - only page views are.
const ANALYTICS_SAMPLE = Math.max(0, Math.min(1, Number(process.env.ANALYTICS_SAMPLE || "1")));
const bindingEnabled = !!(UPSTASH_REDIS_REST_URL && UPSTASH_REDIS_REST_TOKEN);

for (const name of ["MOLLIE_API_KEY", "LICENSE_SECRET", "BASE_URL"]) {
  if (!process.env[name]) {
    console.error(`FATAL: missing required environment variable ${name}`);
    process.exit(1);
  }
}

if (!bindingEnabled) {
  console.warn(
    "WARNING: UPSTASH_REDIS_REST_URL/TOKEN not set - device binding is OFF, " +
    "keys will work on unlimited devices. Set them to stop key-sharing."
  );
}

// Upstash Redis REST helper: sends a single command as a JSON array.
async function redis(cmd) {
  const res = await fetch(UPSTASH_REDIS_REST_URL, {
    method: "POST",
    headers: {
      Authorization: `Bearer ${UPSTASH_REDIS_REST_TOKEN}`,
      "Content-Type": "application/json",
    },
    body: JSON.stringify(cmd),
  });
  const data = await res.json().catch(() => ({}));
  if (!res.ok || data.error) {
    throw new Error(`Redis ${cmd[0]} -> ${res.status}: ${JSON.stringify(data)}`);
  }
  return data.result;
}

// Upstash returns HGETALL as a flat [field, value, field, value, ...] array
// (or null for a missing key). Turns it into a plain object.
function flattenHash(flat) {
  const obj = {};
  if (!flat) return obj;
  for (let i = 0; i + 1 < flat.length; i += 2) obj[flat[i]] = flat[i + 1];
  return obj;
}

// ---------- rate limiting ----------
//
// In-memory fixed-window limiter, keyed by route name + client IP. Deliberately
// NOT Redis-backed: it must cost zero Upstash quota (abuse of these endpoints
// is exactly what would burn the quota), and a single Render instance means
// in-memory is accurate. A restart resets the windows, which is fine - the
// point is stopping sustained abuse, not perfect accounting.

const rlBuckets = new Map();

// Returns true if this request is allowed, false if the caller is over the
// limit of `max` requests per `windowMs` for this route.
function rateAllow(name, req, max, windowMs) {
  const now = Date.now();
  const k = name + ":" + (req.ip || "?");
  let b = rlBuckets.get(k);
  if (!b || now > b.reset) {
    b = { count: 0, reset: now + windowMs };
    rlBuckets.set(k, b);
  }
  b.count++;
  return b.count <= max;
}

// Sweep expired buckets so the map can't grow unbounded under an IP-rotating
// flood. unref() keeps the timer from holding the process open.
setInterval(() => {
  const now = Date.now();
  for (const [k, b] of rlBuckets) if (now > b.reset) rlBuckets.delete(k);
}, 10 * 60 * 1000).unref();

// ---------- first-party analytics (no third parties, all in our Redis) ----------
//
// A random "vid" cookie identifies a browser so returning visitors and the
// path from visit -> download -> purchase can be followed. Everything is
// fire-and-forget: analytics must never slow down or break a real request.
// Uniques use HyperLogLog (PFADD/PFCOUNT) so the count stays tiny regardless
// of traffic; event lists are capped.

function today() {
  return new Date().toISOString().slice(0, 10); // YYYY-MM-DD (UTC)
}

function clientIp(req) {
  return String(req.headers["x-forwarded-for"] || req.socket.remoteAddress || "")
    .split(",")[0].trim();
}

function clientCountry(req) {
  return String(req.headers["cf-ipcountry"] || req.headers["x-vercel-ip-country"] || "").slice(0, 8);
}

function refHost(req) {
  const r = String(req.headers["referer"] || req.headers["referrer"] || "");
  if (!r) return "direct";
  try { return new URL(r).hostname.replace(/^www\./, "").slice(0, 48); }
  catch { return "other"; }
}

// Reads the vid cookie, or assigns a new one (also sets it on the response).
// Returns the visitor id string.
function visitorId(req, res) {
  const m = /(?:^|;\s*)vid=([A-Za-z0-9]{16})/.exec(String(req.headers.cookie || ""));
  if (m) return m[1];
  const vid = crypto.randomBytes(8).toString("hex");
  // 2-year first-party cookie; SameSite=Lax so it survives the Mollie round-trip
  const prev = res.getHeader("Set-Cookie");
  const cookie = `vid=${vid}; Path=/; Max-Age=${2 * 365 * 24 * 3600}; SameSite=Lax`;
  res.setHeader("Set-Cookie", prev ? [].concat(prev, cookie) : cookie);
  return vid;
}

// A single "presence" sorted set scored by last-activity time (ms) powers the
// most-recently-active list. Members are "v:<vid>" for web-only visitors and
// "k:<key>" for app users; once a visitor buys, their v: entry is removed so
// they appear as one person under their key.
function touchPresence(member) {
  redis(["ZADD", "presence", String(Date.now()), member]).catch(() => {});
  // Occasionally trim to the newest 500 so the set can't grow unbounded
  if (Math.random() < 0.03) {
    redis(["ZREMRANGEBYRANK", "presence", "0", "-501"]).catch(() => {});
  }
}

// Records a page view + updates the visitor's profile. Called on real pages.
//
// Redis-cost split so the Upstash free-tier quota lasts far longer:
//   - Aggregate KPIs (totals, uniques, page/ref breakdown) always run - cheap,
//     and they're what actually matters. ~5 commands.
//   - The expensive per-visitor profile + journey (~7 commands, powering the
//     individual visitor drill-down) runs on an ANALYTICS_SAMPLE fraction only.
// So at ANALYTICS_SAMPLE=1 behavior is unchanged; at 0.1 a page view costs ~5
// commands instead of ~14, ~3x more traffic per quota.
function trackView(req, res, pageName) {
  if (!bindingEnabled) return "";
  const vid = visitorId(req, res);
  const d = today();
  const ref = refHost(req);
  const country = clientCountry(req);

  // Always: cheap aggregate counters (the headline dashboard numbers)
  Promise.all([
    redis(["INCR", "stats:views:total"]),
    redis(["INCR", "stats:views:" + d]),
    redis(["PFADD", "stats:uniq", vid]),
    redis(["PFADD", "stats:uniq:" + d, vid]),
    redis(["HINCRBY", "stats:page", pageName, 1]),
    redis(["HINCRBY", "stats:ref", ref, 1]),
  ]).catch((e) => console.error("trackView(agg) failed:", e.message));

  // Sampled: presence + full per-visitor profile + journey timeline
  if (Math.random() < ANALYTICS_SAMPLE) {
    touchPresence("v:" + vid);
    const now = new Date().toISOString();
    const ua = String(req.headers["user-agent"] || "").slice(0, 120);
    Promise.all([
      country ? redis(["HINCRBY", "stats:country", country, 1]) : Promise.resolve(),
      redis(["HSET", "visitor:" + vid, "lastSeen", now, "country", country, "ref", ref, "ua", ua]),
      redis(["HSETNX", "visitor:" + vid, "firstSeen", now]),
      redis(["HINCRBY", "visitor:" + vid, "views", 1]),
      redis(["SADD", "visitors", vid]),
      redis(["LPUSH", "journey:" + vid, JSON.stringify({ at: now, t: "view", p: pageName })]),
      redis(["LTRIM", "journey:" + vid, "0", "49"]),
    ]).catch((e) => console.error("trackView(detail) failed:", e.message));
  }
  return vid;
}

// Records a named event (download, buy_click, purchase, key_activated) plus a
// capped recent-events feed. meta is a short object stored with the event.
function trackEvent(req, res, name, meta = {}) {
  if (!bindingEnabled) return "";
  const vid = visitorId(req, res);
  const entry = JSON.stringify({
    at: new Date().toISOString(), vid, event: name,
    country: clientCountry(req), ref: refHost(req), ...meta,
  });
  Promise.all([
    redis(["INCR", "stats:event:" + name]),
    redis(["HINCRBY", "visitor:" + vid, "ev_" + name, 1]),
    redis(["HSET", "visitor:" + vid, "ev_" + name + "_at", new Date().toISOString()]),
    redis(["LPUSH", "analytics:events", entry]),
    redis(["LTRIM", "analytics:events", "0", "299"]),
    // Same journey timeline as page views, so a visitor's clicks show inline
    redis(["LPUSH", "journey:" + vid, JSON.stringify({ at: new Date().toISOString(), t: "event", e: name })]),
    redis(["LTRIM", "journey:" + vid, "0", "49"]),
  ]).catch((e) => console.error("trackEvent failed:", e.message));
  return vid;
}

// Where a visitor got to in the funnel, from their event counters. Highest
// stage reached wins. Used for the badge in the presence + visitor list.
function visitorFunnel(v) {
  if (v.activatedAt || Number(v.ev_purchase || 0) > 0) return { stage: "Bought", color: "#5fd18a" };
  if (Number(v.ev_buy_click || 0) > 0) return { stage: "Clicked buy", color: "#38b0f8" };
  if (Number(v.ev_download || 0) > 0) return { stage: "Downloaded", color: "#c99cf1" };
  return { stage: "Browsed", color: "#9a9aa5" };
}

// Human "3m 40s" from two ISO timestamps
function humanDuration(fromIso, toIso) {
  const ms = Date.parse(toIso) - Date.parse(fromIso);
  if (!(ms > 0)) return "0s";
  const s = Math.round(ms / 1000);
  if (s < 60) return s + "s";
  const m = Math.floor(s / 60);
  if (m < 60) return m + "m " + (s % 60) + "s";
  const h = Math.floor(m / 60);
  return h + "h " + (m % 60) + "m";
}

// ---------- license key helpers ----------

function sign(paymentId) {
  return crypto
    .createHmac("sha256", LICENSE_SECRET)
    .update(paymentId)
    .digest("hex")
    .slice(0, 20)
    .toUpperCase();
}

function makeKey(paymentId) {
  return `ANTI-${paymentId}-${sign(paymentId)}`;
}

function parseKey(key) {
  const m = /^ANTI-(.+)-([A-F0-9]{20})$/.exec(String(key || "").trim());
  return m ? { paymentId: m[1], sig: m[2] } : null;
}

// ---------- Mollie REST helper ----------

async function mollie(method, path, body) {
  const res = await fetch("https://api.mollie.com/v2" + path, {
    method,
    headers: {
      Authorization: `Bearer ${MOLLIE_API_KEY}`,
      "Content-Type": "application/json",
    },
    body: body ? JSON.stringify(body) : undefined,
  });
  const data = await res.json().catch(() => ({}));
  if (!res.ok) {
    throw new Error(`Mollie ${method} ${path} -> ${res.status}: ${JSON.stringify(data)}`);
  }
  return data;
}

// ---------- tiny HTML shell ----------

function page(title, bodyHtml, opts = {}) {
  const width = opts.wide ? 760 : 560;
  // Social share preview (shows a proper card when the link is pasted into
  // Discord, Twitter/X, Reddit, iMessage, etc.) - critical since sharing is
  // the main growth channel. Same image/description across pages.
  const desc = "Real-time voice changer + soundboard for Discord, games & calls. Pay £10 once, own it forever.";
  const ogImg = `${BASE_URL}/og.png`;
  return `<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>${title}</title>
<meta name="description" content="${desc}">
<link rel="icon" href="/favicon.ico">
<meta property="og:type" content="website">
<meta property="og:site_name" content="Antigravity Voice Engine">
<meta property="og:title" content="${title}">
<meta property="og:description" content="${desc}">
<meta property="og:image" content="${ogImg}">
<meta property="og:url" content="${BASE_URL}/">
<meta name="twitter:card" content="summary_large_image">
<meta name="twitter:title" content="${title}">
<meta name="twitter:description" content="${desc}">
<meta name="twitter:image" content="${ogImg}">
<style>
  *{box-sizing:border-box}
  body{font-family:system-ui,sans-serif;background:#0f0f14;color:#f2f2f7;display:flex;justify-content:center;
    padding:48px 16px;margin:0}
  main{max-width:${width}px;width:100%}
  h1{color:#8a74ff;font-size:1.6rem;margin-bottom:4px}
  h2{color:#f2f2f7;font-size:1.15rem;margin:32px 0 12px}
  a.btn,button{display:inline-block;background:#6d5ae0;color:#fff;border:none;border-radius:8px;
    padding:12px 24px;font-size:1rem;text-decoration:none;cursor:pointer;font-family:inherit}
  a.btn:hover,button:hover{background:#8a74ff}
  a.btn.big{padding:16px 36px;font-size:1.15rem;font-weight:600}
  a.btn.ghost{background:#1c1c26;border:1px solid #2e2e3e}
  a.btn.ghost:hover{background:#26263a}
  code.key{display:block;background:#1c1c26;border:1px solid #2e2e3e;border-radius:8px;padding:16px;
    font-size:1.05rem;word-break:break-all;margin:16px 0;user-select:all}
  p.muted{color:#9a9aa5}
  .hero{text-align:center;padding:24px 0 8px}
  .hero p.tagline{font-size:1.05rem;color:#c7c7d1;max-width:460px;margin:8px auto 28px}
  .pricing{display:grid;grid-template-columns:1fr 1fr;gap:16px;margin:20px 0}
  @media (max-width:520px){.pricing{grid-template-columns:1fr}}
  .card{background:#161620;border:1px solid #2e2e3e;border-radius:12px;padding:22px;text-align:center}
  .card .price{font-size:1.8rem;font-weight:700;color:#f2f2f7;margin:6px 0 14px}
  .card .price span{font-size:0.95rem;color:#9a9aa5;font-weight:400}
  .features{list-style:none;padding:0;margin:0;display:grid;grid-template-columns:1fr 1fr;gap:10px 20px}
  .features li{color:#c7c7d1;font-size:0.95rem;padding-left:22px;position:relative}
  .features li::before{content:"✓";position:absolute;left:0;color:#8a74ff;font-weight:700}
  @media (max-width:520px){.features{grid-template-columns:1fr}}
  .chip{background:#161620;border:1px solid #2e2e3e;border-radius:20px;padding:6px 14px;
    font-size:0.85rem;color:#c7c7d1}
  ol code{background:#0f0f14;border:1px solid #2e2e3e;border-radius:4px;padding:1px 6px;font-size:0.85rem}
  footer{margin-top:40px;color:#5a5a68;font-size:0.85rem;text-align:center}
</style></head><body><main>${bodyHtml}</main></body></html>`;
}

// ---------- routes ----------

// Normalizes a ?code= query value; returns "" unless the code exists in Redis.
async function validRefCode(raw) {
  const code = String(raw || "").toUpperCase().replace(/[^A-Z0-9]/g, "").slice(0, 16);
  if (!code || !bindingEnabled) return "";
  try {
    return (await redis(["GET", "refcode:" + code])) ? code : "";
  } catch { return ""; }
}

app.get("/", async (req, res) => {
  trackView(req, res, "home");
  // Streamer referral code (?code=NAME): lifetime drops to £8, and the
  // monthly card loses its 80p intro so promos never stack.
  const refCode = await validRefCode(req.query.code);
  const codeQS = refCode ? "&code=" + refCode : "";
  const refBanner = refCode
    ? `<div style="background:rgba(95,209,138,0.12);border:1px solid rgba(95,209,138,0.5);border-radius:10px;
         padding:10px 16px;margin-bottom:16px;color:#5fd18a;font-weight:600">
         Code ${refCode} applied - Pro Lifetime £8 instead of £10</div>`
    : "";
  const monthlyBtn = bindingEnabled
    ? (refCode
      ? `<div class="card">
           <div>Pro Monthly</div>
           <div class="price">£3<span>/month</span></div>
           <div class="muted" style="font-size:0.85rem;margin:-8px 0 12px">cancel anytime</div>
           <a class="btn ghost" href="/buy?plan=monthly${codeQS}">Start monthly</a>
         </div>`
      : `<div class="card">
         <div>Pro Monthly <span style="color:#5fd18a;font-size:0.8rem">· try it cheap</span></div>
         <div class="price">£0.80<span> first month</span></div>
         <div class="muted" style="font-size:0.85rem;margin:-8px 0 12px">then £3/month · cancel anytime</div>
         <a class="btn ghost" href="/buy?plan=monthly">Start for £0.80</a>
       </div>`)
    : "";

  // Honest social proof: real download count, only shown once it's meaningful
  // (never a fabricated number). Falls back to nothing while it's still small.
  let proof = "";
  if (bindingEnabled) {
    try {
      const dl = Number(await redis(["GET", "stats:event:download"]) || 0);
      if (dl >= 50) proof = `<p class="muted" style="text-align:center;margin-top:8px">Downloaded ${dl.toLocaleString()}+ times</p>`;
    } catch {}
  }

  res.send(
    page(
      "Antigravity Voice Engine - voice changer & soundboard",
      `${refBanner}<div class="hero">
         <div style="display:inline-block;background:rgba(138,116,255,0.14);border:1px solid rgba(138,116,255,0.45);
              color:#a794ff;border-radius:20px;padding:6px 16px;font-size:0.85rem;font-weight:600;margin-bottom:14px">
           NEW &middot; Now free to use - upgrade to Pro anytime &darr;</div>
         <h1 style="font-size:2.1rem;line-height:1.15">Change your voice live.<br>Free to use - Pro unlocks everything.</h1>
         <p class="tagline">A real-time voice changer <strong>and</strong> soundboard for Windows, in one
         low-latency app. Sound like a monster, a robot, or a chipmunk - and fire off sound effects -
         live in Discord, games, Zoom and OBS. No account, no subscription trap.</p>
         <a class="btn big" href="/download">Download for Windows - Free</a>
         ${proof}
         <p class="muted" style="margin-top:12px">Free forever: Reverb + Pitch Shifter and 2 soundboard clips.
         Unlock all 9 effects and unlimited sounds with a one-time £10 Pro key (or £0.80 first month, then £3/mo).
         Works with Discord, Zoom, OBS, and any game.</p>
       </div>

       <div style="display:flex;gap:10px;justify-content:center;flex-wrap:wrap;margin:24px 0">
         <span class="chip">✓ Free to use</span>
         <span class="chip">✓ No account needed</span>
         <span class="chip">✓ Upgrade anytime</span>
         <span class="chip">✓ 2 devices per Pro key</span>
       </div>

       <h2>See it in action</h2>
       <video controls preload="none" poster="/demo-poster.jpg" playsinline
              style="width:100%;border-radius:12px;border:1px solid #2e2e3e;display:block;background:#000">
         <source src="/demo.mp4" type="video/mp4">
         Your browser can't play this video - <a href="/demo.mp4">download it</a> instead.
       </video>
       <p class="muted" style="text-align:center;margin-top:8px">Live in-game voice chat, changed in real time.</p>

       <h2 id="v21">New: free tier + a completely redesigned app</h2>
       <p class="muted">The app is now free to use - try the real thing before you ever pay - on top of
       the all-new interface. Same ultra-low-latency engine, brand-new cockpit, crisp on high-DPI screens.</p>
       <img src="/ui-v21.png" alt="Antigravity Voice Engine - redesigned interface"
            style="width:100%;border-radius:12px;border:1px solid #2e2e3e;display:block" loading="lazy">
       <ul class="features" style="margin-top:14px">
         <li>Free tier - use it forever, upgrade to Pro anytime</li>
         <li>All-new dark interface with a fresh look and new logo</li>
         <li>Sharp text on high-DPI / scaled displays</li>
         <li>Resizable window with adjustable panels</li>
         <li>Effect chain shown as clear on/off cards</li>
         <li>Soundboard grid that adapts to your window size</li>
       </ul>
       <p class="muted">Already have a Pro key? Open the app and hit the green <strong>Update</strong> button -
       your key keeps working, nothing to re-buy.</p>

       <h2>Free vs Pro</h2>
       <div class="pricing" style="grid-template-columns:1fr 1fr">
         <div class="card" style="text-align:left">
           <div style="text-align:center;font-weight:600;margin-bottom:6px">Free</div>
           <ul class="features" style="grid-template-columns:1fr">
             <li>Reverb &amp; Pitch Shifter effects</li>
             <li>One-click pitch presets (Deep, Chipmunk…)</li>
             <li>2 soundboard clips with global hotkeys</li>
             <li>Ultra-low latency + guided setup</li>
             <li>No account, no time limit</li>
           </ul>
         </div>
         <div class="card" style="text-align:left;border-color:#8a74ff">
           <div style="text-align:center;font-weight:600;margin-bottom:6px;color:#a794ff">Pro</div>
           <ul class="features" style="grid-template-columns:1fr">
             <li><strong>Everything in Free, plus:</strong></li>
             <li>All 9 effects (AI noise remover, EQ, compressor, robot, distortion…)</li>
             <li>Unlimited soundboard clips</li>
             <li>Every future effect, included</li>
           </ul>
         </div>
       </div>

       <h2>Pricing</h2>
       <div class="pricing" style="grid-template-columns:1fr 1fr 1fr">
         <div class="card">
           <div>Free</div>
           <div class="price">£0<span> forever</span></div>
           <a class="btn ghost" href="/download">Download</a>
         </div>
         <div class="card" style="border-color:#8a74ff">
           <div>Pro Lifetime <span style="color:#5fd18a;font-size:0.8rem">· best value</span></div>
           ${refCode
             ? `<div class="price">£8<span> once · <s style="color:#5a5a68">£10</s></span></div>`
             : `<div class="price">£10<span> once</span></div>`}
           <a class="btn" href="/buy?plan=life${codeQS}">Buy lifetime</a>
         </div>
         ${monthlyBtn}
       </div>
       <p class="muted">Start free, upgrade whenever. Secure checkout via Mollie (card, PayPal &amp; more).
       After paying you get a Pro key - paste it into the app and everything unlocks. Keep it safe for reinstalls.</p>

       <h2>Installing (30 seconds)</h2>
       <div class="card" style="text-align:left">
         <p style="margin-top:0">Because we're a small indie studio, our installer isn't
         "big-corporation signed" yet, so <strong>Windows shows a blue SmartScreen warning</strong> the
         first time. It's safe - here's how to get past it:</p>
         <ol style="color:#c7c7d1;line-height:1.7;margin:0;padding-left:20px">
           <li>Run the downloaded <code>AntigravityVoiceEngine-Setup.exe</code></li>
           <li>On the blue screen, click <strong style="color:#8a74ff">More info</strong></li>
           <li>Then click <strong style="color:#8a74ff">Run anyway</strong></li>
           <li>Follow the wizard - it even installs the virtual mic driver for you</li>
         </ol>
       </div>

       <h2>How it works with Discord / games</h2>
       <p class="muted">The app takes your mic, applies the effects, and outputs to a virtual audio cable
       (installed automatically). In Discord/your game, just pick <strong>"CABLE Output"</strong> as your
       microphone - everyone now hears your new voice. The guided setup walks you through it on first launch.</p>

       <h2>Streamers: partner with us</h2>
       <div class="card" style="text-align:left">
         <p style="margin-top:0" class="muted">Stream on Twitch, YouTube, Kick or TikTok? Partner with
         Antigravity and get <strong style="color:#f2f2f7">a free lifetime Pro key</strong>,
         <strong style="color:#f2f2f7">10 Pro keys to drop to your viewers</strong>, and
         <strong style="color:#f2f2f7">your own creator code</strong> - your viewers get lifetime Pro
         for £8 instead of £10, and you earn £2.50 for every sale made with your code, paid out by
         PayPal. No contracts, no scripts, nothing to pay - ever.</p>
         <a class="btn" href="/partners">Become a partner →</a>
       </div>

       <div style="margin:36px 0 8px">
       <!-- BEGIN AADS AD UNIT 2447478 -->
       <div id="frame" style="width: 100%;margin: auto;position: relative; z-index: 99998;">
                 <iframe data-aa='2447478' src='//acceptable.a-ads.com/2447478/?size=Adaptive'
                                   style='border:0; padding:0; width:70%; height:auto; overflow:hidden;display: block;margin: auto'></iframe>
               </div>
       <!-- END AADS AD UNIT 2447478 -->
       </div>

       <footer>Antigravity Voice Engine · <a href="/download" style="color:#5a5a68">Download</a>
         · <a href="/partners" style="color:#5a5a68">Streamers</a>
         · <a href="/recover" style="color:#5a5a68">Lost key?</a>
         · <a href="/legal" style="color:#5a5a68">Terms &amp; Privacy</a></footer>`,
      { wide: true }
    )
  );
});

// Terms of Service + privacy / data notice. Concise and honest: a clear,
// findable disclosure is what actually keeps you compliant (GDPR/UK GDPR fine
// people for HIDING data use, not for a short plain-English notice).
app.get("/legal", (req, res) => {
  res.send(page("Terms & Privacy - Antigravity Voice Engine", `
    <h1>Terms &amp; Privacy</h1>
    <p class="muted">Last updated: 2026-07-17. Plain-English summary of how
    Antigravity Voice Engine works and what data it uses. Using the app or site means you accept this.</p>

    <h2>The basics</h2>
    <ul class="features" style="grid-template-columns:1fr">
      <li>The app is provided "as is", for lawful use. Don't use it to harass, impersonate, or break
          the rules of Discord, a game, or any platform.</li>
      <li>A licence key unlocks the app. One key works on up to ${DEVICE_LIMIT} machines. Don't resell
          or publicly share keys - shared keys can be revoked.</li>
      <li>Payments are handled by <a href="https://www.mollie.com" style="color:#8a74ff">Mollie</a>;
          we never see your card details. Lifetime keys are one-time; monthly keys stop working if the
          subscription ends.</li>
    </ul>

    <h2>What data we use, and why</h2>
    <p class="muted">We don't use Google Analytics, don't run trackers across other sites, and don't sell
    data. There are no user accounts. What we do collect:</p>
    <ul class="features" style="grid-template-columns:1fr">
      <li><strong>Website:</strong> a random visitor id (in a cookie), pages viewed, the site that referred
          you, and an approximate country (from your IP). Used only to see how many people visit and what
          works.</li>
      <li><strong>App:</strong> when it checks your licence, it sends the key, an <em>anonymous</em> one-way
          fingerprint of the PC (a hashed Windows ID - not reversible, not your name), your Windows and app
          version, and approximate country. This enforces the ${DEVICE_LIMIT}-device limit, stops key-sharing,
          and tells us which versions/systems to support.</li>
      <li><strong>Stability:</strong> if the app crashes, it may send the app version, OS, and an error code -
          no documents, audio, or personal files, ever. Your microphone audio is processed live on your PC
          and is <strong>never uploaded</strong>.</li>
      <li><strong>Install status:</strong> we record that a machine has the app installed (by that anonymous
          fingerprint) and, if you uninstall, that it was removed.</li>
      <li><strong>Ads:</strong> this website shows one ad banner served by
          <a href="https://a-ads.com" style="color:#8a74ff">A-ADS</a>; loading it means their server sees
          your IP address (we share nothing else with them). The free version of the app may also show an
          ad banner, fetched via our server. Pro has no ads.</li>
    </ul>

    <h2>Your choices</h2>
    <p class="muted">Uninstalling removes the app and stops all app data collection. Want your records deleted
    or have a question? Email the address on our store/checkout and we'll sort it. Because the machine
    fingerprint is anonymised, quote your licence key so we can find your entries.</p>

    <p style="margin-top:28px"><a class="btn" href="/">← Back</a></p>`, { wide: true }));
});

// Serves the installer built by package.ps1 (server/public/downloads/).
// Kept as a real file on disk (checked into the repo) rather than a
// redirect, so there's exactly one URL to give out that always works.
app.get("/download", (req, res) => {
  const file = path.join(__dirname, "public", "downloads", "AntigravityVoiceEngine-Setup.exe");
  if (!fs.existsSync(file)) {
    return res.status(404).send(page("Not available",
      `<h1>Download not available yet</h1><p class="muted">Check back soon.</p>`));
  }
  trackEvent(req, res, "download");
  res.download(file, "AntigravityVoiceEngine-Setup.exe");
});

app.get("/buy", async (req, res, next) => {
  try {
    // Every hit creates real Mollie objects (payments, and customers for the
    // monthly plan) - throttle so a bot can't pollute the Mollie account.
    // 8/hour per IP is far more than any genuine buyer needs.
    if (!rateAllow("buy", req, 8, 60 * 60 * 1000)) {
      return res.status(429).send(page("Slow down",
        `<h1>Too many checkout attempts</h1>
         <p class="muted">Please wait a bit and try again, or email us if you're stuck.</p>
         <a class="btn" href="/">← Back</a>`));
    }
    const plan = req.query.plan === "monthly" ? "monthly" : "life";
    const refCode = await validRefCode(req.query.code);
    trackEvent(req, res, "buy_click", { plan, ...(refCode ? { code: refCode } : {}) });

    if (plan === "monthly" && !bindingEnabled) {
      return res.status(503).send(page("Unavailable",
        `<h1>Monthly plan unavailable</h1><p>Subscriptions need the database
         (Upstash) configured. <a class="btn" href="/buy?plan=life">Buy lifetime instead</a></p>`));
    }

    // Carry the visitor id through Mollie so the completed purchase can be
    // tied back to the browsing session on /key (visit -> purchase link).
    const vid = visitorId(req, res);
    // Referral pricing: lifetime £8 with a valid code. Monthly with a code is
    // plain £3.00 first month (the 80p intro never stacks with the discount,
    // and only lifetime sales earn the streamer commission).
    let amount = PLANS[plan].amount;
    if (refCode) {
      amount = plan === "life"
        ? { currency: "GBP", value: REF_LIFE_PRICE }
        : PLANS.monthly.recurring;
    }
    let paymentBody = {
      amount,
      description: `${PRODUCT} (${PLANS[plan].label.toLowerCase()}${refCode ? ", code " + refCode : ""})`,
      redirectUrl: `${BASE_URL}/key`,
      webhookUrl: `${BASE_URL}/webhook`,
      metadata: refCode ? { vid, plan, code: refCode } : { vid, plan },
    };

    if (plan === "monthly") {
      // Recurring: a Mollie customer + "first" payment establishes the
      // mandate; the subscription itself is created once this is paid.
      const customer = await mollie("POST", "/customers", {
        name: "Antigravity customer",
      });
      paymentBody.customerId = customer.id;
      paymentBody.sequenceType = "first";
    }

    const payment = await mollie("POST", "/payments", paymentBody);
    // Point the redirect at this specific payment so /key can look it up.
    await mollie("PATCH", `/payments/${payment.id}`, {
      redirectUrl: `${BASE_URL}/key?pid=${payment.id}`,
    });
    res.redirect(payment._links.checkout.href);
  } catch (err) {
    next(err);
  }
});

// After a "first" payment is paid: create the Mollie subscription (starting
// next month - the first month is the payment just made) and open the key's
// paid-until window. Idempotent: safe to call from both /key and the webhook.
async function activateSubscription(payment) {
  const custId = payment.customerId;
  if (!custId) return;
  const key = makeKey(payment.id);

  const existing = await redis(["HGET", "sub:" + key, "subscriptionId"]);
  if (existing) return;

  const start = new Date(Date.now() + 30 * 24 * 3600 * 1000);
  const startDate = start.toISOString().slice(0, 10); // YYYY-MM-DD
  let subId = null;
  try {
    const sub = await mollie("POST", `/customers/${custId}/subscriptions`, {
      amount: PLANS.monthly.recurring, // £3/month ongoing
      interval: "1 month",
      startDate,
      description: `${PRODUCT} (monthly)`,
      webhookUrl: `${BASE_URL}/webhook`,
    });
    subId = sub.id;
  } catch (err) {
    // Both /key and the webhook can race here, or a previous run may have
    // created the subscription but failed to record it (e.g. Redis outage).
    // Mollie rejects the duplicate; recover the existing subscription's id.
    if (String(err.message).includes("already exists")) {
      const list = await mollie("GET", `/customers/${custId}/subscriptions`);
      const subs = (list._embedded && list._embedded.subscriptions) || [];
      const match = subs.find((s) => s.status === "active" || s.status === "pending");
      if (match) subId = match.id;
    }
    if (!subId) throw err;
  }

  // subscription itself charges the recurring (£3) amount monthly.
  // (The £0.80 first payment was the checkout above.)
  //   -- amount handled in the mollie() call below --
  await redis(["HSET", "sub:" + key,
    "plan", "monthly",
    "customerId", custId,
    "subscriptionId", subId,
    "paidUntil", Date.now() + SUB_GRACE_MS,
  ]);
  await redis(["SET", "cust:" + custId, key]); // recurring payments -> key
}

// Credits the referring streamer for a paid LIFETIME purchase made with their
// code. Callers must gate this behind the stats:purchased SADD (first-seen
// guard) so a payment can never be credited twice, whichever of /key or the
// webhook sees it first. Monthly purchases never earn commission by design.
async function creditReferral(payment) {
  const meta = payment.metadata || {};
  if (meta.plan !== "life" || !meta.code) return;
  const pid = await redis(["GET", "refcode:" + String(meta.code)]);
  if (!pid) return;
  await redis(["HINCRBYFLOAT", "partner:" + pid, "owed", REF_COMMISSION]);
  await redis(["HINCRBY", "partner:" + pid, "sales", 1]);
  console.log(`Referral sale: code ${meta.code} +£${REF_COMMISSION}`);
}

app.get("/key", async (req, res, next) => {
  try {
    const pid = String(req.query.pid || "");
    if (!/^[a-zA-Z0-9_]+$/.test(pid)) {
      return res.status(400).send(page("Invalid request", `<h1>Invalid request</h1>`));
    }
    const payment = await mollie("GET", `/payments/${pid}`);

    if (payment.status === "paid") {
      let subNote = "the key never expires.";
      if (payment.sequenceType === "first" && payment.customerId) {
        try {
          await activateSubscription(payment);
          subNote = "your subscription renews monthly; the key stops working if it lapses.";
        } catch (err) {
          console.error("subscription activation failed:", err.message);
        }
      }
      // Analytics: record the purchase and link it to the browsing session.
      // Guarded against double-counting if the buyer refreshes /key.
      if (bindingEnabled) {
        const key = makeKey(pid);
        const vid = (payment.metadata && payment.metadata.vid) || "";
        const plan = (payment.metadata && payment.metadata.plan) || "life";
        redis(["SADD", "stats:purchased", pid]).then(async (added) => {
          if (added === 1) {
            await Promise.all([
              redis(["INCR", "stats:event:purchase"]),
              // What the buyer actually paid (referral-discounted or full)
              redis(["INCRBYFLOAT", "stats:revenue",
                (payment.amount && payment.amount.value) || (PLANS[plan] ? PLANS[plan].amount.value : "0")]),
            ]);
            await creditReferral(payment).catch((e) => console.error("referral credit failed:", e.message));
            if (vid) {
              await Promise.all([
                redis(["HSET", "visitor:" + vid, "key", key, "plan", plan, "purchasedAt", new Date().toISOString()]),
                redis(["HSET", "keyvisitor:" + key, "vid", vid]), // key -> visitor
                // Collapse into one person: from now on they're tracked by key
                redis(["ZREM", "presence", "v:" + vid]),
                redis(["ZADD", "presence", String(Date.now()), "k:" + key]),
              ]);
            }
          }
        }).catch((e) => console.error("purchase track failed:", e.message));
      }
      return res.send(
        page(
          "Your license key",
          `<h1>Payment complete – thank you!</h1>
           <p>Your license key (click to select, then copy):</p>
           <code class="key">${makeKey(pid)}</code>
           <p>Open <strong>Antigravity Voice Engine</strong>, paste the key into the
           activation screen and press <strong>Activate</strong>.</p>
           <p class="muted"><strong>Save this key somewhere safe now</strong> (password manager,
           notes app) – this page won't be reachable forever, but ${subNote}</p>
           <p class="muted">Lose it anyway? This browser can get it back any time at
           <a href="/recover" style="color:#8a74ff">${BASE_URL.replace(/^https?:\/\//, "")}/recover</a>.</p>`
        )
      );
    }

    if (payment.status === "open" || payment.status === "pending") {
      return res.send(
        page(
          "Payment pending",
          `<h1>Payment not confirmed yet</h1>
           <p>Your payment is still <strong>${payment.status}</strong>. Wait a few seconds and
           <a class="btn" href="/key?pid=${pid}">refresh</a></p>`
        )
      );
    }

    res.send(
      page(
        "Payment not completed",
        `<h1>Payment ${payment.status}</h1>
         <p>The payment was not completed. No money was taken for failed or
         cancelled payments.</p>
         <a class="btn" href="/buy">Try again</a>`
      )
    );
  } catch (err) {
    next(err);
  }
});

// Lost-key recovery. The purchase flow stores visitor:<vid> -> key, and the
// vid cookie lives for 2 years, so the browser that bought can always get its
// own key back without accounts or email. Only that browser: the vid is a
// 64-bit random cookie value, not something a stranger can guess or look up.
app.get("/recover", async (req, res) => {
  if (!rateAllow("recover", req, 10, 60 * 60 * 1000)) {
    return res.status(429).send(page("Slow down",
      `<h1>Too many attempts</h1><p class="muted">Wait a bit and try again.</p>`));
  }
  trackView(req, res, "recover");

  const supportNote = `
    <h2>Different browser or PC?</h2>
    <p class="muted">Recovery works from the browser you bought in (it remembers the purchase).
    If you're somewhere else, email us via the address on your Mollie/PayPal payment receipt and
    quote the payment date and amount - we'll look your key up and send it back.</p>
    <p style="margin-top:24px"><a class="btn ghost" href="/">← Back</a></p>`;

  if (!bindingEnabled) {
    return res.send(page("Recover your key", `<h1>Recover your key</h1>
      <p class="muted">Automatic recovery isn't available right now.</p>${supportNote}`));
  }
  try {
    const m = /(?:^|;\s*)vid=([A-Za-z0-9]{16})/.exec(String(req.headers.cookie || ""));
    const vid = m ? m[1] : "";
    const key = vid ? await redis(["HGET", "visitor:" + vid, "key"]) : null;
    if (key) {
      return res.send(page("Your license key", `<h1>Found it!</h1>
        <p>This browser bought the following Pro key:</p>
        <code class="key">${key}</code>
        <p class="muted">Paste it into the app's activation screen. Save it somewhere safe this time -
        a password manager or a note works great.</p>
        <p style="margin-top:24px"><a class="btn ghost" href="/">← Back</a></p>`));
    }
    res.send(page("Recover your key", `<h1>No purchase found in this browser</h1>
      <p class="muted">We couldn't find a Pro purchase made from this browser (cookies may have
      been cleared).</p>${supportNote}`));
  } catch (err) {
    console.error("recover failed:", err.message);
    res.status(500).send(page("Recover your key", `<h1>Recovery temporarily unavailable</h1>
      <p class="muted">Please try again in a few minutes.</p>${supportNote}`));
  }
});

// ---------- streamer partner program ----------
//
// Inbound funnel: streamers apply here (linked from personal outreach emails
// and the site footer). Deliberately NOT an automated mass-mailer - outreach
// stays one-to-one from the owner's own mailbox (see docs/streamer-outreach.md);
// this page just makes saying yes a one-click affair, and the admin approve
// flow turns a request into keys + a pre-filled reply email.

const esc = (s) => String(s ?? "").replace(/[&<>"]/g, (c) =>
  ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));

app.get("/partners", (req, res) => {
  trackView(req, res, "partners");
  const form = bindingEnabled ? `
    <form method="POST" action="/partners" style="display:grid;gap:10px">
      <input required name="channel" maxlength="200" placeholder="Your channel link (Twitch / YouTube / TikTok / Kick)"
        style="padding:12px;border-radius:8px;border:1px solid #2e2e3e;background:#1c1c26;color:#f2f2f7">
      <input required type="email" name="email" maxlength="200" placeholder="Business email (where we send the keys)"
        style="padding:12px;border-radius:8px;border:1px solid #2e2e3e;background:#1c1c26;color:#f2f2f7">
      <input name="viewers" maxlength="40" placeholder="Typical live viewers (rough number, optional)"
        style="padding:12px;border-radius:8px;border:1px solid #2e2e3e;background:#1c1c26;color:#f2f2f7">
      <textarea name="message" maxlength="500" rows="3" placeholder="Anything else? (optional)"
        style="padding:12px;border-radius:8px;border:1px solid #2e2e3e;background:#1c1c26;color:#f2f2f7;font-family:inherit"></textarea>
      <button type="submit">Request partner keys</button>
    </form>`
    : `<p class="muted">Applications are briefly offline - email us via the address on our checkout instead.</p>`;

  res.send(page("Partner program - Antigravity Voice Engine", `
    <h1>Streamer partner program</h1>
    <p class="muted">Use Antigravity on stream, give your viewers free Pro keys. No contracts, no
    scripts to read, nothing to pay - ever.</p>
    <div class="card" style="text-align:left;margin:18px 0">
      <ul class="features" style="grid-template-columns:1fr">
        <li><strong>You get a free lifetime Pro key</strong> - every effect, forever.</li>
        <li><strong>Your viewers get 10 one-month Pro keys</strong> to give away however you like
            (chat drops, subs, contests - your call).</li>
        <li><strong>Your creator code earns you £2.50 per sale</strong> - viewers who buy through
            your link get lifetime Pro for £8 instead of £10, and we pay you out by PayPal
            (£20 minimum, no upper limit).</li>
        <li>No obligations: use it on stream if you enjoy it, that's the whole deal. A shout-out
            or a link in your description is appreciated, never required.</li>
      </ul>
      <p class="muted" style="font-size:0.85rem;margin-top:10px">The fine print: commission is earned on
      lifetime purchases made with your code; the discount and the £0.80 monthly intro don't stack.
      When you mention your code or link, UK advertising rules require you to disclose the
      partnership (a simple #ad is enough). Payouts monthly via PayPal once you're over £20.</p>
    </div>
    ${form}
    <p class="muted" style="margin-top:16px">We review every request by hand and reply by email,
    usually within a couple of days. <a href="/" style="color:#8a74ff">What is Antigravity?</a></p>`));
});

app.post("/partners", async (req, res) => {
  if (!rateAllow("partners", req, 3, 60 * 60 * 1000)) {
    return res.status(429).send(page("Slow down", `<h1>Too many requests</h1>
      <p class="muted">Please wait a while and try again.</p>`));
  }
  if (!bindingEnabled) return res.redirect("/partners");

  const channel = String(req.body.channel || "").trim().slice(0, 200);
  const email = String(req.body.email || "").trim().slice(0, 200);
  const viewers = String(req.body.viewers || "").trim().slice(0, 40);
  const message = String(req.body.message || "").trim().slice(0, 500);
  if (!channel || !/^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(email)) {
    return res.status(400).send(page("Invalid request", `<h1>Missing channel or valid email</h1>
      <p><a class="btn" href="/partners">← Back</a></p>`));
  }

  try {
    const id = crypto.randomBytes(6).toString("hex");
    await redis(["HSET", "partner:" + id,
      "channel", channel, "email", email, "viewers", viewers, "message", message,
      "at", new Date().toISOString(), "status", "pending"]);
    await redis(["SADD", "partners:all", id]);
    trackEvent(req, res, "partner_request", { channel: channel.slice(0, 60) });
    res.send(page("Request received", `<h1>Request received - thank you!</h1>
      <p class="muted">We review every application by hand and reply to
      <strong style="color:#f2f2f7">${esc(email)}</strong>, usually within a couple of days.</p>
      <a class="btn" href="/">← Back to the site</a>`));
  } catch (err) {
    console.error("partner request failed:", err.message);
    res.status(500).send(page("Error", `<h1>Something went wrong - please try again</h1>`));
  }
});

// Signature check for a key string (constant-time). Returns true/false.
function signatureValid(key) {
  const parsed = parseKey(key);
  if (!parsed) return false;
  const expected = Buffer.from(sign(parsed.paymentId));
  const got = Buffer.from(parsed.sig);
  return expected.length === got.length && crypto.timingSafeEqual(expected, got);
}

// Per-license usage profile ("user profile" keyed by license, no accounts
// needed). Fire-and-forget: analytics must never delay or fail a verify.
//
// The profile/install writes here are analytics, not correctness - the app
// re-verifies on every launch (and periodically), so writing ~7 commands each
// time is the biggest recurring Redis cost. They're throttled to once per
// device per day via a SET NX EX guard: repeat verifies within a day cost just
// that one guard command instead of the full write burst. touchPresence stays
// every verify so the "recently active" list remains live.
function recordProfile(req, key, device) {
  if (!bindingEnabled) return;
  touchPresence("k:" + key); // app activity feeds the same recent-active list (live)

  // Once-per-day-per-device gate. "OK" only for the first verify of the day.
  redis(["SET", "seen:" + device, "1", "NX", "EX", "86400"]).then((fresh) => {
    if (!fresh) return; // already recorded a profile/install for this device today

    const now = new Date().toISOString();
    // Deliberately NO raw IP here: the legal page promises app users are
    // tracked only by the anonymous fingerprint + country, the dashboard
    // never displays an IP, and not storing it keeps that promise true
    // (data minimization - can't leak or be subpoenaed what isn't kept).
    const country = String(req.headers["cf-ipcountry"] || req.headers["x-vercel-ip-country"] || "");
    const fields = [
      "lastSeen", now,
      "lastDevice", device || "",
      "os", String(req.query.os || "").slice(0, 64),
      "appVersion", String(req.query.v || "").slice(0, 32),
    ];
    if (country) fields.push("country", country.slice(0, 8));
    Promise.all([
      redis(["HSET", "profile:" + key, ...fields]),
      redis(["HSETNX", "profile:" + key, "firstSeen", now]),
      redis(["HINCRBY", "profile:" + key, "checks", 1]), // now counts active days
    ]).catch((err) => console.error("profile write failed:", err.message));

    // Per-machine install record: which PCs the app lives on, where, and whether
    // it's still there. Refreshes lastSeen and clears any prior "uninstalled"
    // flag (covers a reinstall on the same PC).
    const os = String(req.query.os || "").slice(0, 64);
    const ver = String(req.query.v || "").slice(0, 32);
    const hash = String(req.query.h || "").slice(0, 32);
    // Tamper flag: only meaningful once EXPECTED_HASH is set (per release) in
    // Render. A reported hash that differs is a modified/patched binary.
    const tampered = EXPECTED_HASH && hash && hash !== EXPECTED_HASH ? "1" : "0";
    Promise.all([
      redis(["SADD", "installs", device]),
      redis(["HSET", "install:" + device,
        "key", key, "os", os, "version", ver, "hash", hash, "tampered", tampered,
        "country", country.slice(0, 8), "lastSeen", now, "uninstalled", "0"]),
      redis(["HSETNX", "install:" + device, "firstSeen", now]),
    ]).catch((err) => console.error("install record failed:", err.message));

    // "When do they first use a key" - count each key's first-ever activation
    // once, and mark the linked visitor (if we know them) as converted. The
    // SADD is idempotent (added===1 only the first ever time), so gating it
    // behind the daily throttle keeps it correct while saving a command per
    // repeat verify.
    redis(["SADD", "stats:activated", key]).then(async (added) => {
      if (added !== 1) return;
      await redis(["INCR", "stats:event:key_activated"]);
      const vid = await redis(["HGET", "keyvisitor:" + key, "vid"]);
      if (vid) {
        await redis(["HSET", "visitor:" + vid, "activatedAt", now, "activatedOs", String(req.query.os || "").slice(0, 64)]);
      }
      const entry = JSON.stringify({ at: now, event: "key_activated", key, os: String(req.query.os || "").slice(0, 64) });
      await redis(["LPUSH", "analytics:events", entry]);
      await redis(["LTRIM", "analytics:events", "0", "299"]);
    }).catch((err) => console.error("activation track failed:", err.message));
  }).catch((err) => console.error("profile throttle failed:", err.message));
}

// Called by the desktop app. Query: key (required), device (fingerprint),
// os + v (analytics, optional).
//   { valid: true }                          -> unlock
//   { valid: false, reason: "invalid" }      -> bad/forged key
//   { valid: false, reason: "device_limit" } -> real key, too many devices
app.get("/api/verify", async (req, res) => {
  // The app verifies on launch and periodically - 60/hour per IP is generous
  // for real use but stops key-guessing floods and Redis-quota burn. The
  // over-limit body deliberately contains NO "valid" field: the app treats an
  // unrecognized response as a network error and falls back to offline grace,
  // so a throttled paying user is never locked out or has their key deleted.
  if (!rateAllow("verify", req, 60, 60 * 60 * 1000)) {
    return res.status(429).json({ error: "rate_limited" });
  }
  const key = req.query.key;
  if (!signatureValid(key)) {
    return res.json({ valid: false, reason: "invalid" });
  }

  const device = String(req.query.device || "").slice(0, 128);

  // No binding configured, or app didn't send a device id: signature-only.
  if (!bindingEnabled || !device) {
    return res.json({ valid: true });
  }

  try {
    // Owner revocation kills a key everywhere (stolen/shared/refunded). Checked
    // before anything else so a revoked key can never re-register or unlock.
    if ((await redis(["SISMEMBER", "keys:revoked", key])) === 1) {
      return res.json({ valid: false, reason: "revoked" });
    }

    // Time-boxed keys (monthly subscription or an admin-issued trial): reject
    // once the paid-until window has lapsed.
    const sub = flattenHash(await redis(["HGETALL", "sub:" + key]));
    const paidUntil = sub.paidUntil !== undefined ? Number(sub.paidUntil) : null;
    if (paidUntil !== null && Date.now() > paidUntil) {
      return res.json({ valid: false, reason: "expired" });
    }
    // Tell the app which plan this is so it can show it next to Reset.
    const plan = sub.plan || (paidUntil === null ? "lifetime" : "monthly");
    const planFields = paidUntil === null
      ? { plan }
      : { plan, paidUntil: new Date(paidUntil).toISOString() };

    const setKey = "lic:" + key;
    // Already activated on this device? Always allow.
    if ((await redis(["SISMEMBER", setKey, device])) === 1) {
      await redis(["SADD", "keys:used", key]);
      recordProfile(req, key, device);
      return res.json({ valid: true, ...planFields });
    }
    // New device: allow only if under the per-key limit.
    const count = await redis(["SCARD", setKey]);
    if (count < DEVICE_LIMIT) {
      await redis(["SADD", setKey, device]);
      await redis(["SADD", "keys:used", key]); // permanent used-key registry
      recordProfile(req, key, device);
      return res.json({ valid: true, ...planFields });
    }
    return res.json({ valid: false, reason: "device_limit" });
  } catch (err) {
    // Redis outage: don't punish a paying user whose key is math-valid.
    console.error(err);
    return res.json({ valid: true, degraded: true });
  }
});

// Called by the app's "Reset License Key" button: frees this device's slot so
// the key can be activated on another machine. The key stays in the permanent
// keys:used registry - releasing never makes a key look unused.
// Called by the app's "Unbind All Devices" button: possession of a valid key
// authorizes wiping that key's own device slots (same trust model as
// activating it). The app re-verifies right after, re-registering this PC.
app.get("/api/unbindall", async (req, res) => {
  if (!rateAllow("unbind", req, 10, 60 * 60 * 1000)) {
    return res.status(429).json({ cleared: false, reason: "rate_limited" });
  }
  const key = req.query.key;
  if (!signatureValid(key)) {
    return res.json({ cleared: false, reason: "invalid" });
  }
  if (!bindingEnabled) {
    return res.json({ cleared: true });
  }
  try {
    await redis(["DEL", "lic:" + key]);
    res.json({ cleared: true });
  } catch (err) {
    console.error(err);
    res.json({ cleared: false, reason: "error" });
  }
});

app.get("/api/release", async (req, res) => {
  if (!rateAllow("release", req, 10, 60 * 60 * 1000)) {
    return res.status(429).json({ released: false, reason: "rate_limited" });
  }
  const key = req.query.key;
  if (!signatureValid(key)) {
    return res.json({ released: false, reason: "invalid" });
  }
  const device = String(req.query.device || "").slice(0, 128);
  if (!bindingEnabled || !device) {
    return res.json({ released: true });
  }
  try {
    await redis(["SREM", "lic:" + key, device]);
    return res.json({ released: true });
  } catch (err) {
    console.error(err);
    return res.json({ released: false, reason: "error" });
  }
});

// ---------- owner-only admin (set ADMIN_SECRET in Render to enable) ----------

const ADMIN_SECRET = process.env.ADMIN_SECRET || "";

function safeEqual(a, b) {
  return a.length === b.length &&
         crypto.timingSafeEqual(Buffer.from(a), Buffer.from(b));
}

// Session cookie: "adm=<expiryMs>.<HMAC(expiryMs)>", valid 7 days
function makeSessionCookie() {
  const exp = String(Date.now() + 7 * 24 * 3600 * 1000);
  const sig = crypto.createHmac("sha256", LICENSE_SECRET).update("adm" + exp).digest("hex");
  return `adm=${exp}.${sig}; Path=/; HttpOnly; Secure; SameSite=Lax; Max-Age=${7 * 24 * 3600}`;
}

function hasValidSession(req) {
  const m = /(?:^|;\s*)adm=(\d+)\.([a-f0-9]{64})/.exec(String(req.headers.cookie || ""));
  if (!m || Date.now() > Number(m[1])) return false;
  const expected = crypto.createHmac("sha256", LICENSE_SECRET).update("adm" + m[1]).digest("hex");
  return safeEqual(m[2], expected);
}

function adminAuthorized(req) {
  if (ADMIN_SECRET.length === 0) return false;
  // Authorization is by signed session cookie only (obtained via POST /login).
  // The admin secret is deliberately NOT accepted as a query parameter: URLs
  // leak into server logs, browser history and the Referer header.
  return hasValidSession(req);
}

// CSRF guard for state-changing admin routes. These are GET links inside the
// dashboard, so they must be same-origin. A forged cross-site navigation
// (malicious link/img/redirect) carries Sec-Fetch-Site: cross-site and is
// rejected; genuine dashboard clicks are same-origin. Falls back to a Referer
// host check for browsers that don't send Sec-Fetch-Site.
function sameOriginRequest(req) {
  const site = req.headers["sec-fetch-site"];
  if (site) return site === "same-origin" || site === "same-site" || site === "none";
  const ref = String(req.headers["referer"] || "");
  if (!ref) return false;
  try {
    return new URL(ref).host === new URL(BASE_URL).host;
  } catch {
    return false;
  }
}

// Combined gate for admin routes that mutate state: valid session AND a
// same-origin request. Sends the right rejection and returns false if not.
function adminMutationAllowed(req, res) {
  if (!adminAuthorized(req)) {
    res.status(403).json({ error: "forbidden" });
    return false;
  }
  if (!sameOriginRequest(req)) {
    res.status(403).json({ error: "cross-site request blocked" });
    return false;
  }
  return true;
}

// Dashboard login: password form -> signed session cookie
app.get("/login", (req, res) => {
  res.send(page("Login", `<h1>Dashboard login</h1>
    <form method="POST" action="/login">
      <p><input type="password" name="password" placeholder="Admin password" autofocus
         style="padding:12px;border-radius:8px;border:1px solid #2e2e3e;background:#1c1c26;color:#f2f2f7;width:100%"></p>
      <p><button type="submit">Sign in</button></p>
    </form>
    <p class="muted">Password = the ADMIN_SECRET environment variable on the server.</p>`));
});

app.post("/login", (req, res) => {
  // 5 attempts per 15 minutes per IP stops password brute-forcing while
  // leaving plenty of room for a fat-fingered real login.
  if (!rateAllow("login", req, 5, 15 * 60 * 1000)) {
    return res.status(429).send(page("Too many attempts",
      `<h1>Too many login attempts</h1>
       <p class="muted">Wait 15 minutes and try again.</p>`));
  }
  const pw = String(req.body.password || "");
  if (ADMIN_SECRET.length > 0 && safeEqual(pw, ADMIN_SECRET)) {
    res.setHeader("Set-Cookie", makeSessionCookie());
    return res.redirect("/admin");
  }
  res.status(403).send(page("Login", `<h1>Wrong password</h1><p><a class="btn" href="/login">Try again</a></p>`));
});

// Builds the "most recently active" people list from the presence sorted set,
// resolving each entry to a human label (key user vs web visitor) and marking
// who's active in the last 5 minutes ("online now").
async function collectPresence(limit = 60) {
  const flat = (await redis(["ZREVRANGE", "presence", "0", String(limit - 1), "WITHSCORES"])) || [];
  const now = Date.now();
  const people = [];
  for (let i = 0; i + 1 < flat.length; i += 2) {
    const member = flat[i];
    const ts = Number(flat[i + 1]);
    const online = now - ts < 5 * 60 * 1000;
    const ago = Math.max(0, Math.round((now - ts) / 1000));

    if (member.startsWith("k:")) {
      const key = member.slice(2);
      const p = flattenHash(await redis(["HGETALL", "profile:" + key]));
      const sub = flattenHash(await redis(["HGETALL", "sub:" + key]));
      const plan = sub.plan || "lifetime";
      people.push({
        online, ts, ago,
        type: "App",
        id: key,
        label: key.slice(0, 20) + "…",
        plan,
        detail: [p.os, p.appVersion ? "v" + p.appVersion : "", p.country].filter(Boolean).join(" · "),
      });
    } else if (member.startsWith("v:")) {
      const vid = member.slice(2);
      const v = flattenHash(await redis(["HGETALL", "visitor:" + vid]));
      const f = visitorFunnel(v);
      const dur = v.firstSeen && v.lastSeen ? humanDuration(v.firstSeen, v.lastSeen) : "";
      people.push({
        online, ts, ago,
        type: "Web",
        id: vid,
        label: "Visitor " + vid.slice(0, 8),
        plan: f.stage, planColor: f.color,
        detail: [v.ref && v.ref !== "direct" ? "via " + v.ref : "direct",
                 v.country, (v.views || "1") + " views",
                 dur && dur !== "0s" ? dur + " on site" : ""].filter(Boolean).join(" · "),
      });
    }
  }
  return people;
}

// Every machine the app has been installed on, newest activity first, with a
// live status (online / active / idle / uninstalled).
async function collectInstalls(limit = 200) {
  const devices = (await redis(["SMEMBERS", "installs"])) || [];
  const now = Date.now();
  const out = [];
  for (const d of devices.slice(0, limit)) {
    const h = flattenHash(await redis(["HGETALL", "install:" + d]));
    const last = h.lastSeen ? Date.parse(h.lastSeen) : 0;
    const ageMs = now - last;
    let status;
    if (h.uninstalled === "1") status = "uninstalled";
    else if (ageMs < 5 * 60 * 1000) status = "online";
    else if (ageMs < 7 * 24 * 3600 * 1000) status = "active";
    else status = "idle"; // not seen in 7+ days - possibly removed without a ping
    out.push({
      device: d, status,
      os: h.os || "", version: h.version || "", country: h.country || "",
      key: h.key || "", firstSeen: h.firstSeen || "", lastSeen: h.lastSeen || "",
      uninstalledAt: h.uninstalledAt || "", last,
      tampered: h.tampered === "1",
    });
  }
  out.sort((a, b) => b.last - a.last);
  return out;
}

async function collectKeyData() {
  const keys = (await redis(["SMEMBERS", "keys:used"])) || [];
  const out = [];
  for (const k of keys.slice(0, 500)) {
    const p = flattenHash(await redis(["HGETALL", "profile:" + k]));

    // Plan/status from the subscription record (absent sub hash = lifetime)
    const sub = flattenHash(await redis(["HGETALL", "sub:" + k]));
    const paidUntil = sub.paidUntil !== undefined ? Number(sub.paidUntil) : null;
    const plan = sub.plan || (paidUntil === null ? "lifetime" : "monthly");
    const revoked = (await redis(["SISMEMBER", "keys:revoked", k])) === 1;
    const status =
      revoked ? "revoked"
      : paidUntil === null ? "active"
      : Date.now() > paidUntil ? "expired" : "active";

    out.push({
      key: k,
      plan,
      status,
      revoked,
      paidUntil: paidUntil === null ? "" : new Date(paidUntil).toISOString(),
      activeDevices: await redis(["SCARD", "lic:" + k]),
      ...p,
    });
  }
  return { count: keys.length, keys: out };
}

// Issues a new key without a Mollie payment (giveaways, trials, support
// comps). days=0 -> lifetime; days>0 -> expires after that many days and is
// tagged plan "trial" so the dashboard/app can tell it apart from a paid sub.
async function generateKey({ days, note }) {
  const id = "admin_" + crypto.randomBytes(9).toString("hex");
  const key = makeKey(id);
  const now = new Date().toISOString();

  if (days > 0) {
    await redis(["HSET", "sub:" + key, "plan", "trial", "paidUntil", Date.now() + days * 86400000]);
  }
  // Recorded immediately (not just on first activation) so it shows up on
  // the dashboard right away, even before anyone has used it.
  await redis(["SADD", "keys:used", key]);
  await redis(["HSET", "profile:" + key, "source", "admin", "note", note, "firstSeen", now]);
  return key;
}

// JSON view of every key ever activated + its usage profile.
app.get("/api/admin/keys", async (req, res) => {
  if (!adminAuthorized(req)) return res.status(403).json({ error: "forbidden" });
  if (!bindingEnabled) {
    return res.json({ count: 0, keys: [], note: "device binding disabled - nothing tracked" });
  }
  try {
    res.json(await collectKeyData());
  } catch (err) {
    console.error(err);
    res.status(500).json({ error: "redis error" });
  }
});

// Device manager: wipe all device slots for a key (e.g. customer got a new PC
// and lost access to the old ones). The key stays in the used registry.
app.get("/api/admin/clear", async (req, res) => {
  if (!adminMutationAllowed(req, res)) return;
  const key = String(req.query.key || "");
  if (!signatureValid(key)) return res.status(400).json({ error: "invalid key" });
  try {
    await redis(["DEL", "lic:" + key]);
    if (req.query.back) return res.redirect("/admin"); // session cookie carries auth
    res.json({ cleared: true });
  } catch (err) {
    console.error(err);
    res.status(500).json({ error: "redis error" });
  }
});

// Revoke a key (owner only): kills it everywhere and frees its device slots.
// Works on any plan - lifetime, monthly or trial. Use for stolen/shared keys.
app.get("/api/admin/revoke", async (req, res) => {
  if (!adminMutationAllowed(req, res)) return;
  const key = String(req.query.key || "");
  if (!signatureValid(key)) return res.status(400).json({ error: "invalid key" });
  try {
    await redis(["SADD", "keys:revoked", key]);
    await redis(["DEL", "lic:" + key]); // boot every device immediately
    if (req.query.back) return res.redirect("/admin");
    res.json({ revoked: true });
  } catch (err) {
    console.error(err);
    res.status(500).json({ error: "redis error" });
  }
});

// Un-revoke (owner only): re-enable a key you revoked by mistake.
app.get("/api/admin/unrevoke", async (req, res) => {
  if (!adminMutationAllowed(req, res)) return;
  const key = String(req.query.key || "");
  if (!signatureValid(key)) return res.status(400).json({ error: "invalid key" });
  try {
    await redis(["SREM", "keys:revoked", key]);
    if (req.query.back) return res.redirect("/admin");
    res.json({ unrevoked: true });
  } catch (err) {
    console.error(err);
    res.status(500).json({ error: "redis error" });
  }
});

// Delete a key from the dashboard entirely (owner only): revokes it (so it
// can never work again even if it re-verifies) AND removes its records from
// every list, so it disappears from the dashboard.
app.get("/api/admin/delete", async (req, res) => {
  if (!adminMutationAllowed(req, res)) return;
  const key = String(req.query.key || "");
  if (!signatureValid(key)) return res.status(400).json({ error: "invalid key" });
  try {
    await redis(["SADD", "keys:revoked", key]); // stays dead even if it pings again
    await redis(["SREM", "keys:used", key]);
    await redis(["DEL", "lic:" + key]);
    await redis(["DEL", "sub:" + key]);
    await redis(["DEL", "profile:" + key]);
    if (req.query.back) return res.redirect("/admin");
    res.json({ deleted: true });
  } catch (err) {
    console.error(err);
    res.status(500).json({ error: "redis error" });
  }
});

// Issues a giveaway/trial key (owner only) and sends the admin back to the
// dashboard with it highlighted so it can be copied immediately.
app.get("/admin/generate", async (req, res) => {
  if (!adminAuthorized(req)) return res.redirect("/login");
  if (!sameOriginRequest(req)) return res.status(403).send(page("Blocked",
    `<h1>Cross-site request blocked</h1><p><a class="btn" href="/admin">← Back</a></p>`));
  if (!bindingEnabled) return res.redirect("/admin");
  const days = Math.max(0, Math.min(3650, parseInt(req.query.days || "0", 10) || 0));
  const note = String(req.query.note || "").slice(0, 80);
  try {
    const key = await generateKey({ days, note });
    res.redirect("/admin?newkey=" + encodeURIComponent(key));
  } catch (err) {
    console.error(err);
    res.redirect("/admin");
  }
});

// Approve a partner request: generate 1 lifetime key for the streamer plus a
// batch of 30-day viewer keys, store them on the request (idempotent - a
// second visit just re-shows them), and hand back a pre-filled mailto: reply
// so the owner reviews and sends every email personally from their own
// mailbox. Deliberately no server-side sending: outreach stays one-to-one.
app.get("/admin/partner/approve", async (req, res) => {
  if (!adminAuthorized(req)) return res.redirect("/login");
  if (!sameOriginRequest(req)) return res.status(403).send(page("Blocked",
    `<h1>Cross-site request blocked</h1><p><a class="btn" href="/admin">← Back</a></p>`));
  if (!bindingEnabled) return res.redirect("/admin");
  const id = String(req.query.id || "");
  if (!/^[a-f0-9]{12}$/.test(id)) return res.redirect("/admin");

  try {
    const p = flattenHash(await redis(["HGETALL", "partner:" + id]));
    if (!p.email) return res.redirect("/admin");

    let skey = p.skey || "";
    let vkeys = p.vkeys ? p.vkeys.split(",") : [];
    let code = p.code || "";
    if (!skey) {
      const label = p.channel.slice(0, 60);
      skey = await generateKey({ days: 0, note: "partner: " + label });
      vkeys = [];
      for (let i = 0; i < 10; ++i) {
        vkeys.push(await generateKey({ days: 30, note: "viewers: " + label }));
      }
      // Referral code from the channel name (last URL segment), e.g.
      // twitch.tv/CoolStreamer -> COOLSTREAMER. Random digits on collision.
      const seg = p.channel.replace(/\/+$/, "").split("/").pop() || "";
      let base = seg.toUpperCase().replace(/[^A-Z0-9]/g, "").slice(0, 12) || "STREAM";
      code = base;
      while (await redis(["GET", "refcode:" + code])) {
        code = base.slice(0, 10) + Math.floor(10 + Math.random() * 90);
      }
      await redis(["SET", "refcode:" + code, id]);
      await redis(["HSET", "partner:" + id,
        "status", "approved", "skey", skey, "vkeys", vkeys.join(","), "code", code,
        "approvedAt", new Date().toISOString()]);
    }

    const bodyLines = [
      "Hey!",
      "",
      "Thanks for applying to the Antigravity partner program - you're in.",
      "",
      "Your personal lifetime Pro key (paste it into the app's activation screen):",
      "  " + skey,
      "",
      "And 10 one-month Pro keys for your viewers - drop them in chat, run a",
      "contest, whatever works for your stream:",
      ...vkeys.map((k) => "  " + k),
      "",
      "You also have a creator code: " + code,
      "Viewers who buy through your link get lifetime Pro for GBP 8 instead of",
      "GBP 10, and you earn GBP 2.50 for every lifetime sale:",
      "  " + BASE_URL + "/?code=" + code,
      "We track your sales and pay out by PayPal once you're over GBP 20 -",
      "just reply with your PayPal email whenever you like.",
      "",
      "One legal note: when you mention the code or link on stream or in your",
      "description, UK advertising rules require disclosing it's a partnership",
      "(a simple #ad or 'partner link' next to it is enough).",
      "",
      "Download: " + BASE_URL + "/download",
      "",
      "No obligations - enjoy it, and if it gets a laugh on stream, a link in",
      "your description would mean a lot. Any problems, just reply to this email.",
      "",
      "Have fun!",
    ].join("\n");
    const mailto = "mailto:" + encodeURIComponent(p.email)
      + "?subject=" + encodeURIComponent("Your Antigravity partner keys")
      + "&body=" + encodeURIComponent(bodyLines);

    res.send(page("Partner approved", `
      <h1>Partner approved</h1>
      <p class="muted">${esc(p.channel)} · ${esc(p.email)}</p>
      <p>Streamer lifetime key:</p>
      <code class="key">${esc(skey)}</code>
      <p>Viewer giveaway keys (30 days each):</p>
      <code class="key" style="font-size:0.85rem">${vkeys.map(esc).join("<br>")}</code>
      <p>Creator code (lifetime £8 for viewers, £${REF_COMMISSION}/sale to the streamer):</p>
      <code class="key">${esc(code)} · ${BASE_URL}/?code=${esc(code)}</code>
      <p><a class="btn" href="${mailto}">Open reply email (review &amp; send)</a>
         <a class="btn ghost" href="/admin">← Dashboard</a></p>
      <p class="muted">The email opens in your own mail app - nothing is sent automatically.</p>`,
      { wide: true }));
  } catch (err) {
    console.error("partner approve failed:", err.message);
    res.redirect("/admin");
  }
});

// Records a commission payout: moves the partner's owed balance into their
// lifetime paid-out total. Click AFTER actually sending the PayPal payment -
// this is bookkeeping, no money moves here (payouts stay manual on purpose).
app.get("/admin/partner/paid", async (req, res) => {
  if (!adminAuthorized(req)) return res.redirect("/login");
  if (!sameOriginRequest(req)) return res.status(403).send(page("Blocked",
    `<h1>Cross-site request blocked</h1><p><a class="btn" href="/admin">← Back</a></p>`));
  const id = String(req.query.id || "");
  if (/^[a-f0-9]{12}$/.test(id)) {
    try {
      const owed = parseFloat(await redis(["HGET", "partner:" + id, "owed"]) || "0");
      if (owed > 0) {
        await redis(["HINCRBYFLOAT", "partner:" + id, "paidOut", String(owed)]);
        await redis(["HSET", "partner:" + id, "owed", "0", "lastPaidAt", new Date().toISOString()]);
      }
    } catch (err) { console.error("partner paid failed:", err.message); }
  }
  res.redirect("/admin");
});

// Remove a partner request from the list (spam / not a fit). Any keys already
// generated for it stay valid - revoke them from the key table if needed.
app.get("/admin/partner/dismiss", async (req, res) => {
  if (!adminAuthorized(req)) return res.redirect("/login");
  if (!sameOriginRequest(req)) return res.status(403).send(page("Blocked",
    `<h1>Cross-site request blocked</h1><p><a class="btn" href="/admin">← Back</a></p>`));
  const id = String(req.query.id || "");
  if (/^[a-f0-9]{12}$/.test(id)) {
    try {
      await redis(["SREM", "partners:all", id]);
      await redis(["DEL", "partner:" + id]);
    } catch (err) { console.error("partner dismiss failed:", err.message); }
  }
  res.redirect("/admin");
});

const PLAN_COLORS = { lifetime: "#f2f2f7", monthly: "#38b0f8", trial: "#c99cf1" };

// Per-visitor journey: exactly what one person did on the site, in order.
app.get("/admin/visitor/:vid", async (req, res) => {
  if (!adminAuthorized(req)) return res.redirect("/login");
  const vid = String(req.params.vid || "").slice(0, 64);
  const esc = (s) => String(s ?? "").replace(/[&<>"]/g, (c) =>
    ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));
  try {
    const v = flattenHash(await redis(["HGETALL", "visitor:" + vid]));
    if (!v.firstSeen) {
      return res.send(page("Visitor", `<h1>Unknown visitor</h1>
        <p><a class="btn" href="/admin">← Back</a></p>`));
    }
    const raw = (await redis(["LRANGE", "journey:" + vid, "0", "49"])) || [];
    const f = visitorFunnel(v);
    const dur = v.firstSeen && v.lastSeen ? humanDuration(v.firstSeen, v.lastSeen) : "0s";

    const pretty = { view: "viewed", event: "" };
    const evLabel = { download: "clicked DOWNLOAD", buy_click: "clicked BUY", key_activated: "activated a key" };
    const steps = raw.map((r) => {
      let j = {}; try { j = JSON.parse(r); } catch {}
      const when = (j.at || "").replace("T", " ").slice(0, 19);
      if (j.t === "event") {
        const lbl = evLabel[j.e] || j.e;
        return `<tr><td class="muted">${esc(when)}</td>
          <td style="color:#38b0f8;font-weight:600">${esc(lbl)}</td></tr>`;
      }
      return `<tr><td class="muted">${esc(when)}</td><td>viewed <strong>${esc(j.p || "page")}</strong></td></tr>`;
    }).join("");

    res.send(page("Visitor " + vid.slice(0, 8), `
      <p><a href="/admin" style="color:#5a5a68">← Dashboard</a></p>
      <h1>Visitor ${esc(vid.slice(0, 8))}</h1>
      <p><span style="color:${f.color};font-weight:600">${esc(f.stage)}</span>
         <span class="muted">· ${esc(v.country || "?")} ·
         ${esc(v.ref && v.ref !== "direct" ? "via " + v.ref : "direct")} ·
         ${esc(v.views || "1")} views · ${esc(dur)} on site</span></p>

      <div class="card" style="margin:16px 0">
        <div style="display:flex;gap:24px;flex-wrap:wrap">
          <div><div class="muted" style="font-size:0.8rem">First seen</div>${esc((v.firstSeen || "").replace("T", " ").slice(0, 16))}</div>
          <div><div class="muted" style="font-size:0.8rem">Last seen</div>${esc((v.lastSeen || "").replace("T", " ").slice(0, 16))}</div>
          <div><div class="muted" style="font-size:0.8rem">Downloaded</div>${Number(v.ev_download || 0) > 0 ? "✓ yes" : "no"}</div>
          <div><div class="muted" style="font-size:0.8rem">Clicked buy</div>${Number(v.ev_buy_click || 0) > 0 ? "✓ yes" : "no"}</div>
          <div><div class="muted" style="font-size:0.8rem">Bought</div>${(v.activatedAt || Number(v.ev_purchase || 0) > 0) ? "✓ yes" : "no"}</div>
        </div>
      </div>

      <h2>Journey (newest first)</h2>
      <div style="overflow-x:auto"><table style="width:100%;border-collapse:collapse;font-size:0.85rem">
        <tr style="text-align:left;color:#9a9aa5"><th>When (UTC)</th><th>What</th></tr>
        ${steps || `<tr><td colspan="2" class="muted">No steps recorded.</td></tr>`}
      </table></div>
      <style>td,th{padding:7px 10px;border-bottom:1px solid #2e2e3e}</style>`, { wide: true }));
  } catch (e) {
    console.error("visitor page failed:", e.message);
    res.status(500).send(page("Error", `<h1>Error loading visitor</h1>`));
  }
});

// Human-friendly dashboard: plans, status, devices, last login, OS, version.
app.get("/admin", async (req, res) => {
  if (!adminAuthorized(req)) {
    return res.redirect("/login");
  }
  if (!bindingEnabled) {
    return res.send(page("Admin", `<h1>License dashboard</h1>
      <p>Device binding is disabled (no Upstash configured) - nothing is tracked.</p>`));
  }
  try {
    const data = await collectKeyData();
    const esc = (s) => String(s ?? "").replace(/[&<>"]/g, (c) =>
      ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));
    const paid = data.keys.filter((k) => k.source !== "admin");
    const monthly = data.keys.filter((k) => k.plan === "monthly");
    const trials = data.keys.filter((k) => k.plan === "trial");
    const activeSubs = monthly.filter((k) => k.status === "active").length;

    const newKey = String(req.query.newkey || "");
    const newKeyBanner = newKey
      ? `<div class="card" style="border-color:#38b0f8;margin-bottom:20px">
           <div class="muted" style="margin-bottom:8px">New key generated - copy it now:</div>
           <code class="key">${esc(newKey)}</code>
         </div>`
      : "";

    const enc = (k) => encodeURIComponent(k);
    const rows = data.keys
      .sort((a, b) => String(b.lastSeen || "").localeCompare(String(a.lastSeen || "")))
      .map((k) => `<tr${k.revoked ? ' style="opacity:0.55"' : ""}>
        <td><code>${esc(k.key)}</code></td>
        <td style="color:${PLAN_COLORS[k.plan] || "#f2f2f7"}">${esc(k.plan)}</td>
        <td style="color:${k.status === "active" ? "#5fd18a" : "#f47272"}">${esc(k.status)}${
          k.paidUntil ? `<br><span class="muted" style="font-size:0.75rem">until ${esc(k.paidUntil.slice(0, 10))}</span>` : ""}</td>
        <td>${esc(k.activeDevices)}/${DEVICE_LIMIT}</td>
        <td>${esc((k.lastSeen || "").replace("T", " ").slice(0, 16))}</td>
        <td>${esc(k.os || "")}</td>
        <td>${esc(k.appVersion || "")}</td>
        <td>${esc(k.country || "")}</td>
        <td>${k.source === "admin" ? `Given away${k.note ? ` <span class="muted">(${esc(k.note)})</span>` : ""}` : "Purchased"}</td>
        <td>${esc(k.checks || 0)}</td>
        <td style="white-space:nowrap">
          <a href="/api/admin/clear?key=${enc(k.key)}&back=1"
             onclick="return confirm('Free this key\\'s device slots? The owner can then re-activate on a new PC.')">clear devices</a>
          ${k.revoked
            ? ` · <a style="color:#5fd18a" href="/api/admin/unrevoke?key=${enc(k.key)}&back=1"
                   onclick="return confirm('Re-enable this key?')">un-revoke</a>`
            : ` · <a href="/api/admin/revoke?key=${enc(k.key)}&back=1"
                   onclick="return confirm('REVOKE this key? It stops working everywhere immediately (use for stolen/shared keys).')">revoke</a>`}
          · <a href="/api/admin/delete?key=${enc(k.key)}&back=1"
               onclick="return confirm('DELETE this key permanently? It is revoked and removed from the list. This cannot be undone.')">delete</a>
        </td>
      </tr>`).join("");
    res.send(page("License dashboard", `
      <h1>License dashboard</h1>
      <p class="muted">
        <strong style="color:#f2f2f7">${data.count}</strong> key(s) total ·
        <strong style="color:#f2f2f7">${paid.length}</strong> purchased ·
        <strong style="color:#f2f2f7">${activeSubs}</strong> active subscription(s) ·
        <strong style="color:#f2f2f7">${trials.length}</strong> trial/giveaway key(s)
      </p>

      ${newKeyBanner}

      <div class="card" style="margin-bottom:24px;text-align:left">
        <h2 style="margin-top:0">Give away a key</h2>
        <form method="GET" action="/admin/generate" style="display:flex;gap:10px;flex-wrap:wrap;align-items:center">
          <select name="days" style="padding:10px;border-radius:6px;background:#1c1c26;color:#fff;border:1px solid #2e2e3e">
            <option value="0">Lifetime</option>
            <option value="1">1 day</option>
            <option value="7" selected>1 week</option>
            <option value="30">1 month</option>
          </select>
          <input type="text" name="note" placeholder="Note (optional, e.g. a streamer's name)"
            style="padding:10px;border-radius:6px;background:#1c1c26;color:#fff;border:1px solid #2e2e3e;flex:1;min-width:180px">
          <button type="submit">Generate key</button>
        </form>
      </div>

      ${await (async () => {
        try {
          const ids = (await redis(["SMEMBERS", "partners:all"])) || [];
          if (!ids.length) return "";
          const reqs = [];
          for (const id of ids.slice(0, 100)) {
            const p = flattenHash(await redis(["HGETALL", "partner:" + id]));
            if (p.email) reqs.push({ id, ...p });
          }
          reqs.sort((a, b) => String(b.at || "").localeCompare(String(a.at || "")));
          const pending = reqs.filter((r) => r.status !== "approved").length;
          const totalOwed = reqs.reduce((s, r) => s + parseFloat(r.owed || "0"), 0);
          const rows = reqs.map((r) => {
            const owed = parseFloat(r.owed || "0");
            return `<tr${r.status === "approved" ? "" : ' style="opacity:0.75"'}>
            <td>${esc((r.at || "").replace("T", " ").slice(0, 16))}</td>
            <td>${esc(r.channel)}</td>
            <td>${esc(r.email)}</td>
            <td>${esc(r.viewers || "")}</td>
            <td>${r.code ? `<code>${esc(r.code)}</code>` : ""}</td>
            <td>${esc(r.sales || "0")}</td>
            <td style="color:${owed > 0 ? "#5fd18a" : "#9a9aa5"}">£${owed.toFixed(2)}${
              parseFloat(r.paidOut || "0") > 0 ? `<br><span class="muted" style="font-size:0.75rem">paid £${parseFloat(r.paidOut).toFixed(2)}</span>` : ""}</td>
            <td style="white-space:nowrap">${r.status === "approved"
              ? `<a href="/admin/partner/approve?id=${r.id}">view keys</a>`
              : `<a style="color:#5fd18a" href="/admin/partner/approve?id=${r.id}"
                   onclick="return confirm('Approve this streamer? Generates their lifetime key + 10 viewer keys + creator code.')">approve</a>`}
              ${owed > 0 ? ` · <a style="color:#38b0f8" href="/admin/partner/paid?id=${r.id}"
                   onclick="return confirm('Mark £${owed.toFixed(2)} as paid out? Click this AFTER sending the PayPal payment.')">mark paid</a>` : ""}
              · <a href="/admin/partner/dismiss?id=${r.id}"
                   onclick="return confirm('Remove this request from the list?')">dismiss</a></td>
          </tr>`; }).join("");
          return `<h2>Partner requests${pending ? ` <span style="color:#5fd18a;font-size:0.9rem">· ${pending} pending</span>` : ""}${
            totalOwed > 0 ? ` <span style="color:#38b0f8;font-size:0.9rem">· £${totalOwed.toFixed(2)} owed</span>` : ""}</h2>
            <div style="overflow-x:auto"><table style="width:100%;border-collapse:collapse;font-size:0.85rem">
            <tr style="text-align:left;color:#9a9aa5"><th>When (UTC)</th><th>Channel</th><th>Email</th><th>Viewers</th><th>Code</th><th>Sales</th><th>Owed</th><th></th></tr>
            ${rows}</table></div>`;
        } catch (e) {
          console.error("partners render failed:", e.message);
          return "";
        }
      })()}

      <div style="overflow-x:auto"><table style="width:100%;border-collapse:collapse;font-size:0.85rem">
        <tr style="text-align:left;color:#9a9aa5">
          <th>Key</th><th>Plan</th><th>Status</th><th>Devices</th><th>Last login (UTC)</th>
          <th>OS</th><th>App</th><th>Country</th><th>Source</th><th>Checks</th><th></th>
        </tr>
        ${rows || `<tr><td colspan="11" class="muted">No keys yet.</td></tr>`}
      </table></div>

      <h2 style="display:flex;align-items:center;gap:12px">Recently active people
        <a class="btn ghost" href="/admin" style="padding:6px 14px;font-size:0.8rem">↻ Refresh</a>
        <label class="muted" style="font-size:0.8rem;font-weight:400">
          <input type="checkbox" id="autoref"> auto (15s)</label>
      </h2>
      <script>
        // Auto-refresh toggle for the live lists; remembers the choice
        (function(){
          var cb = document.getElementById('autoref');
          if (!cb) return;
          if (localStorage.getItem('adminAuto') === '1') cb.checked = true;
          var t = null;
          function apply(){
            if (cb.checked){ localStorage.setItem('adminAuto','1'); t = setTimeout(function(){ location.reload(); }, 15000); }
            else { localStorage.removeItem('adminAuto'); if (t) clearTimeout(t); }
          }
          cb.addEventListener('change', apply); apply();
        })();
      </script>
      ${await (async () => {
        try {
          const people = await collectPresence(60);
          if (!people.length) return `<p class="muted">Nobody tracked yet.</p>`;
          const onlineCount = people.filter((p) => p.online).length;
          const rel = (s) => s < 60 ? s + "s ago" : s < 3600 ? Math.round(s / 60) + "m ago"
            : s < 86400 ? Math.round(s / 3600) + "h ago" : Math.round(s / 86400) + "d ago";
          const rows = people.map((p) => `<tr>
            <td>${p.online
              ? `<span style="color:#5fd18a">● online</span>`
              : `<span class="muted">${rel(p.ago)}</span>`}</td>
            <td>${p.type === "Web"
              ? `<a href="/admin/visitor/${encodeURIComponent(p.id)}" style="color:#c7c7d1">${esc(p.label)}</a>`
              : esc(p.label)}</td>
            <td>${p.type === "App"
              ? `<span style="color:#38b0f8">App</span>`
              : `<span style="color:#c99cf1">Web</span>`}</td>
            <td><span style="color:${p.planColor || "#9a9aa5"}">${esc(p.plan || "")}</span></td>
            <td class="muted">${esc(p.detail || "")}</td>
          </tr>`).join("");
          return `<p class="muted"><strong style="color:#5fd18a">${onlineCount}</strong> online now
            (active in the last 5 min) · showing ${people.length} most recent · click a visitor to see their journey</p>
            <div style="overflow-x:auto"><table style="width:100%;border-collapse:collapse;font-size:0.85rem">
            <tr style="text-align:left;color:#9a9aa5"><th>Status</th><th>Who</th><th>Via</th><th>Stage</th><th>Details</th></tr>
            ${rows}</table></div>`;
        } catch (e) {
          console.error("presence render failed:", e.message);
          return `<p class="muted">Presence list unavailable.</p>`;
        }
      })()}

      <h2>Installations</h2>
      ${await (async () => {
        try {
          const installs = await collectInstalls(200);
          if (!installs.length) return `<p class="muted">No installs recorded yet.</p>`;
          const rel = (ms) => {
            if (!ms) return "never";
            const s = Math.max(0, Math.round((Date.now() - ms) / 1000));
            return s < 60 ? s + "s ago" : s < 3600 ? Math.round(s / 60) + "m ago"
              : s < 86400 ? Math.round(s / 3600) + "h ago" : Math.round(s / 86400) + "d ago";
          };
          const colors = { online: "#5fd18a", active: "#38b0f8", idle: "#9a9aa5", uninstalled: "#f47272" };
          const labels = { online: "● online", active: "installed", idle: "idle (7d+)", uninstalled: "uninstalled" };
          const live = installs.filter((i) => i.status !== "uninstalled").length;
          const gone = installs.filter((i) => i.status === "uninstalled").length;
          const tamperedCount = installs.filter((i) => i.tampered).length;
          const rows = installs.map((i) => `<tr>
            <td style="color:${colors[i.status]}">${labels[i.status]}${
              i.tampered ? ` <span style="color:#f47272" title="Reported hash differs from EXPECTED_HASH">⚠ modified</span>` : ""}</td>
            <td><code>${esc(i.device.slice(0, 12))}</code></td>
            <td>${esc(i.os)}</td>
            <td>${esc(i.version ? "v" + i.version : "")}</td>
            <td>${esc(i.country)}</td>
            <td class="muted">${esc(i.key ? i.key.slice(0, 16) + "…" : "")}</td>
            <td class="muted">${i.status === "uninstalled" && i.uninstalledAt
              ? "removed " + rel(Date.parse(i.uninstalledAt)) : rel(i.last)}</td>
          </tr>`).join("");
          return `<p class="muted"><strong style="color:#f2f2f7">${installs.length}</strong> machine(s) ·
            <strong style="color:#5fd18a">${live}</strong> still installed ·
            <strong style="color:#f47272">${gone}</strong> uninstalled${
            tamperedCount ? ` · <strong style="color:#f47272">${tamperedCount}</strong> modified binary` : ""} ·
            each row is one PC (anonymous machine fingerprint)</p>
            <div style="overflow-x:auto"><table style="width:100%;border-collapse:collapse;font-size:0.85rem">
            <tr style="text-align:left;color:#9a9aa5"><th>Status</th><th>Machine</th><th>OS</th><th>App</th><th>Country</th><th>Key</th><th>Last seen</th></tr>
            ${rows}</table></div>`;
        } catch (e) {
          console.error("installs render failed:", e.message);
          return `<p class="muted">Installations list unavailable.</p>`;
        }
      })()}

      <h2>Website analytics</h2>
      ${await (async () => {
        try {
          const days = [];
          for (let i = 13; i >= 0; i--) {
            const d = new Date(Date.now() - i * 86400000).toISOString().slice(0, 10);
            days.push(d);
          }
          const [
            totalViews, uniqTotal, homeViews,
            evDownload, evBuy, evPurchase, evActivated, revenue,
            refFlat, countryFlat,
          ] = await Promise.all([
            redis(["GET", "stats:views:total"]),
            redis(["PFCOUNT", "stats:uniq"]),
            redis(["HGET", "stats:page", "home"]),
            redis(["GET", "stats:event:download"]),
            redis(["GET", "stats:event:buy_click"]),
            redis(["GET", "stats:event:purchase"]),
            redis(["GET", "stats:event:key_activated"]),
            redis(["GET", "stats:revenue"]),
            redis(["HGETALL", "stats:ref"]),
            redis(["HGETALL", "stats:country"]),
          ]);
          const perDay = await Promise.all(days.map((d) => redis(["GET", "stats:views:" + d])));
          const uniqPerDay = await Promise.all(days.map((d) => redis(["PFCOUNT", "stats:uniq:" + d])));

          const n = (x) => Number(x || 0);
          const views = n(totalViews), uniq = n(uniqTotal);
          const downloads = n(evDownload), buys = n(evBuy), purchases = n(evPurchase), activations = n(evActivated);
          const rev = parseFloat(revenue || "0").toFixed(2);
          const conv = uniq > 0 ? ((purchases / uniq) * 100).toFixed(1) : "0.0";

          // Mini bar chart of the last 14 days of views
          const maxDay = Math.max(1, ...perDay.map(n));
          const bars = days.map((d, i) => {
            const h = Math.round((n(perDay[i]) / maxDay) * 46) + 2;
            return `<div title="${d}: ${n(perDay[i])} views, ${n(uniqPerDay[i])} unique"
              style="flex:1;display:flex;flex-direction:column;justify-content:flex-end;align-items:center;gap:4px">
              <div style="width:70%;height:${h}px;background:#2a70a6;border-radius:3px 3px 0 0"></div>
              <span style="font-size:0.6rem;color:#5a5a68">${d.slice(5)}</span></div>`;
          }).join("");

          const toSorted = (flat) => {
            const o = flattenHash(flat); const arr = Object.entries(o).map(([k, v]) => [k, Number(v)]);
            arr.sort((a, b) => b[1] - a[1]); return arr;
          };
          const refRows = toSorted(refFlat).slice(0, 8).map(([k, v]) =>
            `<tr><td>${esc(k)}</td><td>${v}</td></tr>`).join("") || `<tr><td class="muted" colspan="2">No data yet</td></tr>`;
          const countryRows = toSorted(countryFlat).slice(0, 8).map(([k, v]) =>
            `<tr><td>${esc(k)}</td><td>${v}</td></tr>`).join("") || `<tr><td class="muted" colspan="2">No data yet</td></tr>`;

          const card = (label, value, sub) =>
            `<div class="card" style="padding:16px"><div class="muted" style="font-size:0.8rem">${label}</div>
             <div style="font-size:1.6rem;font-weight:700;color:#f2f2f7">${value}</div>
             ${sub ? `<div class="muted" style="font-size:0.75rem">${sub}</div>` : ""}</div>`;

          return `
            <div style="display:grid;grid-template-columns:repeat(auto-fit,minmax(120px,1fr));gap:12px;margin-bottom:20px">
              ${card("Page views", views)}
              ${card("Unique visitors", uniq)}
              ${card("Downloads", downloads)}
              ${card("Purchases", purchases, "£" + rev + " revenue")}
              ${card("Keys activated", activations)}
              ${card("Conversion", conv + "%", "visitors -> purchase")}
            </div>

            <div class="card" style="margin-bottom:20px">
              <div class="muted" style="font-size:0.8rem;margin-bottom:10px">Views per day (last 14 days)</div>
              <div style="display:flex;align-items:flex-end;gap:3px;height:70px">${bars}</div>
            </div>

            <div class="card" style="margin-bottom:20px">
              <div class="muted" style="font-size:0.85rem;margin-bottom:8px">Funnel</div>
              <div style="display:flex;flex-wrap:wrap;gap:8px;align-items:center;font-size:0.9rem">
                <span>${homeViews ? n(homeViews) : views} visits</span> <span class="muted">-&gt;</span>
                <span>${downloads} downloads</span> <span class="muted">-&gt;</span>
                <span>${buys} buy clicks</span> <span class="muted">-&gt;</span>
                <span style="color:#5fd18a">${purchases} purchases</span> <span class="muted">-&gt;</span>
                <span style="color:#38b0f8">${activations} activated</span>
              </div>
            </div>

            <div style="display:grid;grid-template-columns:1fr 1fr;gap:16px;margin-bottom:8px">
              <div><div class="muted" style="font-size:0.85rem;margin-bottom:6px">Top referrers</div>
                <table style="width:100%;border-collapse:collapse;font-size:0.8rem">
                <tr style="color:#9a9aa5;text-align:left"><th>Source</th><th>Views</th></tr>${refRows}</table></div>
              <div><div class="muted" style="font-size:0.85rem;margin-bottom:6px">Top countries</div>
                <table style="width:100%;border-collapse:collapse;font-size:0.8rem">
                <tr style="color:#9a9aa5;text-align:left"><th>Country</th><th>Views</th></tr>${countryRows}</table></div>
            </div>`;
        } catch (e) {
          console.error("analytics render failed:", e.message);
          return `<p class="muted">Analytics unavailable.</p>`;
        }
      })()}

      <h2>Recent activity</h2>
      ${await (async () => {
        try {
          const raw = (await redis(["LRANGE", "analytics:events", "0", "24"])) || [];
          if (!raw.length) return `<p class="muted">No events yet.</p>`;
          const rows = raw.map((r) => {
            let e = {}; try { e = JSON.parse(r); } catch {}
            const detail = e.plan ? e.plan : (e.key ? esc(String(e.key).slice(0, 22)) + "..." : "");
            return `<tr><td>${esc((e.at || "").replace("T", " ").slice(0, 16))}</td>
              <td>${esc(e.event || "")}</td><td>${esc(detail)}</td>
              <td>${esc(e.ref || "")}</td><td>${esc(e.country || e.os || "")}</td></tr>`;
          }).join("");
          return `<div style="overflow-x:auto"><table style="width:100%;border-collapse:collapse;font-size:0.8rem">
            <tr style="text-align:left;color:#9a9aa5"><th>When (UTC)</th><th>Event</th><th>Detail</th><th>Source</th><th>Where</th></tr>
            ${rows}</table></div>`;
        } catch { return `<p class="muted">Activity feed unavailable.</p>`; }
      })()}

      <h2>Recent crashes</h2>
      ${await (async () => {
        try {
          const total = await redis(["LLEN", "crashes"]);
          if (!total) return `<p class="muted">No crashes reported. Nice.</p>`;
          const raw = (await redis(["LRANGE", "crashes", "0", "9"])) || [];
          const crashRows = raw.map((r) => {
            let c = {};
            try { c = JSON.parse(r); } catch {}
            return `<tr><td>${esc((c.at || "").replace("T", " ").slice(0, 16))}</td>
              <td>${esc(c.v || "")}</td><td>${esc(c.os || "")}</td>
              <td>${esc(c.kind || "")}</td><td><code>${esc(c.code || "")}</code></td></tr>`;
          }).join("");
          return `<p class="muted">${total} total (showing latest 10)</p>
            <div style="overflow-x:auto"><table style="width:100%;border-collapse:collapse;font-size:0.85rem">
            <tr style="text-align:left;color:#9a9aa5"><th>When (UTC)</th><th>App</th><th>OS</th><th>Kind</th><th>Code</th></tr>
            ${crashRows}</table></div>`;
        } catch { return `<p class="muted">Crash log unavailable.</p>`; }
      })()}

      <style>td,th{padding:8px 10px;border-bottom:1px solid #2e2e3e} td a{color:#f47272} .muted{color:#9a9aa5}</style>`,
      { wide: true }));
  } catch (err) {
    console.error("admin dashboard failed:", err);
    const esc = (s) => String(s ?? "").replace(/[&<>"]/g, (c) =>
      ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));
    // Admin-only page: surfacing the real cause (HTTP status from Upstash) is
    // safe here and turns a dead-end "Redis error" into something diagnosable.
    // A 429 = Upstash daily/monthly command limit hit; 401 = token rotated;
    // paused/expired free DB also shows here.
    const msg = esc(err && err.message ? err.message : String(err));
    let hint = "";
    if (/\b429\b/.test(msg))                    hint = "Upstash command limit reached (free tier). Lower ANALYTICS_SAMPLE in Render (e.g. 0.1) to cut usage, wait for the monthly reset, or upgrade the Upstash plan.";
    else if (/\b40[13]\b/.test(msg))            hint = "Upstash rejected the token - update UPSTASH_REDIS_REST_TOKEN in Render and redeploy.";
    else if (/fetch|ENOTFOUND|ECONN/.test(msg)) hint = "Could not reach Upstash - check the DB isn't paused/deleted and UPSTASH_REDIS_REST_URL is correct.";
    res.status(500).send(page("Error", `<h1>Dashboard temporarily unavailable</h1>
      <p class="muted">A backend (Upstash Redis) call failed, so the dashboard couldn't load.
      Your keys and licences are unaffected - this is only the analytics/admin view.</p>
      ${hint ? `<div class="card" style="border-color:#f47272"><strong>Likely cause:</strong> ${esc(hint)}</div>` : ""}
      <p class="muted" style="margin-top:16px">Details:</p>
      <code class="key">${msg}</code>
      <p style="margin-top:16px"><a class="btn ghost" href="/admin">Retry</a></p>`, { wide: true }));
  }
});

// Mollie webhook: posted {id} whenever a payment changes state. Recurring
// subscription payments arrive ONLY here (no user browser involved), so this
// is what keeps monthly keys alive. Always answers 200 so Mollie stops retrying.
app.post("/webhook", async (req, res) => {
  res.sendStatus(200); // ack first; processing errors are ours to log
  try {
    const id = String(req.body.id || "");
    if (!/^tr_[a-zA-Z0-9]+$/.test(id) || !bindingEnabled) return;
    const payment = await mollie("GET", `/payments/${id}`);
    if (payment.status !== "paid") return;

    if (payment.sequenceType === "first" && payment.customerId) {
      // Backup path in case the buyer never returns to /key
      await activateSubscription(payment);
    } else if (!payment.customerId) {
      // One-time (lifetime) payment. Same first-seen guard as /key, so the
      // purchase stats + referral commission land exactly once no matter
      // which path sees the payment first (buyer may never return to /key).
      const added = await redis(["SADD", "stats:purchased", id]);
      if (added === 1) {
        await redis(["INCR", "stats:event:purchase"]);
        await redis(["INCRBYFLOAT", "stats:revenue",
          (payment.amount && payment.amount.value) || "0"]);
        await creditReferral(payment).catch((e) => console.error("referral credit failed:", e.message));
      }
    } else if (payment.sequenceType === "recurring" && payment.customerId) {
      // Renewal: extend the paid-until window of this customer's key
      const key = await redis(["GET", "cust:" + payment.customerId]);
      if (key) {
        await redis(["HSET", "sub:" + key, "paidUntil", Date.now() + SUB_GRACE_MS]);
        console.log(`Renewal OK for ${key.slice(0, 12)}...`);
      }
    }
  } catch (err) {
    console.error("webhook processing failed:", err.message);
  }
});

// App update check. Set LATEST_VERSION (e.g. "1.4.0") and DOWNLOAD_URL in
// Render when you publish a new build; the app shows an update banner.
app.get("/api/version", (req, res) => {
  res.json({ version: LATEST_VERSION, url: DOWNLOAD_URL });
});

// Called by the uninstaller (best-effort) so the dashboard knows the app was
// removed from a machine. Device id only - no key required.
app.get("/api/uninstall", async (req, res) => {
  res.json({ ok: true });
  // Unauthenticated by nature (the app is being removed), so throttle hard:
  // a real machine uninstalls once, not hundreds of times an hour.
  if (!rateAllow("uninstall", req, 5, 60 * 60 * 1000)) return;
  if (!bindingEnabled) return;
  const device = String(req.query.device || "").slice(0, 128);
  if (!device) return;
  try {
    // Only flag an existing record; don't create one from a random ping
    if ((await redis(["EXISTS", "install:" + device])) === 1) {
      await redis(["HSET", "install:" + device,
        "uninstalled", "1", "uninstalledAt", new Date().toISOString()]);
      await redis(["INCR", "stats:event:uninstall"]);
    }
  } catch (err) {
    console.error("uninstall record failed:", err.message);
  }
});

// Crash telemetry from the desktop app (fire-and-forget on its side).
// Kept as a capped list; readable only through the admin dashboard.
app.get("/api/crash", async (req, res) => {
  res.json({ ok: true }); // answer instantly - a dying app is waiting
  // Unauthenticated write - throttle so a bot can't flood the crash list or
  // burn Redis quota. A genuinely crash-looping machine still gets 10 reports
  // an hour on the dashboard, which is plenty to diagnose it.
  if (!rateAllow("crash", req, 10, 60 * 60 * 1000)) return;
  if (!bindingEnabled) return;
  try {
    const entry = JSON.stringify({
      at: new Date().toISOString(),
      v: String(req.query.v || "").slice(0, 32),
      os: String(req.query.os || "").slice(0, 64),
      kind: String(req.query.kind || "").slice(0, 16),
      code: String(req.query.code || "").slice(0, 24),
    });
    await redis(["LPUSH", "crashes", entry]);
    await redis(["LTRIM", "crashes", "0", "199"]);
  } catch (err) {
    console.error("crash record failed:", err.message);
  }
});

// Ad banner shown on the app's activation (locked) screen for unlicensed
// users. Set AD_IMAGE_URL (a direct https link to a PNG/JPG) and AD_LINK_URL
// (where a click should go) in Render to enable it - no app update needed to
// change or swap the ad later, just edit these env vars and redeploy.
const AD_IMAGE_URL = process.env.AD_IMAGE_URL || "";
const AD_LINK_URL = process.env.AD_LINK_URL || "";
app.get("/api/ad", (req, res) => {
  if (!AD_IMAGE_URL || !AD_LINK_URL) {
    return res.json({ imageUrl: null, linkUrl: null });
  }
  res.json({ imageUrl: AD_IMAGE_URL, linkUrl: AD_LINK_URL });
});

app.use((err, req, res, next) => {
  console.error(err);
  res
    .status(500)
    .send(page("Error", `<h1>Something went wrong</h1><p>Please try again in a minute.</p>`));
});

app.listen(PORT, () => console.log(`License server listening on :${PORT}`));
