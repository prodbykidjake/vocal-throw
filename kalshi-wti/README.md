# WTI 15-min coach (Kalshi `KXWTI15M`)

A local app that follows Kalshi's **WTI Oil 15 min** up/down market by itself, watches the live WTI price
against the window's target, estimates the chance of Up/Down with an honest confidence label, and tells
you in plain English when to **BUY** (which side, limit price, size), **SELL**, **HOLD**, or **WAIT**
and what would change the answer. It records every window and learns from settled outcomes.

By default it is a coach, not a bot: **you** place trades in the Kalshi app and tell the dashboard what you did,
and nothing needs a Kalshi API key. Since v9 it can also **trade for you** (`[auto]` in the config, see below):
with an API key it buys the card's calls and sells when the position card says SELL NOW, with hard dollar limits
and a PAUSE/STOP button, after a dry run that shows every order it would have sent.

> Reality check: 15-minute markets are close to efficient and fees are steep for coin-flip contracts.
> Expect WAIT most of the time. The point is discipline, exact fee/edge math, and a running record of
> whether the model's confidence is real before you risk money on it.

## How it works

| Piece | What it does |
| --- | --- |
| Kalshi public REST API | finds the live `KXWTI15M` window every few seconds (target, open/close time), reads the live **order book** every second for Up/Down bid/ask (the market-list prices can lag the real book by a lot), picks up the result after settlement, reads the series' settlement source and fee parameters |
| Price feed | **Kalshi's own live price series** for the window (`/live_data/events/{event}`), the Pyth PYTHOIL value Kalshi draws as "Now" and settles on, polled every second, free and unauthenticated. The Hyperliquid WTI perp (`xyz:CL`) runs alongside as an automatic fallback whenever that series goes stale |
| Model | `P(Up) = Φ((price − target) / (σ·√time left))` with σ = realized volatility of recent price changes, then a tiny online logistic learner nudges it using what settled windows taught it (its weight grows with data, starting at 0) |
| Decision engine | edge = model chance − contract ask − Kalshi fee; BUY only above your minimum edge; ¼-Kelly sizing; exit rules for a position you report; every WAIT says exactly what would turn it into a trade |
| Dashboard | `http://127.0.0.1:8787`: live chart with target and expected range, model vs market, signal card, your position, window history, stats (Brier score vs the market, calibration, paper P&L) |
| Storage | SQLite in `data/` (git-ignored): ticks, quotes, model snapshots every 15 s, signals, paper trades, your trades, settlements |

## Setup (macOS)

Needs Python 3.11 or newer (`brew install python` or python.org).

```bash
cd kalshi-wti
python3 -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt
cp config.example.toml config.toml     # edit bankroll / edge_min if you like
```

## What the live APIs look like (confirmed 2026-10-07 with `python -m wti15m probe`)

- Series `KXWTI15M`: `fee_type=quadratic`, `fee_multiplier=1.0`, settlement source **Pyth - WTI**
  (`Commodities.Index.PYTHOIL/USD`).
- Rules: *"If the close price of the 1-minute candlestick for WTI Oil at 7:00 PM EDT is at least the close
  price of the 1-minute Pyth PYTHOIL candlestick at 6:45 PM EDT, the market resolves to Yes."*
  So the target is the close of the 1-minute candle at the open time, settlement is the close of the
  1-minute candle at the close time (about 60 s after trading stops; `settle_lag_s` in the config), a tie
  pays Up, and **the next window's target is the previous window's settlement price**. The app uses that
  to measure the feed's error exactly.
- Market fields: prices only as dollar strings (`yes_bid_dollars`, …, sub-cent values like `0.012`),
  sizes as `*_fp` strings, `floor_strike` holds the target, `status` is `open` then `finalized`,
  `expiration_value` holds the settlement price, tickers look like `KXWTI15M-26OCT071900-00`.
- Hyperliquid: dex `xyz`, symbol `xyz:CL`, `allMids` keys carry the `xyz:` prefix, 1-minute candles
  with `t`/`T`/`c` fields. Its mid tracked the Kalshi market's implied direction at probe time.

`python -m wti15m probe` re-dumps all of this to `fixtures/probe-<timestamp>/` if anything changes.

## Running

```bash
python -m wti15m serve        # dashboard, opens your browser
python -m wti15m quotes       # 20 s side-by-side check of Kalshi's price sources (list / market / order book)
python -m wti15m livedata     # 20 s side-by-side check of Kalshi's live price series vs the Hyperliquid mid
python -m wti15m watch        # terminal-only live view
python -m wti15m demo         # offline demo on a simulated market (90-second windows)
python -m wti15m replay       # re-score recorded windows (Brier model vs base vs market)
python -m wti15m replay --retrain --save   # walk-forward retrain the learner from scratch
pytest                        # unit tests (offline, fixtures)
```

Leave `serve` running. The first ~30 minutes are warm-up (it needs price history to size the odds);
after that, BUY/SELL signals trigger a macOS notification and a sound. Each 15-minute window appears
automatically; nothing to click.

## Reading the dashboard

- **Target** is the WTI price when the window opened. Up pays if the close price is at or above it.
- **Model vs Kalshi odds**: the model's Up chance next to the market's. Confidence labels: `confident`
  (≥ 20 points from 50%), `lean`, `coinflip`, `warming_up`, `stale` (feed too old to trust).
- **Signal card** (built for scalping): the app does not re-decide every second. When a setup has
  held for a few seconds it commits to a **plan**: `BUY UP · up to 23¢ · $20 (≈ 87 shares) · sell at 45¢
  (~50% chance) · ≈ +$19 if it gets there`, with a status line (`valid: ask 22¢ ≤ 23¢ · buy now`). The plan
  stays on screen until you report a buy ("I bought this plan" pre-fills the position form), the ask runs 4¢
  past the limit for 10 s (**missed**), the thesis breaks (**cancelled**: the model's chance for that side
  falls to the stop or to 60% of what it was, or the market disagrees strongly), or the window ends. After a
  missed or cancelled plan it says so and waits 30 s before the next one.
  Dollar amounts come from the model's confidence: lean = half a unit, confident = 1 unit, strong = 2,
  near-certain = 3 (unit $10, max $50 per window by default; `learn_unit` uses the median of your own
  recent buys instead). A cheap contract below the lowest tier is still a plan when the edge after fees is
  there, tagged **longshot** (a 32% chance priced at 14¢): half a unit, a full unit when the model's chance
  is at least double the price. **Scalp calls** (since v8.3): on contracts at or under 55¢ the card also calls
  a side when the simulated paths give its price at least a 40% chance of reaching a sell level 8¢ (or 30%)
  higher before the close and the model or the last few minutes of price action lean its way, even with no
  settlement edge; these show as `BUY DOWN · limit 41¢ · $10 · scalp to 54¢ (~70% chance)` and are sized at
  one unit (half a unit under a 45% chance). The sell target is the model's fair exit, lowered to a price the simulated
  paths (below) reach with at least `plan_min_chance` (30%); the chance is printed next to it. Between plans
  the card shows the plain analysis, "setup forming" while a candidate confirms, and the exact reason when a
  confirmed setup cannot become a plan (not enough room to scalp, too little chance of reaching the target,
  model and Kalshi's odds too far apart). A refusal is held on the card instead of being reworded every
  second, and a setup refused in the last minute does not start a new countdown.
- The **quick-scalps card** of v8.4–v9 (cheap sides, a few cents within minutes) is retired: it lost in live use.
  The code stays behind `quick_scalps` / `take_quick`, both off; the dashboard no longer shows it and the auto
  trader never buys from it.
- **Your position** (built for scalping): tap **UP** or **DOWN**, the price box fills with the live ask
  (edit it if your fill differed), type the dollars you spent, press Enter. The card then tracks it like
  Kalshi's sell sheet: shares, live sell price, **cash out** after fees and P&L, updating every second.
  The box above them is forward-looking (since v8): **HOLD · SELL BETWEEN 39¢ – 75¢** with a line under it
  such as `now 22¢ · ~50% chance of 39¢ · ~20% chance of 75¢ · ~80% chance of getting back to 25.6¢ or
  winning`. The chances come from 2,000 simulated price paths from the current market price to the close,
  using the model's volatility: the low end of the zone is the price reached in half the paths, the high end
  the price reached in one in five; the "getting back to breakeven" chance runs to the very end of trading
  and is never below the chance of simply winning (a win pays $1). The zone is set once (from the plan if
  you took one) and never chases the price upward; it comes down only when the old low has become
  unlikely, and never below breakeven.
  **SELL NOW** (lit up) fires when the bid enters the zone, when a profitable run rolls over 25% from its
  high (and then re-arms only after a new high, so it does not nag on every dip), in the last 30 s when a
  profit is on the table and the zone is out of reach ("take the profit before the close"), or, the only
  loss-taking case, when the chance of getting back to breakeven is at or under `give_up_prob` (10%) and
  the sale still returns something worth clicking for; otherwise a hopeless position is called a lottery
  ticket and left alone. Every SELL NOW latches: a 1¢ wobble back across its threshold does not flip the
  box back to HOLD, a SELL NOW appears only once its rule has held for two seconds, and it stays until the
  rule has been gone for two seconds; the paper trade that mirrors each plan follows the same rules. **HOLD TO SETTLEMENT** appears only in the last two minutes with ≥ 90% and selling
  clearly worse. "I sold at the bid" records the exit at the live price in one click; "I sold at this"
  takes the price you actually got.
- **History**: every window with target, result, model Up at 10 and 3 minutes, market Up at close,
  whether the feed agreed with the settlement, and what a paper trade would have made.
- **Stats**: Brier score (0.25 = coin flip; lower is better) for model vs market at 10/5/2/1 minutes
  left, calibration table (when it said 60-70%, how often Up?), paper P&L by confidence, learner weights,
  and **plan results**: how often each tier's sell target was reached and what taking every plan at its
  limit would have made after fees.

Treat the coach as untested until Stats shows a model Brier score at least as good as the market's over
100+ windows and paper P&L after fees is positive.

## Auto trading (v9)

The app can place the orders itself, no button presses. Setup, in this order:

1. At `kalshi.com/account/profile` → **API Keys → Create API key**. Key type: Ed25519 (the default) or RSA, both
   work. Permissions: tick **Read all data** and the granular **Trade** (`write::trade`); leave **Full access** off
   so the key can never move money out of the account. Note the key id and keep the private-key file Kalshi
   downloads (a `.txt`) somewhere outside this folder, e.g. `~/kalshi-key.txt`. Never commit or share it
   (`config.toml`, `*.pem` and `*.key` are git-ignored). Anyone with the file can trade your account.
2. In `config.toml`:
   ```toml
   [auto]
   enabled = true
   dry_run = true
   api_key_id = "xxxxxxxx-xxxx-..."
   private_key_path = "~/kalshi-key.txt"
   ```
   For Kalshi's play-money **demo exchange** (separate account and keys at `demo.kalshi.co`), also set
   `[kalshi] base_url = "https://external-api.demo.kalshi.co/trade-api/v2"`.
3. `python -m wti15m auth` prints your balance and open positions. If it says 401/403, the key id or file is wrong
   (or the Mac's clock is off).
4. `python -m wti15m serve` as usual. With `dry_run = true` the **Auto trading** card runs the whole thing against a
   simulated account on the real quotes: it "buys" plans, "sells" on SELL NOW, keeps a P&L. Nothing
   is sent to Kalshi. Watch it for a day. When the log looks right, set `dry_run = false` and restart: the card turns
   red and says LIVE.

What it does every second (`wti15m/autotrader.py`):

- **Buys** one order when the signal card has a valid plan (ask at or a hair over the limit): a limit order at the
  plan's limit for the plan's dollars, capped by `max_order_dollars`. It rests for `buy_ttl_s` (20 s) and is
  cancelled if unfilled, if the plan dies, or if the window ends. A fill opens the position card at the real fill
  price and fee, with the plan's sell zone. Nothing else is bought.
- **Sells** when the position card says SELL NOW: one immediate-or-cancel, reduce-only order down to
  `bid − sell_slip_cents` (3¢), so it takes whatever is on the book. A partial fill takes part of the position off and
  the next SELL NOW sells the rest. A position the card wants to hold to settlement is left alone; Kalshi pays it out.
- **Syncs** your balance and Kalshi position on the live window every `sync_s`. A position you opened in the Kalshi
  app is adopted (so the sell rules apply to it); one you sold there is closed here. It never buys while Kalshi
  already shows a position on the window, and never while an order is open.
- **Limits** (all in `[auto]`): `max_order_dollars` (20), `max_window_dollars` (50), `max_day_dollars` (300),
  `max_day_loss` (40: realized loss in a day that stops buying until tomorrow), `max_losses_in_a_row` (4: stops
  buying until you press RESUME). `take_plans = false` turns buying off while sells keep running.
- **PAUSE** stops new buys (sells still run, so an open position is still managed). **STOP** cancels the open order
  and pauses. The card shows the balance, today's buys and P&L, the open order, the last fills, and the exact reason
  when it is not buying. Live mode refuses to start unless the dashboard is bound to `127.0.0.1`.

Orders go to Kalshi's v2 order endpoint (`/portfolio/events/orders`, bid/ask on the Yes price, fixed-point
strings, checked against Kalshi's OpenAPI spec 3.34.0); if that endpoint is unavailable the client falls back to
the legacy `/portfolio/orders` by itself. Fractional contracts are used (`fractional = true`); if Kalshi rejects
the count it switches to whole contracts. Fill prices and fees come from the fills ledger, which states both the
Yes and the No price of every fill. Every order, fill and error is logged (History tab and the card). The first
live orders still deserve watching.

## Configuration (`config.toml`)

See `config.example.toml`. For plans and sizing: `unit_dollars`, `max_trade_dollars`, `learn_unit`,
`min_scalp_cents`, `confirm_s`, `min_hold_s`, `miss_margin_cents`, `miss_seconds`, `cooldown_s`,
`settle_advice`, `plan_min_chance`. For the sell zone: `zone_low_prob`, `zone_high_prob`, `zone_refresh_s`,
`zone_drop_prob`, `zone_exclude_last_s`, `give_up_prob`, `give_up_min_cash`, `mc_paths`. The others that
matter: `bankroll`, `edge_min` (default 5 points), `kelly_fraction`, `max_contracts`, `confident_margin`,
`min_warmup_minutes`, `no_entry_first_s`, `late_entry_s`, `stop_prob`, `hold_to_settle_prob`,
`basis_min_windows`/`basis_min_t`, and `[feed] hyperliquid_symbol` if auto-detection picks the wrong market.

## Caveats

- **Feed.** Since v5 the primary feed is Kalshi's own live series for the window, which is the settlement
  feed itself, so the dashboard's "Now" should match the Kalshi app to the cent. The feed badge says
  `kalshi-live`. If that series is unavailable or stale for more than `stale_after_s`, the app switches to
  the Hyperliquid `xyz:CL` perp and the badge turns amber with `FALLBACK`. Hyperliquid can sit several cents
  away from Pyth, which matters when the price is near the target: the app measures that gap at every
  window boundary, widens the model's uncertainty by the typical error, shows the price Kalshi's own odds
  imply, and refuses to call anything "confident" when model and market disagree by more than 40 points.
  It also shifts the feed's price by the measured bias ("feed adj" under the price), but only once the bias
  is consistent: at least `basis_min_windows` (6) measured windows and `basis_min_t` (1.5) standard errors
  from zero, per feed. Until then the Stats tab shows the measured number as "not applied". A 2¢ wobble
  after three windows must not move every call by 2¢.
  `python -m wti15m livedata` prints the Kalshi series next to the Hyperliquid mid for 20 s.
- **Settlement candle lag.** `settle_lag_s` (default 60) is added to the model's horizon because the
  settlement candle closes about a minute after trading stops. The Stats tab shows which lag fits the
  measured errors better; set it to 0 if the 0-second column is clearly smaller.
- **Fees.** Taker fee = `multiplier × 0.07 × contracts × price × (1 − price)`, rounded up to the cent per
  order. At 99¢ that 1¢ round-up wipes out the gain, which is why the coach says WAIT in the last seconds.
- **Learning.** The learner is deliberately small (9 weights) and shrunk toward the base model until it
  has seen about 100 windows. It can learn that the market is better than the base model, in which
  case signals get rarer. That is the honest outcome, not a bug.
- **Not financial advice.** Paper-trade first.

## Layout

```
wti15m/kalshi.py     Kalshi REST client, market parsing, live-window tracker, settlement polling
wti15m/feeds/        PriceFeed interface, Hyperliquid adapter, simulated feed
wti15m/model.py      volatility estimator, digital-option probability, online calibrator, confidence
wti15m/decision.py   edge/fee/Kelly logic and the plain-English BUY/SELL/HOLD/WAIT coaching
wti15m/engine.py     1 Hz loop joining everything, recording, paper trades, learning at settlement
wti15m/broker.py     your Kalshi account: signed requests (RSA-PSS), orders, fills, positions, balance; SimBroker for dry runs
wti15m/autotrader.py places the cards' calls automatically, with the risk limits and the PAUSE/STOP switch
wti15m/app.py        FastAPI server (JSON + Server-Sent Events) ; ui/ is the dashboard
wti15m/store.py      SQLite schema and queries ; wti15m/replay.py offline evaluation
wti15m/probe.py      raw API dump for the first live run ; wti15m/sim.py simulated Kalshi for demo/tests
```
