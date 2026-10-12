# 15-minute commodities board (Kalshi)

One page with a card for every Kalshi **15-minute commodity** market, the same set as
kalshi.com/category/commodities/frequency/fifteen_min: Gold, Silver, WTI Oil, Copper, Natural Gas, Palladium and
Platinum, plus any new one Kalshi adds. Each card runs the WTI coach's **settlement caller** (v12.2):
- one Up/Down call per window, locked once made;
- the confidence meter;
- a **Wins | Losses** record next to what Kalshi's own odds expected.

It is a separate app (package `cm15m`, folder `kalshi-commodities`, port **8790**). It shares no code, data or
records with the WTI, natural gas or gold coaches, and they keep running as they are.

It is a coach, not a bot: it never places orders and needs no Kalshi API key.

## The page

- **The cards**, in Kalshi's order. Each shows:
  - the target and the live price, and the move between them (in ¢ under $200, in $ above);
  - the countdown and Kalshi's Up/Down odds;
  - the **call box**: STUDYING, then WATCHING, then e.g. **UP AT SETTLEMENT**, locked;
  - the **meter**: the model's chance on a Down ◀ 50% ▶ Up bar, with dashed lines at the 72% call line, a faint band
    for the last 30 seconds, and Kalshi's odds as a gold tick;
  - the market's record and its last 10 calls.
- **The top strip**: the combined record across all markets and each market's line, so it is clear which markets
  the caller reads best.
- **Click a card** for its detail view:
  - the price-vs-target chart;
  - what the model sees;
  - the call history;
  - the stats: the settlement timing check, accuracy vs Kalshi, and learned volatility.
- **🔔 on a card** mutes that market's notifications (remembered). Notifications name the market:
  "GOLD settles UP".

## How each market is handled

- **Same rules as the WTI coach:**
  - 3-minute study;
  - a call when one side holds 72% for 10 s, otherwise its lean at 2:00 left;
  - no call for a window joined late or in its last minute;
  - no confident call without Kalshi's odds;
  - settlement at the close (checked every window);
  - learned volatility by ET hour and around reports;
  - the learner without trend features.
- **Scale is automatic per market:**
  - The tie rule comes from how Kalshi writes the targets: gold $4,173.29 → half a cent, silver $60.707 → half a
    tenth of a cent, gas/copper $3.34148 → half the fifth decimal.
  - The volatility limits are set from the price, so $3 gas and $4,000 gold are both measured, never clipped.
- **Report calendar by kind:**
  - Oil: EIA Wednesday 10:30, API Tuesday 16:30, Baker Hughes Friday 13:00.
  - Gas: EIA storage Thursday 10:30, Baker Hughes.
  - Metals: jobless claims Thursday 8:30 and FOMC decisions at 14:00.

  Windows holding a report get no new calls. CPI, jobs and PCE dates are not built in, because they could not be
  checked against the official schedules. Add them per market in `config.toml` (see `config.example.toml`).
- **Kalshi's rate limit:** one shared connection, at most 10 requests a second. Each market refreshes every
  2 seconds, staggered. The page footer shows if Kalshi ever slows the board down.

## Setup (macOS, next to the other coaches)

```bash
unzip -o ~/Downloads/kalshi-commodities-v1.zip -d ~
cd ~/kalshi-commodities
source ~/.venv/bin/activate
python -m cm15m probe     # first time: lists the 15-minute series and each live target (paste it to Claude)
python -m cm15m serve     # the board on http://127.0.0.1:8790
```

The first window per market may say "joined it too late". Calls start with the next one. `python -m cm15m demo`
runs the board on seven simulated markets at their real price scales (offline). `pytest` runs the tests.

## Upgrading

Unzip a new version over the folder with `unzip -o`. Never delete the folder: `data/` holds every market's
record and what it has learned.
