# 15-minute FX board (Kalshi)

One page with a card for every Kalshi **15-minute currency pair** (kalshi.com/category/financials/frequency/fifteen_min):
EUR/USD, GBP/USD, USD/JPY, AUD/USD, USD/CAD and USD/MXN (KXEURUSD15M ... KXUSDMXN15M), plus any new pair Kalshi adds.
The Treasury-yield, S&P 500, Nasdaq and Dow 15-minute series are not currency pairs and get no card.

Each card runs the commodities board's **settlement caller**, with each pair's own rules chosen on its Kalshi history:
- one Up/Down call per window, locked once made;
- the **Standard / Strict / Very strict** switch, with all three modes recorded side by side;
- **PULL OUT** (30%), the early tag and the dip note;
- **EDGE** as a test-only record (no buy alerts, see below);
- a **Wins | Losses** record next to what Kalshi's own odds expected.

It is a separate app (package `fx15m`, folder `kalshi-fx`, port **8794**). It shares no code, data or records with the
commodities or crypto boards, and runs next to them. It is a coach, not a bot: **it never places orders** and needs no
Kalshi API key.

## How these markets settle

A window resolves **Up** when Pyth Network's price of the pair at the window's close is above the target, the Pyth price
at the open. It is one price at one second, like the commodities (not crypto's minute average). Kalshi's own chart for
each window is that Pyth price, one point a second, and the board reads it there (no key needed). Targets are written to
5 decimals (3 for USD/JPY), so gaps and moves read in **pips**: 0.0001, or 0.01 for yen.

No call is made in a window that holds a scheduled U.S. release, when the dollar jumps:
- U.S. jobless claims (Thursdays 8:30 AM ET);
- the jobs report (the first Friday of the month, 8:30 AM ET, as projected);
- CPI and FOMC decisions.

Central-bank decisions for the other currency (ECB, BoE, BoJ, RBA, BoC, Banxico) are not built in, because their dates
could not be checked. Add them per pair in `config.toml`, for example:

```toml
[board.extra_events]
eurusd = ["2026-12-17 08:15 ECB decision"]
```

## Each pair's call rules

Each pair's Kalshi history was replayed second by second through this board's own model:
- EUR/USD, GBP/USD and USD/JPY: 23 Sep to 18 Nov 2026, 3,716 windows each;
- AUD/USD and USD/CAD: 7 Oct to 18 Nov, 2,777 each;
- USD/MXN has had no windows yet, so it uses the defaults.

For each pair and mode, the lowest bar was kept whose calls won the mode's target on the **first 60% of days**
(Standard 90%, Strict 94%, Very strict 97%). The table shows how those rules did on the **last 40%**, which were never
used to choose them:

| pair | Standard | Strict | Very strict |
|---|---|---|---|
| EUR/USD | 90.4% won, 88% of windows called | 93.6%, 80% | 97.2%, 69% |
| GBP/USD | 90.3%, 87% | 93.7%, 78% | 96.5%, 68% |
| USD/JPY | 90.4%, 86% | 94.9%, 76% | 98.4%, 61% |
| AUD/USD | 90.3%, 86% | 94.3%, 79% | 97.5%, 67% |
| USD/CAD | 87.0%, 91% | 91.7%, 82% | 96.5%, 70% |
| **all five** | **89.8%, 88%** | **93.7%, 79%** | **97.2%, 67%** |

- **USD/CAD** fell short of its targets on the later days; Strict or Very strict suits it better.
- **Kalshi prices FX tightly.** Bought at the ask, the calls lost about a cent (Standard −1.3¢, Strict −1.0¢, Very
  strict −0.1¢ a call). The calls are forecasts you can trust, not an edge on Kalshi's price.
- **EDGE** (model at least 75% and 8 points over the ask at a minute check) won 82% of its signals on the later days
  but lost 1.5¢ a signal. So here it is a **test only**: the box shows what it would buy and keeps a record, with no
  alerts. `[board] edge = "on"` makes it a buy signal; `"off"` hides it.
- **PULL OUT** at 30% was right on 347 of its 436 alarms (80%) on the later days.
- **Not adopted:** the crypto board's stricter 3–6 PM ET bar. FX calls in those hours did about as well as the rest.
- **Every pair** has the 10-second hold, no lean call, and no call in the last minute or for a window joined with under
  5:00 left.
- **Overrides:** change any value per pair in `config.toml`, for example `[tuning.usdcad] call_prob = 0.88`.

## Setup (macOS, next to the other boards)

```bash
cd ~ && unzip -o ~/Downloads/kalshi-fx-v1.zip
cd ~/kalshi-fx
source ~/.venv/bin/activate
pip install -r requirements.txt
python -m fx15m serve             # the board on http://127.0.0.1:8794
```

Other commands:
- `python -m fx15m serve --phone` prints a private link for your phone.
- `python -m fx15m probe` lists Kalshi's FX series with each window's target and live price (paste it to Claude).
- `python -m fx15m demo` runs the board on simulated pairs (offline).
- `pytest` runs the tests.

The board needs about 30 minutes of prices before its first call, the same as the commodities board.

## Upgrading

Unzip a new version over the folder with `unzip -o`. Never delete the folder: `data/` holds every pair's records.
