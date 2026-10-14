# 15-minute commodities board (Kalshi)

One page with a card for every Kalshi **15-minute commodity** market, the same set as
kalshi.com/category/commodities/frequency/fifteen_min: Gold, Silver, WTI Oil, Copper, Natural Gas, Palladium and
Platinum, plus any new one Kalshi adds. Each card runs the WTI coach's **settlement caller** (v12.2):
- one Up/Down call per window, locked once made;
- the confidence meter;
- a **Wins | Losses** record next to what Kalshi's own odds expected.

**WTI, Natural Gas and Gold run their coaches' own engines**:
- WTI: the WTI coach's v12 engine;
- Natural Gas: the gas coach's v1.0.1 engine;
- Gold: the gold coach's v1 engine.

Each runs exactly as in its own app: the same model, study period, learned volatility, settlement timing, learner,
and call rules. Silver, Copper, Palladium and Platinum run the board's own settlement-caller desk (the same call,
with scale worked out per market). The engines are copies inside this folder (`wti15m/`, `ng15m/`, `gold15m/`). On
the board only their settlement calls notify; their signal-card BUY/SELL alerts stay quiet.

It is a separate app (package `cm15m`, folder `kalshi-commodities`, port **8790**). It shares no code, data or
records with the WTI, natural gas or gold coaches, and they keep running as they are.

It is a coach, not a bot: it never places orders and needs no Kalshi API key.

## EDGE: the buy signal (v1.2)

Two months of Kalshi history (Aug 14 - Oct 13 2026, pulled with `kalshi-wti/tools/backfill.py`; second-by-second
settlement prices for 1,600-4,100 windows per market) replayed through each card's own engine said:

- **The settlement calls are honest but not a money edge.** Over ~17,000 replayed calls they won about as often as
  they said (WTI 78.5% vs 78.0% expected), but by the time a call fires Kalshi's price already says the same thing.
  Bought at the ask about a minute after the call (the price real fills got), every market came out between -0.9c and
  +0.6c a contract after fees: break-even. The calls stay on the cards as forecasts.
- **What made money: buying only when the model is clearly more confident than Kalshi's price.** At each whole
  minute from 3:00 to 13:00 into a window, if the model's chance for its side is at least 55% and at least
  **6 points above that side's ask**, the card shows **EDGE · BUY UP/DOWN at N¢ or less** and notifies. Chosen on WTI's
  first 19 days, it made +6.2c a contract on WTI's last 12 days, and, never tuned on them, gold +5.5c, silver +6.4c,
  platinum +3.6c, palladium +3.5c, copper +3.3c, natural gas +2.2c (fills a full minute later, fees included). It
  fires in about one window in six. History is not a promise: the EDGE record on the board keeps the honest count.
- **Hold to settlement.** Stop-losses, take-profits and "cash out when it turns" all lost money in history; so did
  resting a bid below the ask (the orders that fill are the ones going wrong), buying the cheap side, and clicking
  faster (the ask barely moves in the minute after a signal).

The EDGE box under each card's meter shows what it is watching (model vs ask, the gap, the next minute check), a
signal when one fires (one per window), and its result; the top strip shows the EDGE record and profit per contract
across all markets. Signals are kept in `data/edges.db`. The margin and minimum are `[board] edge_gap` (0.06) and
`edge_min_p` (0.55).

## The page

- **The cards**, in Kalshi's order. Each shows:
  - the target and the live price, and the move between them (in ¢ under $200, in $ above);
  - the countdown and Kalshi's Up/Down odds;
  - the **call box**: STUDYING, then WATCHING, then e.g. **UP AT SETTLEMENT**, locked;
  - the **meter**: the model's chance on a Down ◀ 50% ▶ Up bar, with dashed lines at the 72% call line, a faint band
    for the last 30 seconds, and Kalshi's odds as a gold tick;
  - the market's record and its last 10 calls;
  - the **EDGE box** (above).
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
- **Kalshi's rate limit:** one shared limiter, at most 14 requests a second. The three coach engines refresh every
  second, as in their own apps, and the other four markets every 2 seconds, staggered. The page footer shows if Kalshi ever slows the board down.

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
