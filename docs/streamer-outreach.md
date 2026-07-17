# Streamer outreach playbook

The partner funnel is live at **`/partners`**: streamers apply, you approve
from the dashboard, and approval generates their lifetime key + 10 viewer
giveaway keys and opens a pre-filled reply in your own mail app. Nothing is
ever sent automatically - every email is one you personally review and send.
That's deliberate:

- **Deliverability**: bulk unsolicited mail gets your domain blacklisted fast,
  which would also kill your purchase/receipt emails later.
- **UK law (PECR)**: most streamers are sole traders, who count as
  *individuals* - bulk marketing email to them without consent is not allowed.
  A personal, one-to-one business proposal sent to the **business-inquiries
  address they publish** (on their channel's About page, precisely for offers
  like this) is normal industry practice.
- **Conversion**: streamers ignore templates and read personal notes. Ten
  personal emails beat a thousand blasted ones.

## Who to target

Small is better - big streamers ignore cold email and charge money:

- **50-500 average live viewers** on Twitch/Kick/YouTube. Big enough to
  matter, small enough that free Pro + viewer drops is a genuinely nice offer.
- Playing **social/voice-chat games** (among-us-likes, horror co-op, GTA RP,
  Rust, Phasmophobia, party games) - people who'd actually USE a voice
  changer on stream for laughs.
- Find them in the Twitch directory for those games, sorted by viewers,
  scrolling past the top pages. Only use the **business email** listed on
  their About panel. If there's no public business email, skip them (DMs on
  the platform are the fallback, not scraping).

## The email (short beats clever)

Subject: `Free Pro + 10 viewer keys for your streams`

> Hi <name>,
>
> I watched your <game> stream <specific detail - prove you actually looked>.
> I'm the solo dev of Antigravity Voice Engine, a low-latency voice changer +
> soundboard for Windows.
>
> No strings offer: a free lifetime Pro key for you, plus 10 one-month Pro
> keys to drop to your viewers however you like. Nothing to pay, ever, and
> you don't have to promise anything - if it gets a laugh on stream, that's
> the whole win for me.
>
> Grab it here (10 seconds): <BASE_URL>/partners
>
> <your name>

Rules that keep replies coming and complaints at zero:

- One personal detail per email, always. If you can't write one, don't send it.
- Send from your normal mailbox, a handful per day - not a blast tool.
- Never follow up more than once. No reply after one nudge = no.
- If anyone asks to be left alone, note the channel and never contact again.

## The commission deal (creator codes)

Approval also mints a **creator code** (e.g. `COOLSTREAMER`):

- Viewers buying via `BASE_URL/?code=CODE` get **lifetime Pro at £8** (20% off).
- The streamer earns **£2.50 per lifetime sale**. Monthly earns no commission,
  and with a code the monthly 80p intro is disabled - promos never stack, so a
  commission can never exceed what a buyer actually paid.
- Balances accrue on the dashboard (Sales / Owed columns). **Payouts are
  manual and monthly**: send the PayPal payment yourself once someone is over
  £20, THEN click "mark paid" to move the balance into their paid-out total.
- Keep the PayPal receipts - you're making business payments to other sole
  traders and HMRC expects records. (This is another good reason for the Ltd.)
- The approval email already tells streamers about UK disclosure rules (#ad);
  their compliance duty is stated up front so it can't splash back on you.

## After they apply

1. Dashboard -> **Partner requests** -> sanity-check the channel is real and
   roughly the right size.
2. **approve** -> keys are generated -> **Open reply email** -> add one
   personal line at the top -> send.
3. Their keys show up in the main key table (`partner:` / `viewers:` notes),
   so you can see whether they and their viewers actually activate - that
   tells you which kind of streamer converts, so targeting improves each week.
4. Viewer keys expire after 30 days; some of those viewers buy. That's the
   funnel.
