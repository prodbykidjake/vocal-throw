# WTI 15-min coach (Kalshi `KXWTI15M`)

A local app that follows Kalshi's **WTI Oil 15 min** up/down market by itself, watches the live WTI price
against the window's target, estimates the chance of Up/Down with an honest confidence label, and tells
you in plain English when to **BUY** (which side, limit price, size), **SELL**, **HOLD**, or **WAIT**
and what would change the answer. It records every window and learns from settled outcomes.

It is a coach, not a bot: **you** place trades in the Kalshi app and tell the dashboard what you did.
Nothing here needs a Kalshi API key.

> Reality check: 15-minute markets are close to efficient and fees are steep for coin-flip contracts.
> Expect WAIT most of the time. The point is discipline, exact fee/edge math, and a running record of
> whether the model's confidence is real before you risk money on it.

## How it works

| Piece | What it does |
| --- | --- |
| Kalshi public REST API | finds the live `KXWTI15M` window (target, open/close time, Up/Down bid/ask), picks up the result after settlement, reads the series' settlement source and fee parameters |
| Hyperliquid WTI perp feed | free real-time WTI price used for the chart line and the model (not Kalshi's settlement feed, see Caveats) |
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
- **Signal card**: BUY/SELL/HOLD/WAIT, the headline, and the details (gap, time left, expected move,
  edge after fees, size, exit plan, and "would buy if…").
- **Your position** (built for scalping): tap **UP** or **DOWN**, the price box fills with the live ask
  (edit it if your fill differed), type the dollars you spent, press Enter. The card then tracks it like
  Kalshi's sell sheet: shares, live sell price, **cash out** after fees and P&L, updating every second,
  with a box that says **SELL NOW** (lit up), **SELL AT x¢** (a concrete target where the market would
  have caught up to the model), **HOLD TO SETTLEMENT**, or HOLD. "I sold at the bid" records the exit at
  the live price in one click; "I sold at this" takes the price you actually got.
  SELL NOW fires when the market pays more than the model's value, when a profitable position's bid rolls
  over 25% from its high since entry, when the take-profit target is hit with no edge left, or when the
  model flips against you and the bid is still worth taking.
- **History**: every window with target, result, model Up at 10 and 3 minutes, market Up at close,
  whether the feed agreed with the settlement, and what a paper trade would have made.
- **Stats**: Brier score (0.25 = coin flip; lower is better) for model vs market at 10/5/2/1 minutes
  left, calibration table (when it said 60-70%, how often Up?), paper P&L by confidence, learner weights.

Treat the coach as untested until Stats shows a model Brier score at least as good as the market's over
100+ windows and paper P&L after fees is positive.

## Configuration (`config.toml`)

See `config.example.toml`. The ones that matter: `bankroll`, `edge_min` (default 5 points),
`kelly_fraction`, `max_contracts`, `confident_margin`, `min_warmup_minutes`, `no_entry_first_s`,
`late_entry_s`, `stop_prob`, `profit_target`, `hold_to_settle_prob`, and `[feed] hyperliquid_symbol`
if auto-detection picks the wrong market.

## Caveats

- **Feed ≠ settlement feed.** Kalshi settles on Pyth's PYTHOIL index. The free Hyperliquid `xyz:CL` perp
  tracks it closely but not exactly. Because the next target equals the previous settlement price, the
  app measures the feed's error in dollars at every window (Stats tab, both at the close and one candle
  later) and widens the model's uncertainty by the typical error. Pyth itself now needs a paid/trial API
  key; a Pyth adapter can be added later if the measured error turns out to matter.
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
wti15m/app.py        FastAPI server (JSON + Server-Sent Events) ; ui/ is the dashboard
wti15m/store.py      SQLite schema and queries ; wti15m/replay.py offline evaluation
wti15m/probe.py      raw API dump for the first live run ; wti15m/sim.py simulated Kalshi for demo/tests
```
