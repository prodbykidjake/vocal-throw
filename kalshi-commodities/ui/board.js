// The board: one card per 15-minute commodity with its settlement call, confidence meter and record.
(function () {
  const $ = (id) => document.getElementById(id);
  const esc = (s) => String(s ?? "").replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
  const pct = (p) => (p == null ? "--" : `${Math.round(p * 100)}%`);
  const mmss = (s) => (s == null ? "--:--" : `${Math.floor(Math.max(0, s) / 60)}:${String(Math.floor(Math.max(0, s) % 60)).padStart(2, "0")}`);
  const px = (x, dec) => (x == null ? "--" : "$" + Number(x).toLocaleString("en-US", { minimumFractionDigits: Math.max(dec, 2), maximumFractionDigits: Math.max(dec, 2) }));
  // a gap or move in the market's natural unit: cents under $200, dollars above
  const move = (d, sc) => (d == null ? "--" : sc.cents ? `${(Math.abs(d) * 100).toFixed(Math.max(0, sc.decimals - 2))}¢`
    : "$" + Math.abs(d).toLocaleString("en-US", { minimumFractionDigits: 2, maximumFractionDigits: 2 }));
  let state = null, openKey = null, chart = null;

  // ------------------------------------------------------------------ cards
  function cardHtml(m) {
    return `<div class="mkt" id="mkt-${esc(m.key)}" data-key="${esc(m.key)}">
      <div class="mkt-top"><span class="mkt-badge" style="background:${esc(m.color)}">${esc(m.badge)}</span>
        <span class="mkt-kind">${esc(m.kind === "metal" ? "metals" : m.kind === "oil" || m.kind === "gas" ? "oil & gas" : "commodities")}</span>
        <button type="button" class="mute" data-act="mute" title="mute or unmute this market's notifications">🔔</button></div>
      <div class="mkt-title"><span data-f="title"></span></div>
      <div class="mkt-live"><span class="dot">● LIVE</span> <span data-f="close"></span> · <span data-f="countdown" class="mono"></span></div>
      <div class="mkt-prices"><div><div class="lbl">target</div><div class="val" data-f="target">--</div></div>
        <div style="text-align:right"><div class="lbl">now</div><div class="val" data-f="now">--</div><div class="sub" data-f="delta"></div></div></div>
      <div class="mkt-odds"><div class="odd up" data-f="odd-up">Up --</div><div class="odd down" data-f="odd-down">Down --</div></div>
      <div class="call-box" data-f="box">waiting…</div>
      <div class="meter hidden" data-f="meter">
        <div class="meter-labels"><span class="down">◀ DOWN</span><span class="meter-read" data-f="read">--</span><span class="up">UP ▶</span></div>
        <div class="meter-track"><div class="meter-band" data-f="band"></div><div class="meter-fill" data-f="fill"></div>
          <div class="meter-line" data-f="line-down"></div><div class="meter-line" data-f="line-up"></div><div class="meter-mid"></div>
          <div class="meter-mkt" data-f="mkt" title="Kalshi's odds"></div></div>
      </div>
      <div class="mkt-note" data-f="note"></div>
      <div class="mkt-status" data-f="status"></div>
      <div class="mkt-record" data-f="record">Wins: 0 | Losses: 0</div>
      <div class="mkt-bench" data-f="bench"></div>
      <div class="call-recent" data-f="recent"></div>
    </div>`;
  }

  function ensureCards(markets) {
    const box = $("cards");
    for (const m of markets) {
      if (!$(`mkt-${m.key}`)) box.insertAdjacentHTML("beforeend", cardHtml(m));
    }
  }

  const f = (card, name) => card.querySelector(`[data-f="${name}"]`);

  function renderCard(m) {
    const card = $(`mkt-${m.key}`); if (!card) return;
    const sc = m.scale || { decimals: 2, cents: true }, mk = m.market, tick = m.tick, c = m.call || {};
    f(card, "title").textContent = `${m.name} 15 min` + (mk && mk.strike != null ? ` · ${px(mk.strike, sc.decimals)} target` : "");
    f(card, "close").textContent = mk && mk.close_label ? `closes ${mk.close_label}` : "";
    f(card, "countdown").textContent = m.countdown || "--:--";
    card.classList.toggle("live", m.status === "live");
    f(card, "target").textContent = mk && mk.strike != null ? px(mk.strike, sc.decimals) : "--";
    f(card, "now").textContent = tick ? px(tick.price, sc.decimals) : "--";
    const d = tick && mk && mk.strike != null ? tick.price - mk.strike : null;
    const dl = f(card, "delta");
    dl.textContent = d == null ? "" : `${d >= 0 ? "▲" : "▼"} ${move(d, sc)} vs target`;
    dl.className = "sub " + (d == null ? "" : d >= 0 ? "up" : "down");
    const pm = m.prediction && m.prediction.p_market != null ? m.prediction.p_market : (mk && mk.yes_bid != null && mk.yes_ask != null ? (mk.yes_bid + mk.yes_ask) / 2 : null);
    f(card, "odd-up").textContent = `Up ${pct(pm)}`;
    f(card, "odd-down").textContent = `Down ${pm == null ? "--" : pct(1 - pm)}`;
    card.querySelector('[data-act="mute"]').textContent = m.muted ? "🔕" : "🔔";
    // the call box
    const call = c.call, w = c.watch || {}, rules = c.rules || {}, box = f(card, "box");
    if (call) {
      const res = call.result == null ? "" : call.correct ? " · WIN ✓" : " · LOSS ✗";
      box.textContent = `${call.side} AT SETTLEMENT${res}`;
      box.className = "call-box " + (call.side === "UP" ? "up" : "down") + (call.result == null ? "" : call.correct ? " win" : " loss");
    } else if (w.status === "studying" || w.status === "watching") {
      box.textContent = (w.status === "studying" ? `STUDYING · ${mmss(w.study_left_s)}` : "WATCHING") + (w.confirming_s != null ? ` · confirming ${w.confirming_s}/${rules.hold_s} s` : "");
      box.className = "call-box";
    } else {
      box.textContent = (w.status || "waiting for the next window").toUpperCase(); box.className = "call-box";
    }
    renderMeter(card, c, call, w, rules);
    // status line for problems only
    const st = f(card, "status");
    st.textContent = m.status === "no_series" ? `Kalshi has no ${m.series} series (check the probe)` :
      m.status === "no_feed" ? "waiting for Kalshi's live price…" : m.status === "no_market" ? "no window trading right now" :
      (m.feed && m.feed.age_s != null && m.feed.age_s > 8) ? `live price is ${Math.round(m.feed.age_s)} s old: no new calls` : "";
    // record
    const r = c.record || {}, n = (r.wins || 0) + (r.losses || 0);
    f(card, "record").textContent = `Wins: ${r.wins || 0} | Losses: ${r.losses || 0}` + (n ? ` (${Math.round(100 * r.wins / n)}%)` : "");
    f(card, "bench").textContent = n ? `Kalshi's odds expected ${r.market_expected} wins over ${r.n_market} calls` : "no settled calls yet";
    f(card, "recent").innerHTML = (r.recent || []).slice(0, 10).reverse().map((x) =>
      `<span class="chip ${x.correct ? "win" : "loss"}" title="${esc(x.ticker)} · ${esc(x.kind)} · ${pct(x.p)}">${x.side === "UP" ? "▲" : "▼"}${x.correct ? "✓" : "✗"}</span>`).join("");
  }

  function renderMeter(card, c, call, w, rules) {
    const m = c.meter, box = f(card, "meter"), note = f(card, "note");
    if (!m) { box.classList.add("hidden"); note.textContent = w.status && !call ? "" : note.textContent; return; }
    box.classList.remove("hidden");
    const pos = (p) => `${(100 * p).toFixed(1)}%`, up = m.p_up >= 0.5, side = up ? "UP" : "DOWN";
    const pSide = up ? m.p_up : 1 - m.p_up, line = m.line || 0.72;
    const fill = f(card, "fill");
    fill.style.left = up ? "50%" : pos(m.p_up);
    fill.style.width = pos(Math.abs(m.p_up - 0.5));
    fill.className = "meter-fill " + (up ? "up" : "down") + (pSide >= line ? " ready" : "");
    f(card, "band").style.left = pos(m.lo); f(card, "band").style.width = pos(Math.max(0.004, m.hi - m.lo));
    f(card, "line-up").style.left = pos(line); f(card, "line-down").style.left = pos(1 - line);
    const mk = f(card, "mkt");
    if (m.market_up == null) mk.classList.add("hidden"); else { mk.classList.remove("hidden"); mk.style.left = pos(m.market_up); }
    const read = f(card, "read");
    read.textContent = `${side} ${Math.round(pSide * 100)}%`; read.className = "meter-read " + (up ? "up" : "down");
    const loSide = up ? m.lo : 1 - m.hi, hiSide = up ? m.hi : 1 - m.lo;
    const range = (m.lo < 0.5 && m.hi > 0.5) ? "crossing between Up and Down" : `between ${Math.round(Math.min(loSide, hiSide) * 100)}% and ${Math.round(Math.max(loSide, hiSide) * 100)}% ${side}`;
    let t;
    if (call) t = `called ${call.side} with ${mmss(call.seconds_left)} left at ${pct(call.p_side)} · model now ${side} ${Math.round(pSide * 100)}% · the call stays (locked)`;
    else if ((w.status || "").startsWith("no call")) t = `${w.status} · the meter only shows what the model sees`;
    else if (w.status === "studying") t = `studying: no call for ${mmss(w.study_left_s)} · last 30 s ${range}`;
    else if (w.confirming_s != null) t = `over the ${Math.round(line * 100)}% line · confirming ${w.confirming_s}/${rules.hold_s} s`;
    else if (m.disagree) t = "model and Kalshi's odds are far apart: no confident call until they come closer";
    else t = `needs ${Math.round(line * 100)}% for ${rules.hold_s} s · last 30 s ${range}` + (w.deadline_in_s != null ? ` · otherwise its lean in ${mmss(w.deadline_in_s)}` : "");
    note.textContent = t;
  }

  function renderBoard(s) {
    state = s;
    const markets = s.markets || [];
    ensureCards(markets);
    markets.forEach(renderCard);
    const r = s.record || {}, n = (r.wins || 0) + (r.losses || 0);
    $("total-record").textContent = `Wins: ${r.wins || 0} | Losses: ${r.losses || 0}` + (n ? ` (${Math.round(100 * r.wins / n)}%)` : "");
    $("total-sub").textContent = n ? `all markets · Kalshi's odds expected ${r.market_expected} wins over ${r.n_market} calls` : "all markets · no settled calls yet";
    $("lines").innerHTML = (s.lines || []).map((l) => `<span>${esc(l.name)} <b>${l.wins}-${l.losses}</b></span>`).join("");
    $("board-foot").textContent = `${markets.length} markets · ${s.requests != null ? s.requests + " Kalshi requests so far" : "simulated"}` + (s.throttled ? ` · slowed down by Kalshi ${s.throttled}×` : "");
    if (openKey) renderDetailLive();
  }

  // ------------------------------------------------------------------ detail view
  function openDetail(key) {
    openKey = key;
    $("detail").classList.remove("hidden");
    loadDetail();
  }
  function closeDetail() { openKey = null; $("detail").classList.add("hidden"); if (chart) { chart.destroy(); chart = null; } }

  function marketState(key) { return ((state && state.markets) || []).find((m) => m.key === key); }

  function renderDetailLive() {
    const m = marketState(openKey); if (!m) return;
    const sc = m.scale || { decimals: 2, cents: true }, p = m.prediction;
    $("d-badge").textContent = m.badge; $("d-badge").style.background = m.color;
    $("d-title").textContent = `${m.name} 15 min · ${m.series}` + (m.market && m.market.strike != null ? ` · ${px(m.market.strike, sc.decimals)} target · ${m.countdown}` : "");
    $("d-model").textContent = p ? `Model: Up ${pct(p.p_up)} · Kalshi ${pct(p.p_market)} · ${p.confidence.replace("_", " ")} · z ${p.z >= 0 ? "+" : ""}${p.z.toFixed(2)} · expected move to the close ±${move(p.expected_move, sc)}` +
      (p.notes && p.notes.length ? " · " + p.notes.join(" · ") : "") : "no prediction yet";
    const v = m.vol || {};
    $("d-vol").textContent = (v.note ? `Volatility: ${v.note}` : "") + (v.events && v.events.length ? ` · scheduled in this window: ${v.events.map((e) => `${e.name} at ${e.at}`).join(", ")}` : "");
  }

  async function loadDetail() {
    if (!openKey) return;
    renderDetailLive();
    let d;
    try { d = await (await fetch(`/api/market/${encodeURIComponent(openKey)}`)).json(); } catch (e) { return; }
    if (!openKey || d.key !== openKey) return;
    const m = marketState(openKey) || {}, sc = m.scale || { decimals: 2, cents: true };
    drawChart(d.chart, sc, m);
    $("d-calls").querySelector("tbody").innerHTML = (d.calls || []).map((c) =>
      `<tr><td class="mono">${esc(c.ticker.slice(-9))}</td><td class="${c.side === "UP" ? "up" : "down"}">${esc(c.side)}</td><td>${pct(c.p)}</td>
       <td>${pct(c.p_market)}</td><td>${mmss(c.seconds_left)}</td><td>${esc(c.kind)}</td><td class="${c.correct ? "up" : "down"}">${c.correct ? "WIN ✓" : "LOSS ✗"}</td></tr>`).join("") ||
      `<tr><td colspan="7" class="muted">no settled calls yet</td></tr>`;
    const st = d.stats || {}, fe = st.feed_error || {}, b = st.brier || {};
    const brier = Object.entries(b).map(([k, v]) => `${k}: model ${v.model} vs Kalshi ${v.market ?? "--"} (n ${v.n})`).join(" · ");
    const timing = fe.n_lag ? `Settlement timing check: the live price at the close matched the settlement in ${fe.lag0_cent} of ${fe.n_lag} windows, one minute later in ${fe.lag60_cent}` +
      (fe.n_lag >= 6 && fe.lag60_cent > fe.lag0_cent + 2 ? " (the minute later fits better: tell Claude)" : "") : "Settlement timing check: no measured windows yet";
    const vp = st.vol_profile || {};
    $("d-stats").innerHTML = `<div>${esc(timing)}</div><div>Settled windows: ${st.settled_windows || 0} · Up share ${pct(st.up_share)}</div>` +
      `<div>Accuracy (Brier, lower is better): ${esc(brier || "no settled windows yet")}</div>` +
      `<div>Targets written with ${sc.decimals} decimals · tie allowance ${sc.tie_adj ?? "--"} · learned volatility from ${vp.n || 0} windows (overall ×${vp.overall ?? 1})</div>`;
    $("d-log").innerHTML = (d.events || []).map((e) => `<div>${new Date(e.ts * 1000).toLocaleTimeString()} · ${esc(e.text)}</div>`).join("") || "nothing yet";
  }

  function drawChart(c, sc, m) {
    const el = $("d-chart");
    const ticks = (c && c.ticks) || [];
    if (!ticks.length || typeof uPlot === "undefined") { el.textContent = "no price data yet"; return; }
    const xs = ticks.map((t) => t[0]), ys = ticks.map((t) => t[1]);
    const strike = c.strike != null ? xs.map(() => c.strike) : xs.map(() => null);
    const data = [xs, ys, strike];
    $("d-chart-note").textContent = c.strike != null ? `dashed = target ${px(c.strike, sc.decimals)}` : "";
    const opts = { width: Math.max(320, el.clientWidth || 900), height: 300, legend: { show: false },
      axes: [{ stroke: "#8b93a1", grid: { stroke: "#262b33" } },
             { stroke: "#8b93a1", grid: { stroke: "#262b33" }, size: 90, values: (u, v) => v.map((y) => px(y, sc.decimals)) }],
      series: [{}, { stroke: "#9fb3c8", width: 2 }, { stroke: "#e8eaed", dash: [6, 6], width: 1 }] };
    if (chart) chart.destroy();
    el.textContent = "";
    chart = new uPlot(opts, data, el);
  }

  // ------------------------------------------------------------------ events
  $("cards").addEventListener("click", async (ev) => {
    const card = ev.target.closest(".mkt"); if (!card) return;
    const key = card.dataset.key;
    if (ev.target.closest('[data-act="mute"]')) {
      ev.stopPropagation();
      const m = marketState(key);
      await fetch(`/api/market/${encodeURIComponent(key)}/mute`, { method: "POST", headers: { "Content-Type": "application/json", "X-CM15M": "1" },
        body: JSON.stringify({ muted: !(m && m.muted) }) });
      return;
    }
    openDetail(key);
  });
  $("d-close").addEventListener("click", closeDetail);
  $("detail").addEventListener("click", (ev) => { if (ev.target === $("detail")) closeDetail(); });
  document.addEventListener("keydown", (ev) => { if (ev.key === "Escape") closeDetail(); });
  setInterval(() => { if (openKey) loadDetail(); }, 15000);

  function connect() {
    const es = new EventSource("/api/stream");
    es.onmessage = (ev) => { try { renderBoard(JSON.parse(ev.data)); } catch (e) { console.error(e); } };
    es.onerror = () => { es.close(); setTimeout(connect, 3000); };
  }
  fetch("/api/state").then((r) => r.json()).then(renderBoard).catch(() => {});
  connect();
})();
