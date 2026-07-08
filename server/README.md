# License server (Mollie: £10 lifetime / £2 monthly)

Node/Express service that sells license keys for the desktop app via
[Mollie](https://www.mollie.com) and verifies them. Lifetime keys are
HMAC-signed and stateless; monthly subscription keys additionally track a
paid-until window in Upstash Redis, extended by Mollie's recurring-payment
webhooks. Includes a login-protected admin dashboard (`/admin`) with license
viewer, per-key usage profiles, and a device manager.

Extra optional env vars (beyond the ones below):

- `ADMIN_SECRET` — enables `/admin` (dashboard login password) and `/api/admin/*`
- `LATEST_VERSION` + `DOWNLOAD_URL` — power the in-app "update available"
  banner via `/api/version`; bump them when you publish a new build

## Deploy on Render

1. Push this repo to GitHub.
2. In the Render dashboard: **New → Web Service**, pick this repo.
   - **Root Directory**: `server`
   - **Build Command**: `npm install`
   - **Start Command**: `node server.js`
   - Instance type: Free is fine.
3. Add environment variables:
   - `MOLLIE_API_KEY` — from Mollie dashboard → Developers → API keys.
     Use `test_...` to try everything without real money, `live_...` to sell
     (live mode requires your Mollie account/website to be approved by Mollie).
   - `LICENSE_SECRET` — a long random string (Render's "Generate" button is fine).
     **Never change it later** — every key already sold would stop validating.
   - `BASE_URL` — the service's public URL, e.g. `https://antigravity-license.onrender.com`
     (you know it after the first deploy; set it and redeploy).

### Optional: device binding (stop key-sharing)

Without this, a key works on unlimited machines (buyers could share it). To bind
each key to a limited number of devices:

1. Create a free [Upstash](https://upstash.com) account → **Create Database** →
   Redis (any region). On the database page, copy the **REST URL** and
   **REST token** (the "REST API" section, not the Redis TCP URL).
2. Add these env vars to the Render service:
   - `UPSTASH_REDIS_REST_URL` — the REST URL
   - `UPSTASH_REDIS_REST_TOKEN` — the REST token
   - `DEVICE_LIMIT` — max devices per key (default `2`)
3. Redeploy.

The desktop app sends a hashed Windows MachineGuid as the device id. The first
`DEVICE_LIMIT` machines to activate a key are remembered; further machines get
`{"valid":false,"reason":"device_limit"}`. If Upstash is unreachable, the server
falls back to signature-only so paying users are never locked out. Leaving these
vars unset disables binding entirely (server logs a warning).

## Point the desktop app at it

In [src/license/LicenseManager.cpp](../src/license/LicenseManager.cpp) set
`kServerHost` to your Render hostname (no `https://`, no trailing slash), then
rebuild + repackage. The Buy button and key verification then use your server.

## Endpoints

| Route | Purpose |
|---|---|
| `GET /` | Purchase page with Buy button |
| `GET /buy` | Creates the £2 Mollie payment, redirects to Mollie checkout |
| `GET /key?pid=...` | Post-payment page; shows the license key once paid |
| `GET /api/verify?key=...` | Used by the app; returns `{"valid":true\|false}` |
| `POST /webhook` | Mollie webhook (acknowledged; state is read from the API) |

## Test it end to end (no real money)

With `MOLLIE_API_KEY=test_...`, open `/buy`, pick any method, and Mollie's test
checkout lets you choose "Paid" — you'll land on the key page with a working key.

## Honest limitation

The check runs in the desktop app, so a determined person can patch the exe to
skip it. This keeps honest users honest, which is the right bar for £2.
