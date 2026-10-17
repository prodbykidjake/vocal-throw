# 15-minute crypto board (Kalshi)

One page with a card for every Kalshi **15-minute crypto** Up/Down market: Bitcoin, Ethereum, Solana, XRP, Dogecoin,
Hyperliquid (HYPE), NEAR, Zcash and BNB (KXBTC15M ... KXBNB15M), plus any new one Kalshi adds. Each card runs the
commodities board's **settlement caller** with crypto's own settlement rule:
- one Up/Down call per window, locked once made, with the confidence meter;
- the **EDGE** buy signal, the **PULL OUT** alarm, the **Standard / Strict / Very strict** switch with all three
  modes recorded side by side, and **EARLY** tags, all as on the commodities board;
- a **Wins | Losses** record next to what Kalshi's own odds expected.

It is a separate app (package `cx15m`, folder `kalshi-crypto`, port **8792**). It shares no code, data or records with
the commodities board or the coaches, and runs next to them. It is a coach, not a bot: **it never places orders.**

## How these markets settle (and why that needed new maths)

Kalshi settles each window on the **simple average of the 60 once-a-second prints** of a CF Benchmarks index in the
minute before the close (BRTI for Bitcoin, ETHUSD_RTI, SOLUSD_RTI, XRPUSD_RTI, DOGEUSD_RTI, HYPEUSD_RTI, NEARUSD_RTI,
ZECUSD_RTI, BNBUSD_RTI). The target is the same average in the minute before the open, which is the previous window's
settlement value. A tie goes **Up**. Targets are written to 2 decimals (BTC, ETH, BNB), 4 (SOL, XRP, HYPE, NEAR, ZEC)
or 7 (DOGE).

Because it is an average, the last minute moves the result much less than the last price would:
- **With more than a minute left**, the average still ahead moves like a random walk over the time left minus
  40 seconds (an average over a minute varies like a third of a minute).
- **Inside the settlement minute**, the prints already in are locked. With 20 seconds left, two thirds of the
  average is already known, and the rest can only move by a fraction of what the price does.

The model uses both (tested in `tests/test_settle_avg.py` against thousands of simulated paths), the card shows the
"minute avg" so far in the last minute, and the detail view's stats compare the feed's own closing-minute average with
each settlement value.

## Where the price comes from

- **Without a key (the default): the exchanges' median.** CF builds the index from exchange order books, so the board
  takes the best bid/ask midpoint on **Coinbase, Kraken and Bitstamp** (live connections, no account needed), prints
  their median once a second, and seeds its volatility from Coinbase's (or Kraken's) one-minute candles so it can
  call from the first window. A coin an exchange does not list just leaves that exchange out. This tracks the index
  closely but not exactly; each card measures the remaining offset against the settlement values and corrects for a
  steady one.
- **With a Kalshi API key: the exact index.** Kalshi streams the CF index itself, once a second, but only to signed-in
  connections. With a key the board reads it (and Kalshi's own running settlement average in the last minute), with
  the exchanges as a backup. While the index is down the card says so, and **no new call is made** until it is back.

### Getting a Kalshi API key (optional, for exact prices)

1. On kalshi.com: **Account & security → API Keys → Create Key**. Kalshi shows a **Key ID** and downloads a
   **private key file** (`.pem` or `.txt`). It is shown only once.
2. Move that file to your home folder, e.g. `~/kalshi-key.pem`.
3. In `~/kalshi-crypto/config.toml` (copy `config.example.toml` if you have none):
   ```toml
   [kalshi]
   api_key_id = "the Key ID"
   private_key_path = "~/kalshi-key.pem"
   ```
4. Check it: `python -m cx15m probe-index` prints a few seconds of index messages (no secrets in them).

**Treat the private key like your password.** It can trade on your account, so never paste it into a chat, an email or
a screenshot, and never send it to anyone (Claude included). The board reads it from the file on your own computer,
uses it only to sign the connection that reads the index, never places orders, and `.gitignore` keeps `*.pem` and
`config.toml` out of the code.

## The call rules (for now)

Every coin starts with the commodities board's rules, which held up there on two months of history:

| mode        | call when one side holds | study | on commodities (held-out days)  |
|-------------|--------------------------|-------|---------------------------------|
| Standard    | 85% for 10 s             | 5:00  | ~85% of windows called, ~90% won |
| Strict      | 93% for 10 s             | 5:00  | ~75% called, ~94% won            |
| Very strict | 97% for 10 s             | 5:00  | ~65% called, ~97% won            |

No lean call at 2:00, no call in the last minute, none for a window joined with under 5:00 left, none in a window
holding an FOMC decision or CPI release, and none during **Kalshi's weekly maintenance (Thursday 3:00-5:00 AM ET)**.
These are **not yet tuned on crypto**: each coin gets its own bars once its Kalshi history has been replayed through
this code (see below). Until then the side-by-side mode records on each card show how each bar does live.

## Kalshi history (for tuning each coin)

From the WTI coach's folder, which has the history tool:

```bash
cd ~/kalshi-wti && python3 tools/backfill.py --probe --series KXBTC15M
cd ~/kalshi-wti && for s in KXBTC15M KXETH15M KXSOL15M KXXRP15M KXDOGE15M KXHYPE15M KXNEAR15M KXZEC15M KXBNB15M; do python3 tools/backfill.py --series $s --out data/history-$s.db --rps 5; done
```

Send the `history-*.zip` files it makes to Claude.

## Setup (macOS, next to the other boards)

```bash
cd ~ && unzip -o ~/Downloads/kalshi-crypto-v1.zip
cd ~/kalshi-crypto
source ~/.venv/bin/activate
pip install -r requirements.txt   # once: adds the WebSocket and key-signing libraries
python -m cx15m probe             # first time: series, targets, settlements and the exchanges' prices (paste it to Claude)
python -m cx15m serve             # the board on http://127.0.0.1:8792
```

`python -m cx15m serve --phone` prints a private link for your phone (same Wi-Fi or Tailscale), as on the commodities
board. `python -m cx15m demo` runs the board on nine simulated coins at their real price scales (offline). `pytest`
runs the tests. `[feed] source = "kalshi-live"` switches to Kalshi's keyless chart series instead, if `probe` shows it
carries the index.

## Upgrading

Unzip a new version over the folder with `unzip -o`. Never delete the folder: `data/` holds every coin's record and
what it has learned.
