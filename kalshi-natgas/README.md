# Natural gas 15-min coach (Kalshi `KXNATGAS15M`)

The same local coach as `../kalshi-wti` (v12.2), for Kalshi's **Natural Gas 15-Minute** Up/Down market. It is a
separate copy with its own package (`ng15m`), database and dashboard, so nothing done here can affect the WTI
coach. Both can run at once: WTI on `http://127.0.0.1:8787`, natural gas on `http://127.0.0.1:8788`.

It is a coach, not a bot: it never places orders and needs no Kalshi API key.

## What carries over from the WTI coach

- **Kalshi's own live price series** for the window (`/live_data/events/{event}`), the Pyth price Kalshi settles
  on, as the feed. A Hyperliquid natural gas perp is the fallback only if one exists; it never falls back to oil.
- **The model:** `P(Up) = Φ((price − target + tie) / (σ·√time left))`. σ is the realized volatility of recent
  moves, corrected per ET hour by what each hour has really done (learned after each window).
- **3-minute study period** at the start of each window.
- **The settlement call:** one Up/Down call per window, locked once made, scored when Kalshi settles it. It calls
  when one side holds 72% for 10 s, otherwise it calls its best lean at 2:00 left. Its **Wins | Losses** record
  sits next to what Kalshi's own odds expected.
- **Confidence meter** under the call: Down ◀ 50% ▶ Up, the 72% call lines, a 30-second range band, and Kalshi's
  odds as a gold tick.
- **Signal card, trade plans and position card** with sell zones.
- **Settlement timing:** at the close, as measured on WTI. The Stats tab checks both moments on every gas window
  and says so if a minute later ever fits better.

## What is different for natural gas

- **Scale:** gas trades near $3 with three-decimal targets (`$3.245`). Prices show three decimals, moves show
  tenths of a cent, and a tie is half a tenth of a cent (`tie_adj = 0.0005`). The volatility floor and cap are
  sized for gas (0.00002–0.005 $/√s), not for oil.
- **Calendar:** the **EIA Weekly Natural Gas Storage Report (Thursday 10:30 ET)** and Baker Hughes (Friday
  13:00 ET). Windows holding them get no new calls, and their volatility is learned separately. Holiday shifts
  go in `[trading] extra_events`, e.g. `"2026-11-25 12:00 EIA gas storage (holiday)"`.
- **TradingView tab:** `NYMEX:NG1!`, the front-month future.
- **Record:** its own. The 72% threshold is the WTI coach's tuned value. Gas gets retuned once it has its own
  settled windows.

## Setup (macOS, next to the WTI coach)

It uses the same Python environment as the WTI coach:

```bash
unzip -o ~/Downloads/kalshi-natgas-v1.zip -d ~
cd ~/kalshi-natgas
source ~/.venv/bin/activate
python -m ng15m probe     # first time: dumps the live gas market to fixtures/probe-*/ (to confirm its fields)
python -m ng15m serve     # dashboard on http://127.0.0.1:8788
```

The first ~30 minutes are warm-up. Leave it running; each window appears by itself.

Other commands: `python -m ng15m demo` (offline simulated gas market), `python -m ng15m replay` (re-score recorded
windows), `pytest` (tests).

## Upgrading

Unzip a new version over the folder with `unzip -o`. Never delete the folder: `data/ng15m.db` (your record and
everything learned) and `config.toml` live inside it.
