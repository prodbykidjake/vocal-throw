(() => {
  const $ = (id) => document.getElementById(id);
  const fmtC = (p) => { if (p == null) return "--"; const c = p * 100; return (Math.abs(c - Math.round(c)) > 0.05 ? c.toFixed(1) : Math.round(c)) + "¢"; };
  const fmtP = (p) => (p == null ? "--" : Math.round(p * 100) + "%");
  const fmtUsd = (x) => (x == null ? "--" : (x < 0 ? "-" : "+") + "$" + Math.abs(x).toFixed(2));
  const fmtT = (ts) => (ts ? new Date(ts * 1000).toLocaleTimeString([], { hour: "numeric", minute: "2-digit", timeZone: "America/New_York" }) + " ET" : "--");
  let state = null, chart = null, chartTicker = null, chartData = { ticks: [], strike: null, close: null, open: null };
  let lastSignalKey = null, audioCtx = null;

  // ---------------------------------------------------------------- tabs
  document.querySelectorAll(".tab").forEach((b) => b.addEventListener("click", () => {
    document.querySelectorAll(".tab").forEach((x) => x.classList.toggle("active", x === b));
    document.querySelectorAll(".tab-pane").forEach((p) => p.classList.toggle("active", p.id === "tab-" + b.dataset.tab));
    if (b.dataset.tab === "history") loadHistory();
    if (b.dataset.tab === "stats") loadStats();
    if (b.dataset.tab === "rules") loadRules();
  }));

  // ---------------------------------------------------------------- live stream
  function connect() {
    const es = new EventSource("/api/stream");
    es.onmessage = (ev) => { try { state = JSON.parse(ev.data); render(); } catch (e) { console.error(e); } };
    es.onerror = () => { $("kalshi-badge").textContent = "server: reconnecting"; $("kalshi-badge").className = "badge bad"; };
  }

  function badge(el, text, cls) { el.textContent = text; el.className = "badge " + (cls || ""); }

  function render() {
    const s = state; if (!s) return;
    const m = s.market, p = s.prediction, sig = s.signal, feed = s.feed || {};
    $("ticker").textContent = m ? m.ticker : "";
    $("series-line").textContent = s.series ? `${s.series.title || s.series.ticker} · fees ${s.fees.fee_type} ×${s.fees.multiplier}` + (s.series.settlement_sources?.length ? ` · settles on ${s.series.settlement_sources.map((x) => x.name).join(", ")}` : "") : "waiting for Kalshi…";
    $("countdown").textContent = s.countdown || "--:--";
    $("countdown").classList.toggle("urgent", s.seconds_left != null && s.seconds_left < 60);
    $("close-label").textContent = m ? `closes ${m.close_label}` : "no live window";
    // badges
    const age = feed.age_s;
    badge($("feed-badge"), feed.symbol ? `${feed.name} ${feed.symbol} · ${age == null || age < 0 ? "no data" : age.toFixed(1) + "s"}` : `${feed.name}: ${feed.mode}${feed.last_error ? " · " + feed.last_error : ""}`,
      age == null || age < 0 ? "bad" : age > 3 ? "warn" : "ok");
    const tr = s.tracker || {};
    badge($("kalshi-badge"), tr.last_error ? "kalshi: " + tr.last_error.slice(0, 60) : m ? `kalshi · ${tr.polls} polls` : "kalshi: no open market", tr.last_error ? "bad" : m ? "ok" : "warn");
    const seen = feed.seconds_seen || 0, need = (s.config?.min_warmup_minutes || 30) * 60;
    badge($("warm-badge"), seen >= need ? "warmed up" : `warm-up ${Math.floor(seen / 60)}/${Math.round(need / 60)} min`, seen >= need ? "ok" : "warn");
    // prices
    $("strike").textContent = m && m.strike != null ? "$" + m.strike.toFixed(2) : "--";
    $("strike-source").textContent = m ? `from ${m.strike_source}` : "";
    const px = s.tick ? s.tick.price : null;
    $("price").textContent = px != null ? "$" + px.toFixed(2) : "--";
    if (px != null && m && m.strike != null) {
      const d = px - m.strike;
      $("delta").textContent = `${d >= 0 ? "▲" : "▼"} ${Math.abs(d).toFixed(2)} vs target`;
      $("delta").className = "sub mono " + (d >= 0 ? "up" : "down");
      $("price").className = "big mono " + (d >= 0 ? "up" : "down");
    } else { $("delta").textContent = ""; $("price").className = "big mono"; }
    if (m) {
      const yesMid = m.yes_bid != null && m.yes_ask != null ? (m.yes_bid + m.yes_ask) / 2 : m.last_price;
      $("odds-up").textContent = "Up " + fmtP(yesMid);
      $("odds-down").textContent = "Down " + fmtP(yesMid == null ? null : 1 - yesMid);
      $("book").textContent = `Up ask ${fmtC(m.yes_ask)} · Down ask ${fmtC(m.no_ask)} · spread ${fmtC(m.yes_ask != null && m.yes_bid != null ? m.yes_ask - m.yes_bid : null)}`;
    }
    if (p) {
      $("model-up").textContent = "Up " + fmtP(p.p_up);
      $("model-down").textContent = "Down " + fmtP(p.p_down);
      $("model-sub").textContent = `${p.confidence.replace("_", " ")} · z ${p.z >= 0 ? "+" : ""}${p.z.toFixed(2)} · ±$${p.expected_move.toFixed(2)} expected · learner weight ${Math.round(p.shrink * 100)}%`;
    } else { $("model-up").textContent = "Up --"; $("model-down").textContent = "Down --"; $("model-sub").textContent = ""; }
    if (!m) { $("odds-up").textContent = "Up --"; $("odds-down").textContent = "Down --"; $("book").textContent = ""; }
    else if (s.quotes_fresh === false) { $("book").textContent += " · quotes stale"; }
    // signal
    if (sig) {
      const pill = $("signal-action");
      pill.textContent = sig.action + (sig.action === "BUY" || sig.action === "SELL" ? " " + sig.side : "");
      pill.className = "pill " + (sig.action === "BUY" ? "BUY-" + sig.side : sig.action);
      $("signal-headline").textContent = sig.headline;
      $("signal-details").innerHTML = sig.details.map((d) => `<li>${esc(d)}</li>`).join("");
      $("signal-triggers").textContent = "";
      const key = sig.action + ":" + (sig.side || "") + ":" + (sig.reasons[0] || "");
      if (lastSignalKey !== null && key !== lastSignalKey && (sig.action === "BUY" || sig.action === "SELL")) notify(sig);
      lastSignalKey = key;
    } else {
      $("signal-action").textContent = "WAIT"; $("signal-action").className = "pill";
      $("signal-headline").textContent = m ? "waiting for data…" : "no live window right now";
      $("signal-details").innerHTML = "";
    }
    // position
    renderPosition(s);
    const paper = s.paper;
    $("paper-line").textContent = paper ? `Paper position this window: ${paper.size} ${paper.side} @ ${fmtC(paper.entry_price)} (what the coach would have done)` : "No paper position this window.";
    // events
    $("events").innerHTML = (s.events || []).map((e) => `<li><span class="k">${new Date(e.ts * 1000).toLocaleTimeString()} ${esc(e.kind)}</span>${esc(e.text)}</li>`).join("");
    // chart
    const key = m ? m.ticker : "none";
    if (key !== chartTicker) { chartTicker = key; loadChart(); }
    else if (s.tick && chartData.ticks.length) {
      const last = chartData.ticks[chartData.ticks.length - 1];
      const liveStrike = m ? m.strike : null;
      if (liveStrike !== chartData.strike) chartData.strike = liveStrike;
      if (s.tick.ts > last[0]) { chartData.ticks.push([s.tick.ts, s.tick.price]); drawChart(); }
    }
  }

  // ---------------------------------------------------------------- position card
  let pendingSide = null;
  function renderPosition(s) {
    const pos = s.position, m = s.market;
    $("pos-entry").classList.toggle("hidden", !!pos);
    $("position-open").classList.toggle("hidden", !pos);
    if (!pos) {
      // keep the price box following the live ask until the user types in it
      const f = $("pos-form");
      if (pendingSide && m && !f.price_cents.matches(":focus") && !f.dataset.touched) {
        const ask = pendingSide === "UP" ? m.yes_ask : m.no_ask;
        if (ask != null) f.price_cents.value = (ask * 100).toFixed(1);
      }
      return;
    }
    const live = pos.live || {}, sc = pos.scalp;
    $("pos-shares").textContent = `${Number(pos.qty).toFixed(2)} ${pos.side}`;
    $("pos-shares").className = "big2 mono " + (pos.side === "UP" ? "up" : "down");
    $("pos-entry-text").textContent = `bought @ ${fmtC(pos.avg_price)} · $${Number(pos.cost).toFixed(2)} in · ${pos.ticker}`;
    $("pos-bid").textContent = live.bid == null ? "--" : fmtC(live.bid);
    $("pos-bid-sub").textContent = live.awaiting_settlement ? "window over · waiting for settlement" : (live.quotes_fresh === false ? "Kalshi quotes stale" : (pos.high_bid != null ? `high since entry ${fmtC(pos.high_bid)}` : ""));
    $("pos-cash").textContent = live.cash_out == null ? "--" : "$" + Number(live.cash_out).toFixed(2);
    $("pos-pnl").textContent = live.pnl == null ? "" : fmtUsd(live.pnl);
    $("pos-pnl").className = "sub mono " + (live.pnl == null ? "" : live.pnl >= 0 ? "up" : "down");
    const box = $("scalp-box");
    if (live.awaiting_settlement) { box.textContent = "WAITING FOR SETTLEMENT"; box.className = "scalp-box"; }
    else if (!sc) { box.textContent = "…"; box.className = "scalp-box"; }
    else if (sc.action === "SELL NOW") { box.textContent = `SELL NOW · ${fmtC(sc.bid)}`; box.className = "scalp-box sell-now"; }
    else if (sc.action === "SELL AT") { box.textContent = `SELL AT ${fmtC(sc.target)}  (now ${fmtC(sc.bid)})`; box.className = "scalp-box sell-at"; }
    else if (sc.reason === "ride_to_settle") { box.textContent = "HOLD TO SETTLEMENT"; box.className = "scalp-box ride"; }
    else { box.textContent = "HOLD"; box.className = "scalp-box"; }
  }
  document.querySelectorAll(".side-btn").forEach((b) => b.addEventListener("click", () => {
    pendingSide = b.dataset.side;
    const f = $("pos-form"); f.dataset.touched = "";
    $("pos-side-label").textContent = pendingSide; $("pos-side-label").className = "pill " + (pendingSide === "UP" ? "BUY-UP" : "BUY-DOWN");
    const m = state && state.market; const ask = m ? (pendingSide === "UP" ? m.yes_ask : m.no_ask) : null;
    f.price_cents.value = ask != null ? (ask * 100).toFixed(1) : "";
    f.amount.value = "";
    $("side-buttons").classList.add("hidden"); f.classList.remove("hidden"); f.amount.focus();
  }));
  $("pos-form").price_cents.addEventListener("input", () => { $("pos-form").dataset.touched = "1"; });
  $("pos-back").addEventListener("click", () => { pendingSide = null; $("pos-form").classList.add("hidden"); $("side-buttons").classList.remove("hidden"); });

  function esc(t) { return String(t).replace(/[&<>]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;" }[c])); }

  // ---------------------------------------------------------------- chart
  async function loadChart() {
    try {
      const r = await fetch("/api/chart?minutes=20"); const d = await r.json();
      chartData = { ticks: d.ticks || [], strike: d.strike, open: d.open_time, close: d.close_time };
      drawChart();
    } catch (e) { console.error(e); }
  }

  function drawChart() {
    const el = $("chart");
    if (typeof uPlot === "undefined") { el.textContent = "chart library did not load (offline?) — the numbers above still work"; return; }
    const xs = chartData.ticks.map((t) => t[0]), ys = chartData.ticks.map((t) => t[1]);
    const p = state && state.prediction, s = state;
    const strike = chartData.strike;
    const strikeYs = xs.map(() => strike);
    // expected range at close: from now to close, ±σ√τ around the last price
    const coneX = [], coneHi = [], coneLo = [];
    if (p && s && s.market && chartData.close && ys.length) {
      const now = xs[xs.length - 1], last = ys[ys.length - 1];
      for (let i = 0; i <= 10; i++) {
        const t = now + (chartData.close - now) * (i / 10); const tau = Math.max(0, chartData.close - t);
        coneX.push(t); coneHi.push(last + p.sigma * Math.sqrt(chartData.close - now - tau)); coneLo.push(last - p.sigma * Math.sqrt(chartData.close - now - tau));
      }
    }
    const allX = xs.concat(coneX.slice(1));
    const pad = (arr) => arr.concat(coneX.slice(1).map(() => null));
    const coneHiS = xs.map(() => null).slice(0, -1).concat(coneHi.length ? coneHi : [null]);
    const coneLoS = xs.map(() => null).slice(0, -1).concat(coneLo.length ? coneLo : [null]);
    const data = [allX, pad(ys), pad(strikeYs), coneHiS, coneLoS];
    const opts = {
      width: el.clientWidth || 700, height: 320,
      scales: { x: { time: true } },
      axes: [{ stroke: "#8b93a1", grid: { stroke: "#262b33" }, values: (u, v) => v.map((x) => new Date(x * 1000).toLocaleTimeString([], { hour: "numeric", minute: "2-digit" })) },
             { stroke: "#8b93a1", grid: { stroke: "#262b33" }, size: 70, values: (u, v) => v.map((y) => "$" + y.toFixed(2)) }],
      series: [{}, { stroke: "#9fb3c8", width: 2, label: "WTI" }, { stroke: "#e8eaed", dash: [6, 6], width: 1, label: "target" },
               { stroke: "rgba(90,169,255,.7)", dash: [3, 4], width: 1, label: "+1σ" }, { stroke: "rgba(90,169,255,.7)", dash: [3, 4], width: 1, label: "-1σ" }],
      cursor: { show: true }, legend: { show: false },
    };
    if (chart) { chart.setData(data); chart.setSize({ width: el.clientWidth || 700, height: 320 }); }
    else { el.innerHTML = ""; chart = new uPlot(opts, data, el); }
    $("chart-note").textContent = strike != null ? `dashed = target $${strike.toFixed(2)}, blue = expected ±1σ range into the close` : "";
  }
  window.addEventListener("resize", () => chart && chart.setSize({ width: $("chart").clientWidth, height: 320 }));

  // ---------------------------------------------------------------- notifications
  function notify(sig) {
    try {
      if (!audioCtx) audioCtx = new (window.AudioContext || window.webkitAudioContext)();
      const o = audioCtx.createOscillator(), g = audioCtx.createGain();
      o.frequency.value = sig.action === "BUY" ? 880 : 520; g.gain.value = 0.08;
      o.connect(g); g.connect(audioCtx.destination); o.start(); o.stop(audioCtx.currentTime + 0.25);
    } catch (e) {}
    if ("Notification" in window && Notification.permission === "granted") new Notification(`WTI 15m: ${sig.action} ${sig.side || ""}`, { body: sig.headline });
  }
  document.body.addEventListener("click", () => { if ("Notification" in window && Notification.permission === "default") Notification.requestPermission(); }, { once: true });

  // ---------------------------------------------------------------- position forms
  async function post(url, body) {
    const r = await fetch(url, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) });
    if (!r.ok) { let d = {}; try { d = await r.json(); } catch (e) {} alert(d.detail || "request failed"); return null; }
    return r.json();
  }
  $("pos-form").addEventListener("submit", async (ev) => {
    ev.preventDefault();
    const f = ev.target; const amount = Number(f.amount.value); const pc = f.price_cents.value;
    if (!pendingSide || !(amount > 0)) return;
    const body = { side: pendingSide, amount };
    if (pc !== "") body.price = Number(pc) / 100;
    const res = await post("/api/position", body);
    if (res) { pendingSide = null; f.classList.add("hidden"); $("side-buttons").classList.remove("hidden"); }
  });
  $("sold-now").addEventListener("click", async () => {
    const res = await post("/api/position/close", {});
    if (res) alert(`Recorded. Cash out $${res.cash_out.toFixed(2)}, P&L ${fmtUsd(res.pnl)}`);
  });
  $("close-form").addEventListener("submit", async (ev) => {
    ev.preventDefault();
    const pc = ev.target.price_cents.value; if (pc === "") return;
    const res = await post("/api/position/close", { price: Number(pc) / 100 });
    if (res) { ev.target.reset(); alert(`Recorded. Cash out $${res.cash_out.toFixed(2)}, P&L ${fmtUsd(res.pnl)}`); }
  });
  $("cancel-pos").addEventListener("click", () => { if (confirm("Remove this position entry? (use this only if you entered it by mistake)")) fetch("/api/position", { method: "DELETE" }); });

  // ---------------------------------------------------------------- history / stats / rules
  async function loadHistory() {
    const d = await (await fetch("/api/history")).json();
    $("history-table").querySelector("tbody").innerHTML = d.windows.map((w) => `<tr>
      <td>${fmtT(w.close_time)}</td><td class="mono">${w.strike != null ? "$" + w.strike.toFixed(2) : "--"}</td>
      <td class="${w.result === "yes" ? "up" : w.result === "no" ? "down" : "muted"}">${w.result === "yes" ? "UP" : w.result === "no" ? "DOWN" : w.status || "--"}</td>
      <td>${fmtP(w.p_at_10min)}</td><td>${fmtP(w.p_at_3min)}</td><td>${fmtP(w.p_market_at_close)}</td>
      <td>${w.settle_price != null ? "$" + w.settle_price.toFixed(2) : ""} ${w.feed_error == null ? "--" : (w.feed_error >= 0 ? "+" : "") + w.feed_error.toFixed(2)}</td>
      <td>${w.buy_signals || 0}</td><td class="${(w.paper_pnl || 0) >= 0 ? "up" : "down"}">${w.paper_pnl == null ? "--" : fmtUsd(w.paper_pnl)}</td></tr>`).join("");
    $("paper-table").querySelector("tbody").innerHTML = d.paper.map((t) => `<tr><td class="mono">${t.ticker}</td><td class="${t.side === "UP" ? "up" : "down"}">${t.side}</td>
      <td>${fmtC(t.entry_price)}</td><td>${t.size}</td><td>${t.exit_price == null ? "open" : fmtC(t.exit_price)}</td><td>${t.exit_reason || ""}</td>
      <td class="${(t.pnl || 0) >= 0 ? "up" : "down"}">${t.pnl == null ? "--" : fmtUsd(t.pnl)}</td><td>${t.confidence || ""}</td></tr>`).join("");
    $("pos-table").querySelector("tbody").innerHTML = d.positions.map((t) => `<tr><td class="mono">${t.ticker}</td><td class="${t.side === "UP" ? "up" : "down"}">${t.side}</td>
      <td>${Number(t.qty).toFixed(2)}${t.amount != null ? ` ($${Number(t.amount).toFixed(2)})` : ""}</td><td>${fmtC(t.avg_price)}</td><td>${t.exit_price == null ? "open" : fmtC(t.exit_price)}</td><td class="${(t.pnl || 0) >= 0 ? "up" : "down"}">${t.pnl == null ? "--" : fmtUsd(t.pnl)}</td></tr>`).join("");
  }
  async function loadStats() {
    const d = await (await fetch("/api/stats")).json();
    const b = d.brier || {};
    $("brier").innerHTML = Object.keys(b).length ? `<table class="table"><thead><tr><th>time left</th><th>n</th><th>model</th><th>market</th></tr></thead><tbody>` +
      Object.entries(b).map(([k, v]) => `<tr><td>${k}</td><td>${v.n}</td><td>${v.model}</td><td>${v.market ?? "--"}</td></tr>`).join("") + "</tbody></table>" +
      `<div class="sub">Settled windows: ${d.settled_windows} · Up share ${fmtP(d.up_share)} · feed vs settlement: mean error ${d.feed_error.mean_abs == null ? "--" : "$" + d.feed_error.mean_abs.toFixed(3)} over ${d.feed_error.n} windows (max $${(d.feed_error.max_abs ?? 0).toFixed(2)})` +
      (d.feed_error.n_lag ? ` · candle-lag check: feed at the close is off by $${d.feed_error.lag0_mean_abs.toFixed(3)}, one candle later by $${d.feed_error.lag60_mean_abs.toFixed(3)} (the smaller one is the right settle_lag_s)` : "") + `</div>`
      : `<div class="sub">No settled windows yet. Leave the app running; this fills in after each 15-minute window settles.</div>`;
    const c = d.calibration || {};
    $("calibration").innerHTML = Object.keys(c).length ? `<table class="table"><thead><tr><th>model said</th><th>n</th><th>actually Up</th></tr></thead><tbody>` +
      Object.entries(c).map(([k, v]) => `<tr><td>${k}</td><td>${v.n}</td><td>${fmtP(v.realized_up)}</td></tr>`).join("") + "</tbody></table>" : `<div class="sub">Nothing yet.</div>`;
    const pp = d.paper || {};
    $("paper-stats").innerHTML = `<div>${pp.trades || 0} paper trades · ${pp.wins || 0} wins · total ${fmtUsd(pp.pnl || 0)} after fees</div>` +
      (pp.by_confidence && Object.keys(pp.by_confidence).length ? `<table class="table"><thead><tr><th>confidence</th><th>n</th><th>hit rate</th><th>P&amp;L</th></tr></thead><tbody>` +
      Object.entries(pp.by_confidence).map(([k, v]) => `<tr><td>${k}</td><td>${v.n}</td><td>${fmtP(v.hit_rate)}</td><td>${fmtUsd(v.pnl)}</td></tr>`).join("") + "</tbody></table>" : "") +
      `<div class="sub">Your reported trades: ${d.real?.trades || 0}, P&amp;L ${fmtUsd(d.real?.pnl || 0)}</div>`;
    const l = d.calibrator || {};
    $("learner").innerHTML = `<div>Trained on ${l.n_windows || 0} settled windows (${l.n_samples || 0} snapshots) · weight in final answer ${Math.round((l.shrink || 0) * 100)}%</div>` +
      `<div class="sub">Feed basis allowance: $${(d.basis_error || 0).toFixed(2)}</div>` +
      `<pre class="pre">${esc(JSON.stringify(l.weights || {}, null, 1))}</pre>`;
  }
  async function loadRules() {
    const d = await (await fetch("/api/rules")).json();
    $("rules-series").textContent = JSON.stringify({ series: d.series, fees: d.fees, feed: d.feed }, null, 2);
    $("rules-text").textContent = (d.market_title ? d.market_title + "\n\n" : "") + (d.market_rules || "(no live market yet)");
    $("rules-config").textContent = JSON.stringify(d.config, null, 2);
  }
  setInterval(() => { if (document.querySelector("#tab-history.active")) loadHistory(); if (document.querySelector("#tab-stats.active")) loadStats(); }, 15000);

  connect();
})();
