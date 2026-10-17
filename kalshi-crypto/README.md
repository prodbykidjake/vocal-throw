# 15-minute crypto board (Kalshi)

One page with a card for every Kalshi **15-minute crypto** Up/Down market: Bitcoin, Ethereum, Solana, XRP, Dogecoin,
Hyperliquid (HYPE), NEAR, Zcash and BNB (KXBTC15M ... KXBNB15M), plus any new one Kalshi adds. Each card runs the
commodities board's **settlement caller** with crypto's own settlement rule:
- one Up/Down call per window, locked once made, with the confidence meter;
- the **PULL OUT** alarm, the **Standard / Strict / Very strict** switch with all three modes recorded side by side,
  and **EARLY** tags, as on the commodities board, with **each coin's own rules** chosen on 60 days of its Kalshi
  history;
- **EDGE** as a test-only record (no buy alerts): on crypto history it lost money on later days (see below);
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

**Kalshi's own chart series for a crypto window is the settlement average itself**: each point is the CF index's
running 60-second average, and the window settles on the point one second before the close (rounded to the target's
decimals). Checked on 72 of 72 live windows over six coins and on 5,643 of 5,688 Bitcoin windows over 60 days, to the
cent. It is free and needs no key, but it trails the index by about half a minute, and the odds depend on where the
price is now. So the board combines it with the exchanges:

- **Without a key (the default):** the board takes the best bid/ask midpoint on **Coinbase, Kraken and Bitstamp**
  (live connections, no account needed) and prints their median once a second, **matched to Kalshi's average**.
  The exchanges sit a steady 0.5-9.5 basis points off the index (Bitcoin a few dollars); the board measures that
  offset continuously against each new point of Kalshi's average and takes it out. The card's top line says
  "matched to Kalshi's average (offset …)". For the first minute or so after start-up, until it is matched, the card
  says FALLBACK and makes no call.
- **Volatility is measured on Kalshi's average too** (its 30-second changes), as in the history the rules were chosen
  on. The exchanges' prices move less over a few seconds than the index really does, and would make the odds too sure.
- **Inside the settlement minute** the card shows the minute's average so far ("minute avg"), from Kalshi's average,
  exact at the last second.
- **With a Kalshi API key:** the board also reads the raw CF index Kalshi streams to signed-in connections, with the
  matched exchanges as its backup (calls go on while the backup is matched). It is optional: without it the board is
  already matched to the exact settlement series.

### Getting a Kalshi API key (optional)

1. On kalshi.com: **Account & security → API Keys → Create Key**. Kalshi shows a **Key ID** and downloads a
   **private key file** (`.pem` or `.txt`; RSA or Ed25519, both work). The file is offered only once.
2. Run, from the board's folder:
   ```bash
   cd ~/kalshi-crypto && source ~/.venv/bin/activate && python -m cx15m setup-key
   ```
   It finds the key file in your Downloads, moves it to `~/kalshi-key.pem` (readable by you only), asks you to paste
   the **Key ID**, writes both into `config.toml`, and tests the connection for 12 seconds. To do it by hand instead,
   copy `config.example.toml` to `config.toml` and fill in `api_key_id` and `private_key_path` under `[kalshi]`.
3. Restart the board (`python -m cx15m serve`). The start-up line says `prices: kalshi-index`.

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

The board carries its own copy of the history tool (it keeps Dogecoin's 7 decimals):

```bash
cd ~/kalshi-crypto && python3 tools/backfill.py --probe                 # a minute: paste the output to Claude
cd ~/kalshi-crypto && python3 tools/backfill.py --series all --rps 5    # about an hour, unattended; safe to stop and re-run
```

It makes one `data/history-KX...15M.zip` per coin. Send those files to Claude.

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
