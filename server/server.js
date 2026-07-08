// Antigravity Voice Engine - license server
//
// Flow:
//   GET  /            -> purchase page with a Buy button
//   GET  /buy         -> creates a 2.00 GBP Mollie payment, redirects to Mollie checkout
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

const app = express();
app.use(express.urlencoded({ extended: true }));
app.use(express.json());

const { MOLLIE_API_KEY, LICENSE_SECRET, BASE_URL } = process.env;
const PORT = process.env.PORT || 3000;
const PRODUCT = "Antigravity Voice Engine license";

// Two plans: one-time lifetime key, or a monthly subscription whose key
// expires unless Mollie keeps reporting successful recurring payments.
const PLANS = {
  life:    { amount: { currency: "GBP", value: "10.00" }, label: "Lifetime" },
  monthly: { amount: { currency: "GBP", value: "2.00" },  label: "Monthly" },
};
// Each paid month grants 35 days so retries/bank delays never lock a payer out.
const SUB_GRACE_MS = 35 * 24 * 3600 * 1000;

// App update check: bump these env vars in Render when you release a new build.
const LATEST_VERSION = process.env.LATEST_VERSION || "";
const DOWNLOAD_URL = process.env.DOWNLOAD_URL || "";

// Device binding (anti key-sharing). Optional: if the Upstash vars are not
// set, the server falls back to signature-only checks so it still runs.
const { UPSTASH_REDIS_REST_URL, UPSTASH_REDIS_REST_TOKEN } = process.env;
const DEVICE_LIMIT = parseInt(process.env.DEVICE_LIMIT || "2", 10);
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

function page(title, bodyHtml) {
  return `<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>${title}</title>
<style>
  body{font-family:system-ui,sans-serif;background:#0f0f14;color:#f2f2f7;display:flex;justify-content:center;padding:48px 16px}
  main{max-width:560px;width:100%}
  h1{color:#38b0f8;font-size:1.5rem}
  a.btn,button{display:inline-block;background:#2a70a6;color:#fff;border:none;border-radius:8px;
    padding:12px 24px;font-size:1rem;text-decoration:none;cursor:pointer}
  a.btn:hover,button:hover{background:#38b0f8}
  code.key{display:block;background:#1c1c26;border:1px solid #2e2e3e;border-radius:8px;padding:16px;
    font-size:1.05rem;word-break:break-all;margin:16px 0;user-select:all}
  p.muted{color:#9a9aa5}
</style></head><body><main>${bodyHtml}</main></body></html>`;
}

// ---------- routes ----------

app.get("/", (req, res) => {
  const monthlyBtn = bindingEnabled
    ? `<a class="btn" href="/buy?plan=monthly" style="background:#3a3a4e">Subscribe – £2.00/month</a>`
    : "";
  res.send(
    page(
      "Antigravity Voice Engine",
      `<h1>Antigravity Voice Engine &amp; Soundboard</h1>
       <p>Real-time voice changer and soundboard for Windows.</p>
       <p>Pay once for a <strong>lifetime key</strong>, or subscribe monthly.
       After payment you get a license key to paste into the app.</p>
       <p><a class="btn" href="/buy?plan=life">Buy lifetime – £10.00</a> &nbsp; ${monthlyBtn}</p>
       <p class="muted">Payment is handled by Mollie. Keep your key safe – you'll need it
       if you reinstall. Monthly keys stop working if the subscription ends.</p>`
    )
  );
});

app.get("/buy", async (req, res, next) => {
  try {
    const plan = req.query.plan === "monthly" ? "monthly" : "life";

    if (plan === "monthly" && !bindingEnabled) {
      return res.status(503).send(page("Unavailable",
        `<h1>Monthly plan unavailable</h1><p>Subscriptions need the database
         (Upstash) configured. <a class="btn" href="/buy?plan=life">Buy lifetime instead</a></p>`));
    }

    let paymentBody = {
      amount: PLANS[plan].amount,
      description: `${PRODUCT} (${PLANS[plan].label.toLowerCase()})`,
      redirectUrl: `${BASE_URL}/key`,
      webhookUrl: `${BASE_URL}/webhook`,
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
      amount: PLANS.monthly.amount,
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

  await redis(["HSET", "sub:" + key,
    "customerId", custId,
    "subscriptionId", subId,
    "paidUntil", Date.now() + SUB_GRACE_MS,
  ]);
  await redis(["SET", "cust:" + custId, key]); // recurring payments -> key
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
      return res.send(
        page(
          "Your license key",
          `<h1>Payment complete – thank you!</h1>
           <p>Your license key (click to select, then copy):</p>
           <code class="key">${makeKey(pid)}</code>
           <p>Open <strong>Antigravity Voice Engine</strong>, paste the key into the
           activation screen and press <strong>Activate</strong>.</p>
           <p class="muted">Save this key somewhere safe – this page won't be reachable
           forever, but ${subNote}</p>`
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
function recordProfile(req, key, device) {
  if (!bindingEnabled) return;
  const now = new Date().toISOString();
  const ip = String(req.headers["x-forwarded-for"] || req.socket.remoteAddress || "")
    .split(",")[0].trim();
  const country = String(req.headers["cf-ipcountry"] || req.headers["x-vercel-ip-country"] || "");
  const fields = [
    "lastSeen", now,
    "lastDevice", device || "",
    "os", String(req.query.os || "").slice(0, 64),
    "appVersion", String(req.query.v || "").slice(0, 32),
    "ip", ip.slice(0, 64),
  ];
  if (country) fields.push("country", country.slice(0, 8));
  Promise.all([
    redis(["HSET", "profile:" + key, ...fields]),
    redis(["HSETNX", "profile:" + key, "firstSeen", now]),
    redis(["HINCRBY", "profile:" + key, "checks", 1]),
  ]).catch((err) => console.error("profile write failed:", err.message));
}

// Called by the desktop app. Query: key (required), device (fingerprint),
// os + v (analytics, optional).
//   { valid: true }                          -> unlock
//   { valid: false, reason: "invalid" }      -> bad/forged key
//   { valid: false, reason: "device_limit" } -> real key, too many devices
app.get("/api/verify", async (req, res) => {
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
    // Monthly keys: reject once the paid-until window has lapsed.
    const paidUntil = await redis(["HGET", "sub:" + key, "paidUntil"]);
    if (paidUntil !== null && Date.now() > Number(paidUntil)) {
      return res.json({ valid: false, reason: "expired" });
    }
    // Tell the app which plan this is so it can show it next to Reset.
    const planFields = paidUntil === null
      ? { plan: "lifetime" }
      : { plan: "monthly", paidUntil: new Date(Number(paidUntil)).toISOString() };

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
  const given = String(req.query.secret || "");
  if (given && safeEqual(given, ADMIN_SECRET)) return true;
  return hasValidSession(req);
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
  const pw = String(req.body.password || "");
  if (ADMIN_SECRET.length > 0 && safeEqual(pw, ADMIN_SECRET)) {
    res.setHeader("Set-Cookie", makeSessionCookie());
    return res.redirect("/admin");
  }
  res.status(403).send(page("Login", `<h1>Wrong password</h1><p><a class="btn" href="/login">Try again</a></p>`));
});

async function collectKeyData() {
  const keys = (await redis(["SMEMBERS", "keys:used"])) || [];
  const out = [];
  for (const k of keys.slice(0, 500)) {
    const profile = (await redis(["HGETALL", "profile:" + k])) || [];
    // Upstash returns HGETALL as a flat [field, value, ...] array
    const p = {};
    for (let i = 0; i + 1 < profile.length; i += 2) p[profile[i]] = profile[i + 1];

    // Plan/status from the subscription record (absent = lifetime)
    const paidUntil = await redis(["HGET", "sub:" + k, "paidUntil"]);
    const plan = paidUntil === null ? "lifetime" : "monthly";
    const status =
      plan === "lifetime" ? "active"
      : Date.now() > Number(paidUntil) ? "expired" : "active";

    out.push({
      key: k,
      plan,
      status,
      paidUntil: paidUntil === null ? "" : new Date(Number(paidUntil)).toISOString(),
      activeDevices: await redis(["SCARD", "lic:" + k]),
      ...p,
    });
  }
  return { count: keys.length, keys: out };
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
  if (!adminAuthorized(req)) return res.status(403).json({ error: "forbidden" });
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
    const monthly = data.keys.filter((k) => k.plan === "monthly");
    const activeSubs = monthly.filter((k) => k.status === "active").length;
    const rows = data.keys
      .sort((a, b) => String(b.lastSeen || "").localeCompare(String(a.lastSeen || "")))
      .map((k) => `<tr>
        <td><code>${esc(k.key)}</code></td>
        <td>${esc(k.plan)}</td>
        <td style="color:${k.status === "active" ? "#5fd18a" : "#f47272"}">${esc(k.status)}${
          k.paidUntil ? `<br><span class="muted" style="font-size:0.75rem">until ${esc(k.paidUntil.slice(0, 10))}</span>` : ""}</td>
        <td>${esc(k.activeDevices)}/${DEVICE_LIMIT}</td>
        <td>${esc((k.lastSeen || "").replace("T", " ").slice(0, 16))}</td>
        <td>${esc(k.os || "")}</td>
        <td>${esc(k.appVersion || "")}</td>
        <td>${esc(k.country || "")}</td>
        <td>${esc(k.checks || 0)}</td>
        <td><a href="/api/admin/clear?key=${encodeURIComponent(k.key)}&back=1"
               onclick="return confirm('Free all device slots for this key?')">clear devices</a></td>
      </tr>`).join("");
    res.send(page("License dashboard", `
      <h1>License dashboard</h1>
      <p class="muted">
        <strong style="color:#f2f2f7">${data.count}</strong> key(s) sold ·
        <strong style="color:#f2f2f7">${data.count - monthly.length}</strong> lifetime ·
        <strong style="color:#f2f2f7">${activeSubs}</strong> active subscription(s) ·
        "Clear devices" frees all slots so the customer can re-activate anywhere.
      </p>
      <div style="overflow-x:auto"><table style="width:100%;border-collapse:collapse;font-size:0.85rem">
        <tr style="text-align:left;color:#9a9aa5">
          <th>Key</th><th>Plan</th><th>Status</th><th>Devices</th><th>Last login (UTC)</th>
          <th>OS</th><th>App</th><th>Country</th><th>Checks</th><th></th>
        </tr>
        ${rows || `<tr><td colspan="10" class="muted">No keys activated yet.</td></tr>`}
      </table></div>
      <style>td,th{padding:8px 10px;border-bottom:1px solid #2e2e3e} td a{color:#f47272} .muted{color:#9a9aa5}</style>`));
  } catch (err) {
    console.error(err);
    res.status(500).send(page("Error", `<h1>Redis error</h1>`));
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

app.use((err, req, res, next) => {
  console.error(err);
  res
    .status(500)
    .send(page("Error", `<h1>Something went wrong</h1><p>Please try again in a minute.</p>`));
});

app.listen(PORT, () => console.log(`License server listening on :${PORT}`));
