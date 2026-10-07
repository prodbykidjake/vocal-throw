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

## First run: confirm the API shapes

Kalshi's and Hyperliquid's APIs were researched, not exercised, when this was written (the development
environment could not reach either). Run the probe once and send the output back to Claude:

```bash
python -m wti15m probe
```

It prints the series' settlement source and fee type, the live market with its field names, the
Hyperliquid WTI symbol it detected, and writes the raw JSON to `fixtures/probe-<timestamp>/`.

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
- **Your position**: after you buy in the Kalshi app, enter side/qty/price (or "use the signal").
  The coach then switches to exit advice: take profit, cut if the model flips, or hold to settlement.
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

- **Feed ≠ settlement feed.** Kalshi settles this series on a Pyth WTI price series (confirm in the
  Rules tab, which shows `settlement_sources` from Kalshi's API). The free Hyperliquid perp tracks it
  closely but not exactly. The app records the feed's price at each close and whether it agreed with the
  settlement; the model widens its uncertainty by the measured disagreement when the price sits near
  the target. Pyth itself now needs a paid/trial API key; a Pyth adapter can be added later.
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
