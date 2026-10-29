# Gold 15-min coach (Kalshi `KXGOLD15M`)

The same local coach as `../kalshi-wti` (v12.2), for Kalshi's **Gold 15-Minute** Up/Down market. It is a separate
copy with its own package (`gold15m`), database and dashboard, so nothing done here can affect the WTI or natural
gas coaches. All three can run at once:
- WTI on `http://127.0.0.1:8787`;
- natural gas on `8788`;
- gold on `http://127.0.0.1:8789`.

It is a coach, not a bot: it never places orders and needs no Kalshi API key.

## What carries over from the WTI coach

- **The feed:** Kalshi's own live price series for the window (`/live_data/events/{event}`), the Pyth price Kalshi
  settles on. Hyperliquid's `xyz:GOLD` perp can be turned on as a fallback (`[feed] fallback = "hyperliquid"`). It
  is off until its gap to Pyth has been measured.
- **The model:** `P(Up) = Φ((price − target + tie) / (σ·√time left))`. σ is the realized volatility of recent
  moves, corrected per ET hour by what each hour has really done (learned after each window).
- **3-minute study period** at the start of each window.
- **The settlement call:** one call per window, locked once made. It calls when one side holds 72% for 10 s,
  otherwise it calls its best lean at 2:00 left. Its **Wins | Losses** record sits next to what Kalshi's own odds
  expected.
- **Confidence meter** under the call.
- **Signal, plan and position cards.**
- **Settlement timing:** at the close, with the Stats tab checking both moments on every gold window.

## What is different for gold

- **Scale:** gold trades in the thousands and moves dollars per window. Prices show as `$4,012.35`, and gaps and
  expected moves are shown in dollars (`$2.00 above the target`, `±$4.10`). Contract prices stay in ¢. The
  volatility floor and cap are sized for gold (0.005–2.0 $/√s). The oil cap of 0.05 would have clipped gold's real
  moves and made every call overconfident.
- **Calendar:** gold moves on U.S. macro data, not oil reports. Built in:
  - the weekly **initial jobless claims (Thursday 8:30 ET)**;
  - the **FOMC rate decisions (2:00 PM ET)** for the rest of 2026 and the announced early-2027 meetings.

  Monthly releases (**CPI, the jobs report, PCE**) are not built in, because their dates could not be checked
  against the official schedules. Add them as `[trading] extra_events`, e.g.
  `["2026-11-13 08:30 CPI", "2026-11-06 08:30 Jobs report"]`. Windows holding a release get no new calls and their
  own learned volatility.
- **Tie rule:** `tie_adj = 0.005`, assuming cent targets. The first `probe` confirms the target's decimals.
- **TradingView tab:** `TVC:GOLD`.
- **Record:** its own. The 72% threshold is the WTI coach's tuned value. Gold gets retuned once it has its own
  settled windows.

## Setup (macOS, next to the other coaches)

```bash
unzip -o ~/Downloads/kalshi-gold-v1.zip -d ~
cd ~/kalshi-gold
source ~/.venv/bin/activate
python -m gold15m probe     # first time: shows the live gold market (paste the output to Claude)
python -m gold15m serve     # dashboard on http://127.0.0.1:8789
```

The first ~30 minutes are warm-up. Other commands: `python -m gold15m demo` (offline simulated gold market),
`python -m gold15m replay`, `pytest`.

## Upgrading

Unzip a new version over the folder with `unzip -o`. Never delete the folder: `data/gold15m.db` (the record and
everything learned) and `config.toml` live inside it.
