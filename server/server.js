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
const PRICE = { currency: "GBP", value: "2.00" };
const PRODUCT = "Antigravity Voice Engine license";

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
  res.send(
    page(
      "Antigravity Voice Engine",
      `<h1>Antigravity Voice Engine &amp; Soundboard</h1>
       <p>Real-time voice changer and soundboard for Windows.</p>
       <p>Buy a one-time license for <strong>£2.00</strong>. After payment you get a
       license key to paste into the app.</p>
       <a class="btn" href="/buy">Buy license – £2.00</a>
       <p class="muted">Payment is handled by Mollie. Keep your key safe – you'll need it
       if you reinstall.</p>`
    )
  );
});

app.get("/buy", async (req, res, next) => {
  try {
    const payment = await mollie("POST", "/payments", {
      amount: PRICE,
      description: PRODUCT,
      redirectUrl: `${BASE_URL}/key`,
      webhookUrl: `${BASE_URL}/webhook`,
    });
    // Point the redirect at this specific payment so /key can look it up.
    await mollie("PATCH", `/payments/${payment.id}`, {
      redirectUrl: `${BASE_URL}/key?pid=${payment.id}`,
    });
    res.redirect(payment._links.checkout.href);
  } catch (err) {
    next(err);
  }
});

app.get("/key", async (req, res, next) => {
  try {
    const pid = String(req.query.pid || "");
    if (!/^[a-zA-Z0-9_]+$/.test(pid)) {
      return res.status(400).send(page("Invalid request", `<h1>Invalid request</h1>`));
    }
    const payment = await mollie("GET", `/payments/${pid}`);

    if (payment.status === "paid") {
      return res.send(
        page(
          "Your license key",
          `<h1>Payment complete – thank you!</h1>
           <p>Your license key (click to select, then copy):</p>
           <code class="key">${makeKey(pid)}</code>
           <p>Open <strong>Antigravity Voice Engine</strong>, paste the key into the
           activation screen and press <strong>Activate</strong>.</p>
           <p class="muted">Save this key somewhere safe – this page won't be reachable
           forever, but the key never expires.</p>`
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

// Called by the desktop app. Query: key (required), device (fingerprint).
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
    const setKey = "lic:" + key;
    // Already activated on this device? Always allow.
    if ((await redis(["SISMEMBER", setKey, device])) === 1) {
      return res.json({ valid: true });
    }
    // New device: allow only if under the per-key limit.
    const count = await redis(["SCARD", setKey]);
    if (count < DEVICE_LIMIT) {
      await redis(["SADD", setKey, device]);
      return res.json({ valid: true });
    }
    return res.json({ valid: false, reason: "device_limit" });
  } catch (err) {
    // Redis outage: don't punish a paying user whose key is math-valid.
    console.error(err);
    return res.json({ valid: true, degraded: true });
  }
});

// Mollie requires a webhook endpoint; state is derived from the API on demand,
// so acknowledging is all that's needed here.
app.post("/webhook", (req, res) => res.sendStatus(200));

app.use((err, req, res, next) => {
  console.error(err);
  res
    .status(500)
    .send(page("Error", `<h1>Something went wrong</h1><p>Please try again in a minute.</p>`));
});

app.listen(PORT, () => console.log(`License server listening on :${PORT}`));
