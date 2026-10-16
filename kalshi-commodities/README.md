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

## v1.6: EARLY calls and STRONG EDGE signals

Two things the history says are worth acting on, now marked on the cards. Neither changes when a call or a signal
happens; they only show which ones paid best.

- **EARLY** (a gold tag on the call): the call was made with **6:30 or more left**. A call that early needs the price
  to be clearly past the target already, so it only happens in some windows (in Strict, about a third of its calls,
  typically around 8:25 left). At Strict's level, tested on later days than the settings were chosen on, those early
  calls won as often as the later ones (94.7% against 95.3%) and cost less to buy (about 93¢ instead of 95¢): about
  **+1.4¢ a call** at the fill, against +0.3¢ for the later ones. Each card shows its early record ("early calls:
  W–L"). The other windows cannot be called at 95% that early: with 7 minutes left their price can still cross the
  target, and nothing tested (a separate early bar, momentum, gold's moves, more weight on Kalshi's odds) got more
  windows to 95% that early.
- **STRONG EDGE**: the EDGE signal's gap is **12 to 19 points** over the ask (as shown on the card). Those paid about **twice as much per
  contract** as 8-12-point signals, in both halves of the history (+14.6¢ vs +7.8¢, then +9.6¢ vs +4.0¢, fills a minute
  later, fees included), so the box says to size up (about 2× your normal size). A gap of **20+ points** says to keep
  normal size: those were not reliable (+18.6¢, then -2.3¢); the market often knows something the model does not.
  The card's EDGE record and the top strip show the strong signals' own results.

Also tested and not used: gold's last 1-3 minutes as a lead for silver, copper, platinum and palladium (no help: each
metal's own price already moves with gold). Order flow and resting limit orders under the ask could not be tested
(no trade history yet). This downloads Kalshi's trades with the history, so they can be:

```bash
cd ~/kalshi-wti && for s in KXWTI15M KXGOLD15M KXSILVER15M KXCOPPER15M KXNATGAS15M KXPALLADIUM15M KXPLATINUM15M; do python3 tools/backfill.py --series $s --out data/history-$s.db --rps 5 --trades; done
```

## v1.5: what testing found, and a strictness switch

Each market's engine was re-tested on its own terms against two months of history. Settings were always chosen on
earlier days and scored on later ones, at the same number of calls as v1.4. Tested for every market:
- its own mix of the engine and Kalshi's price;
- momentum;
- how wild the window has been so far;
- time-left adjustments;
- holds of 0-20 seconds instead of 10.

**Result: none of them beat v1.4 fairly.** Once calls had to come just as early and stop at the same last minute,
every change was within -0.9 to +0.7 points. Apparent gains came from three things that don't help a trader:
- calling later;
- reacting to one-second price spikes;
- calling at the very last allowed second.

So v1.4's engines stay as they are, and nothing is dressed up as a "smarter" engine.

What really buys accuracy is calling **fewer** windows. The switch at the top of the page sets how strict every card
is. Each market gets its own bars, fitted to how that market moves:

| mode | windows called | won on the later days | called with | Kalshi's ask then |
| --- | --- | --- | --- | --- |
| **Standard** (v1.4) | ~85% | 89-91% | ~6:20 left | ~90¢ |
| **Strict** | ~75% | 93.5-95% | ~5:00 left | ~93¢ |
| **Very strict** | ~65% | 96.5-98% | ~3:50 left | ~96¢ |

- **Profit at the ask:** buying the called side stays about even in every mode, because Kalshi's price rises with the
  certainty. The switch buys **trust in the call**, not a money edge; **EDGE is still the buy signal**.
- **All three modes run on the same windows in the background.** Each card shows their records side by side ("same
  windows: Standard 40–4 · Strict 33–1 · Very strict 27–0"), and so does the switch. After a few days the live
  numbers show which mode suits you.
- **Switching:** the switch applies to every card from the next call on. A call already made stays as it is. The
  choice is remembered (`data/board.json`), and `[board] call_mode = "strict"` in `config.toml` sets the starting mode.
- **v1.5.1: no WTI call on the backup price.** When Kalshi's price feed is out for more than 8 seconds, the WTI
  card reads Hyperliquid's price instead, which has sat 3-17¢ away from the price Kalshi settles on. It now makes no new
  call on that price (the standalone WTI coach has worked this way since v12.4); the meter keeps showing, and the card
  says why. EDGE and PULL OUT already skipped it.

**Checked on days nothing was tuned on.** Fresh Kalshi history from Oct 13 evening to Oct 15 (1,195 windows over
the seven markets, after every setting above was fixed) was replayed through the board's own code in each mode:

| mode | windows called | won | predicted | profit at the fill, fees included |
| --- | --- | --- | --- | --- |
| **Standard** | 84% | 92.3% | 89-91% | +2.3¢ a call |
| **Strict** | 74% | 95.2% | 93.5-95% | +1.4¢ a call |
| **Very strict** | 62% | 97.6% | 96.5-98% | +1.2¢ a call |

The replay matches the live board: of 266 live v1.4 calls in the same windows, it made 263, on the same side in 259,
with the same 242 wins. Natural gas was the weakest market in every mode (87.1%, 89.4% and 94.9%). PULL OUT fired 7
times and was right all 7; EDGE went 80-20 (+2¢ a signal at the next minute's price).

## v1.4: surer calls, and a PULL OUT alarm

**Fewer, surer settlement calls.** Each market now has its own rules (`cm15m/tuning.py`), chosen on the first 60% of
two months of Kalshi history and checked on the last 40% by replaying the board's own code:

| market | call when one side holds ... | study | win rate on the held-out days (v1.3) | windows called |
| --- | --- | --- | --- | --- |
| WTI | 88% for 10 s | 5:00 | **90.5%** (76.4%) | 83% |
| Gold | 88% | 4:00 | **91.4%** (78.0%) | 84% |
| Silver | 88% | 5:00 | **89.3%** (77.6%) | 87% |
| Copper | 88% | 5:00 | **90.7%** (77.0%) | 84% |
| Natural Gas | 85% of a blend of the engine and Kalshi's price | 4:00 | **90.9%** (77.9%) | 82% |
| Palladium | 85% | 5:00 | **89.8%** (78.0%) | 87% |
| Platinum | 85% | 5:00 | **88.8%** (80.8%) | 90% |

- **No more "lean" calls at 2:00 left:** they won only 63-73%. A window that never gets sure enough says
  **NO CALL THIS WINDOW**.
- **No calls in report windows** on WTI, gas and gold too (the other four already skipped them). Windows that opened
  on a release won only 64%. CPI days are now on the metals calendar: Oct 14's CPI moved every metal several standard
  deviations in two minutes.
- **The catch:** calls come later (about 6½ minutes left instead of 10) when Kalshi's price is higher (about 90¢
  instead of 78¢). Bought at the ask they still about break even. **EDGE stays the buy signal.**
- Each card shows its rules and a **since v1.4** record next to the all-time one, so you can see whether it is
  working.
- `[tuning] enabled = false` in `config.toml` brings back v1.3's rules. `[tuning.gold] call_prob = 0.90` changes one
  market.

**PULL OUT.** Under each call there is a PULL OUT box. It flashes red (**⚠ PULL OUT · SELL YOUR UP NOW**), a red
banner appears at the top of the page, and the Mac notifies (even for a muted card) when the locked call has flipped
and is now almost certainly wrong:
- the engine's chance for the called side and Kalshi's price for it **average 3% or less**, with at least 1:00 left;
- if Kalshi has no usable price: the engine alone at 1% or less for 10 seconds.

On two months of history it was right (the call really lost) **98 times in 100**:
- On v1.3's calls: 97.8% on the held-out days and 97-100% in every market, flashing on about 7 calls in 100.
- On v1.4's surer calls: right 162 times out of 166 (97.6%; 67 of 68 on the held-out days). It now flashes **rarely,
  on about 1 call in 100**, usually in the last two minutes. The new calls lose less often, and when they do it is
  more often a last-minute turn, which no alarm can catch in time.

It deliberately stays quiet when a call merely slides: a strong call that drops to 35% with 5 minutes left still wins
about 1 time in 4. Nothing under 1:00 left: by then the losing side often has no bid to sell into.

It is a safety net, not a money maker. Selling at the alarm gets the few cents the losing side still bids (about 2-3¢
a contract on history; maybe more if you sell within seconds) instead of riding it to zero, and about 2 alarms in 100
will be on a call that comes back. After the close, the box shows how the last alarm ended ("last window: right ✓")
once Kalshi settles it, and keeps its own record ("PULL OUT right N of M"). Alarms are kept in `data/pullouts.db`.

## EDGE: the buy signal (v1.2, tightened in v1.3)

Two months of Kalshi history (Aug 14 - Oct 13 2026, pulled with `kalshi-wti/tools/backfill.py`; second-by-second
settlement prices for 1,600-4,100 windows per market) replayed through each card's own engine said:

- **The settlement calls are honest but not a money edge.** Over ~17,000 replayed calls they won about as often as
  they said (WTI 78.5% vs 78.0% expected), but by the time a call fires Kalshi's price already says the same thing.
  Bought at the ask about a minute after the call (the price real fills got), every market came out between -0.9c and
  +0.6c a contract after fees: break-even. The calls stay on the cards as forecasts.
- **What made money: buying only when the model is clearly more confident than Kalshi's price.** At each whole
  minute from 3:00 to 13:00 into a window, if the model's chance for its side is at least **75%** and at least
  **8 points above that side's ask**, the card shows **EDGE · BUY UP/DOWN at N¢ or less** and notifies.
  - The idea was found on WTI alone with a 6-point gap (chosen on WTI's first 19 days: +6.2c a contract on its last 12
    days, and, never tuned on them, +2.2c to +6.4c on the six other markets).
  - v1.3 tightened it after testing 8 combinations on all seven markets: 8 points and 75% won **84%** of the time
    (79% before) and made **+6.5c a contract** (+4.8 to +8.2, 95% range by day; +4.5c before), profitable on every
    market, with about half as many signals (a few per market per day). Fills are priced a full minute after the
    signal, fees included.
  - History is not a promise, and even at 84% about one signal in six loses (a loss costs ~78c a contract, a win pays
    ~22c). The EDGE record on the board keeps the honest count.
- **Hold to settlement.** Stop-losses, take-profits and "cash out when it turns" all lost money in history; so did
  resting a bid below the ask (the orders that fill are the ones going wrong), buying the cheap side, and clicking
  faster (the ask barely moves in the minute after a signal).

The EDGE box under each card's meter shows what it is watching (model vs ask, the gap, the next minute check), a
signal when one fires (one per window), and its result; the top strip shows the EDGE record and the EDGE profit across
all markets: the total for buying one contract on every signal at its ask, fees included, with the average per signal
under it. Signals are kept in `data/edges.db`. The margin and minimum are `[board] edge_gap` (0.08) and
`edge_min_p` (0.75).

### On your phone

`python -m cm15m serve --phone` (or `phone = true` under `[server]`) prints a private link like
`http://192.168.1.23:8790/?key=...`. Open it on your phone on the same Wi-Fi, then Share → Add to Home Screen; the
phone remembers the key for a year. Every other device gets "Locked". The Mac has to stay awake and keep the board
running. Away from home, use Tailscale and the Mac's `100.x.y.z` address. Delete `data/phone_key.txt` to lock every
phone out and get a new link.

## The page

- **The cards**, in Kalshi's order. Each shows:
  - the target and the live price, and the move between them (in ¢ under $200, in $ above);
  - the countdown and Kalshi's Up/Down odds;
  - the **call box**: STUDYING, then WATCHING, then e.g. **UP AT SETTLEMENT**, locked (or **NO CALL THIS WINDOW**);
  - the **meter**: the model's chance on a Down ◀ 50% ▶ Up bar, with dashed lines at the market's call line, a faint
    band for the last 30 seconds, and Kalshi's odds as a gold tick;
  - the **PULL OUT box** (above);
  - the market's record (all time, and since v1.4), its last 10 calls and its rules;
  - the **EDGE box** (above).
- **The top strip**: the combined record across all markets, the record in the current mode, how often PULL OUT was
  right, the EDGE record, and each market's line, so it is clear which markets the caller reads best.
- **The strictness switch** (v1.5, above): Standard / Strict / Very strict, each with its record on the same windows.
- **Click a card** for its detail view:
  - the price-vs-target chart;
  - what the model sees;
  - the call history;
  - the stats: the settlement timing check, accuracy vs Kalshi, and learned volatility.
- **🔔 on a card** mutes that market's notifications (remembered). Notifications name the market:
  "GOLD settles UP".

## How each market is handled

- **The WTI coach's rules, tuned per market in v1.4** (table above):
  - a 4- or 5-minute study;
  - a call when one side holds 85-88% for 10 s, and no call at all if it never gets there;
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
  - Metals: jobless claims Thursday 8:30, FOMC decisions at 14:00 and CPI at 8:30. The CPI dates are Nov 10 and
    Dec 10 2026 (on BLS's schedule) and Jan 13 2027 (projected).

  Windows holding a report get no new calls, on every market. The jobs report and PCE dates are not built in, because
  they could not be checked against the official schedules. Add them per market in `config.toml` (see
  `config.example.toml`).
- **Kalshi's rate limit:** one shared limiter, at most 14 requests a second. The three coach engines refresh every
  second, as in their own apps, and the other four markets every 2 seconds, staggered. The page footer shows if Kalshi ever slows the board down.

## Setup (macOS, next to the other coaches)

```bash
unzip -o ~/Downloads/kalshi-commodities-v1.6.zip -d ~
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
