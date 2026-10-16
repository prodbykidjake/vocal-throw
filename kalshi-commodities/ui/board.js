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
    return `<article class="mkt" id="mkt-${esc(m.key)}" data-key="${esc(m.key)}" style="--accent:${esc(m.color)}">
      <div class="mkt-top"><span class="chip-badge" style="background:${esc(m.color)}">${esc(m.badge)}</span>
        <div><div class="mkt-name">${esc(m.name)}</div><div class="mkt-meta" data-f="meta"></div></div>
        <div class="spacer"></div>
        <button type="button" class="icon-btn" data-act="mute" title="mute or unmute this market's notifications">🔔</button></div>
      <div class="timebar"><span class="live">LIVE</span><div class="progress"><div data-f="progress" style="width:0%"></div></div><span class="cd" data-f="countdown">--:--</span></div>
      <div class="prices"><div><div class="px-k">target</div><div class="px-v" data-f="target">--</div></div>
        <div class="right"><div class="px-k">now</div><div class="px-v" data-f="now">--</div><div class="px-d" data-f="delta"></div></div></div>
      <div class="odds"><div class="odds-bar"><div class="o-up" data-f="odd-up" style="width:50%">Up --</div><div class="o-down" data-f="odd-down">Down --</div></div></div>
      <div class="callbox" data-f="box">waiting…</div>
      <div class="meter hidden" data-f="meter">
        <div class="meter-labels"><span class="down">◀ DOWN</span><span class="meter-read" data-f="read">--</span><span class="up">UP ▶</span></div>
        <div class="meter-track"><div class="meter-band" data-f="band"></div><div class="meter-fill" data-f="fill"></div>
          <div class="meter-line" data-f="line-down"></div><div class="meter-line" data-f="line-up"></div><div class="meter-mid"></div>
          <div class="meter-mkt" data-f="mkt" title="Kalshi's odds"></div></div>
      </div>
      <div class="note" data-f="note"></div>
      <div class="po"><div class="po-box" data-f="po-box">PULL OUT · waiting for a call</div><div class="po-note" data-f="po-note"></div></div>
      <div class="edge"><div class="edge-box" data-f="edge-box">EDGE · waiting</div><div class="edge-note" data-f="edge-note"></div>
        <div class="edge-rec" data-f="edge-rec"></div></div>
      <div class="warn" data-f="status"></div>
      <div class="rec"><div><div class="rec-v" data-f="record">0–0</div><div class="rec-k" data-f="bench">no settled calls yet</div></div>
        <div class="chips" data-f="recent"></div></div>
      <div class="rules" data-f="modes"></div>
      <div class="rules" data-f="rules"></div>
    </article>`;
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
    f(card, "meta").innerHTML = `${esc(m.series)} · ${mk && mk.close_label ? "closes " + esc(mk.close_label) : "waiting for a window"}` +
      (m.engine ? ` · <span class="eng">${esc(m.engine)}</span>` : "");
    f(card, "countdown").textContent = m.countdown || "--:--";
    f(card, "progress").style.width = m.seconds_left == null ? "0%" : `${Math.max(0, Math.min(100, 100 * (1 - m.seconds_left / 900))).toFixed(1)}%`;
    f(card, "target").textContent = mk && mk.strike != null ? px(mk.strike, sc.decimals) : "--";
    f(card, "now").textContent = tick ? px(tick.price, sc.decimals) : "--";
    const d = tick && mk && mk.strike != null ? tick.price - mk.strike : null;
    const dl = f(card, "delta");
    dl.textContent = d == null ? "" : `${d >= 0 ? "▲" : "▼"} ${move(d, sc)} ${d >= 0 ? "above" : "below"}`;
    dl.className = "px-d " + (d == null ? "" : d >= 0 ? "up" : "down");
    const pm = m.prediction && m.prediction.p_market != null ? m.prediction.p_market : (mk && mk.yes_bid != null && mk.yes_ask != null ? (mk.yes_bid + mk.yes_ask) / 2 : null);
    const ou = f(card, "odd-up");
    ou.style.width = pm == null ? "50%" : `${Math.max(14, Math.min(86, pm * 100)).toFixed(1)}%`;
    ou.textContent = `Up ${pct(pm)}`;
    f(card, "odd-down").textContent = `Down ${pm == null ? "--" : pct(1 - pm)}`;
    card.querySelector('[data-act="mute"]').textContent = m.muted ? "🔕" : "🔔";
    const call = c.call, w = c.watch || {}, rules = c.rules || {}, box = f(card, "box");
    card.classList.toggle("called-up", !!call && call.side === "UP" && call.result == null);
    card.classList.toggle("called-down", !!call && call.side === "DOWN" && call.result == null);
    if (call) {
      const res = call.result == null ? "" : call.correct ? " · WIN ✓" : " · LOSS ✗";
      box.textContent = `${call.side === "UP" ? "▲" : "▼"} ${call.side} AT SETTLEMENT${res}`;
      box.className = "callbox " + (call.side === "UP" ? "up" : "down") + (call.result == null ? "" : call.correct ? " win" : " loss");
    } else if (w.status === "studying" || w.status === "watching") {
      box.textContent = (w.status === "studying" ? `STUDYING · ${mmss(w.study_left_s)}` : "WATCHING") + (w.confirming_s != null && rules.hold_s ? ` · confirming ${w.confirming_s}/${rules.hold_s}s` : "");
      box.className = "callbox";
    } else if ((w.status || "").startsWith("no call")) {
      const why = /\((.*)\)/.exec(w.status);
      box.textContent = "NO CALL THIS WINDOW" + (why ? ` · ${why[1]}` : ""); box.className = "callbox none";
    } else {
      box.textContent = (w.status || "waiting for the next window").toUpperCase(); box.className = "callbox";
    }
    renderMeter(card, c, call, w, rules);
    renderPullout(card, m);
    renderEdge(card, m);
    const st = f(card, "status");
    st.textContent = m.status === "no_series" ? `Kalshi has no ${m.series} series right now` :
      m.status === "no_feed" ? "waiting for Kalshi's live price…" : m.status === "no_market" ? "no window trading right now" :
      (m.feed && m.feed.age_s != null && m.feed.age_s > 8) ? `live price is ${Math.round(m.feed.age_s)}s old: no new calls` : "";
    const r = c.record || {}, n = (r.wins || 0) + (r.losses || 0), rr = c.record_rules, tag = rules.tag || "";
    f(card, "record").textContent = `${r.wins || 0}–${r.losses || 0}` + (n ? ` · ${Math.round(100 * r.wins / n)}%` : "");
    let bench = n ? `all time · Kalshi expected ${r.market_expected} of ${r.n_market}` : "no settled calls yet";
    if (rr) {
      const nn = (rr.wins || 0) + (rr.losses || 0);
      bench += ` · since ${tag}: ${rr.wins || 0}–${rr.losses || 0}` + (nn ? ` (${Math.round(100 * rr.wins / nn)}%)` : "");
    }
    f(card, "bench").textContent = bench;
    // the three strictness modes on the same windows (cm15m/shadow.py)
    const md = m.modes || {}, labels = (state && state.mode_labels) || {};
    f(card, "modes").textContent = Object.keys(md).length ? "same windows: " + ["standard", "strict", "very_strict"].filter((k) => md[k]).map((k) => {
      const x = md[k], nn = (x.wins || 0) + (x.losses || 0);
      return `${labels[k] || k} ${x.wins || 0}–${x.losses || 0}` + (nn ? ` (${Math.round(100 * x.wins / nn)}%)` : "");
    }).join(" · ") : "";
    const held = rules.hold_s ? ` held ${rules.hold_s}s` : " (the first second it gets there)";
    f(card, "rules").textContent = rules.prob == null ? "" : `${tag ? tag + " rules" : "rules"}: calls at ${Math.round(rules.prob * 100)}%+${held} · studies ${mmss(rules.study_s)}` +
      (rules.late_call === false ? " · no lean calls" : ` · lean call at ${mmss(rules.deadline_s)} left`) +
      (rules.blend_market ? ` · reads engine + Kalshi (Kalshi ${Math.round(rules.blend_market * 100)}%)` : "");
    f(card, "recent").innerHTML = (r.recent || []).slice(0, 12).reverse().map((x) =>
      `<span class="c ${x.correct ? "w" : "l"}" title="${esc(x.ticker)} · ${esc(x.side)} · ${pct(x.p)}">${x.correct ? "✓" : "✗"}</span>`).join("");
  }

  // PULL OUT: flashes when the engine and Kalshi together put the locked call at 3% or less with 1:00+ left (see cm15m/pullout.py)
  function renderPullout(card, m) {
    const p = m.pullout || {}, w = p.watch || {}, r = p.record || {}, box = f(card, "po-box"), note = f(card, "po-note");
    const a = w.alarm, live = !!a && a.result == null && m.market && a.ticker === m.market.ticker;
    card.classList.toggle("pulled", live);
    if (a) {
      const side = a.side, bid = a.bid != null ? ` · bid ~${cents(a.bid)}` : "";
      if (a.result == null) {
        box.textContent = `⚠ PULL OUT · SELL YOUR ${side} NOW`;
        box.className = "po-box fire";
        note.textContent = `the ${side} call has flipped: engine ${pct(a.p_model)}` + (a.p_kalshi != null ? ` · Kalshi ${pct(a.p_kalshi)}` : " (Kalshi had no usable quote)") +
          ` for ${side} with ${mmss(a.seconds_left)} left${bid} · right about 98 times in 100 on two months of history`;
      } else {
        box.textContent = a.correct ? `PULL OUT was right ✓ · the ${side} call lost` : `PULL OUT was wrong ✗ · the ${side} call won`;
        box.className = "po-box " + (a.correct ? "right" : "wrong");
        note.textContent = `flashed with ${mmss(a.seconds_left)} left at engine ${pct(a.p_model)}` + (a.p_kalshi != null ? ` · Kalshi ${pct(a.p_kalshi)}` : "") + bid;
      }
    } else if (w.state === "watching") {
      box.className = "po-box watch";
      const now = w.p_kalshi != null ? `engine ${pct(w.p_model)} · Kalshi ${pct(w.p_kalshi)}` : `engine ${pct(w.p_model)} (no usable Kalshi quote)`;
      if (w.too_late) {
        box.textContent = `PULL OUT · off in the last minute`;
        note.textContent = `${w.side} call · ${now} · under 1:00 left the losing side often has no bid to sell into`;
      } else {
        box.textContent = `PULL OUT · watching your ${w.side} call · ${now}`;
        note.textContent = w.rule === "B" ? `flashes if the engine stays at ${pct(w.model_level)} or less for ${w.hold_s}s` + (w.low_for_s != null ? ` (${w.low_for_s}s so far)` : "")
          : `flashes only if engine and Kalshi average ${pct(w.level)} or less for ${w.side} (now ${pct(w.score)})`;
      }
    } else if (w.state === "paused") {
      box.className = "po-box"; box.textContent = `PULL OUT · paused`; note.textContent = `${w.why || "waiting"} · no alarm until it is back`;
    } else if (p.last && !(m.market && p.last.ticker === m.market.ticker)) {
      // how the last alarm (an earlier window) ended: Kalshi scores it a little after the close
      const l = p.last;
      box.textContent = l.result == null ? `PULL OUT · last window: waiting for Kalshi's result` :
        l.correct ? `PULL OUT · last window: right ✓ · the ${l.side} call lost` : `PULL OUT · last window: wrong ✗ · the ${l.side} call won`;
      box.className = "po-box" + (l.result == null ? "" : l.correct ? " right" : " wrong");
      note.textContent = `flashed with ${mmss(l.seconds_left)} left at engine ${pct(l.p_model)}` + (l.p_kalshi != null ? ` · Kalshi ${pct(l.p_kalshi)}` : "") +
        (l.bid != null ? ` · bid ~${cents(l.bid)}` : "") + " · waiting for this window's call";
    } else {
      box.className = "po-box"; box.textContent = "PULL OUT · waiting for a call"; note.textContent = "";
    }
    note.textContent += r.n ? `${note.textContent ? " · " : ""}record: right ${r.right} of ${r.n}` : "";
  }

  // EDGE: the buy signal. Only when the model beats Kalshi's ask by the rule's margin at a minute check (see cm15m/edge.py)
  const money = (x) => (x == null ? "--" : (x < 0 ? "−$" : "+$") + Math.abs(x).toFixed(2));
  const cents = (x) => (x == null ? "--" : `${Math.round(x * 100)}¢`);
  function renderEdge(card, m) {
    const e = m.edge || {}, w = e.watch || {}, r = e.record || {}, sig = w.signal;
    const box = f(card, "edge-box"), note = f(card, "edge-note");
    if (sig) {
      const res = sig.result == null ? "" : sig.correct ? " · WIN ✓" : " · LOSS ✗";
      box.textContent = `EDGE · BUY ${sig.side} at ${cents(sig.ask)} or less${res}`;
      box.className = "edge-box sig " + (sig.side === "UP" ? "up" : "down") + (sig.result == null ? "" : sig.correct ? " win" : " loss");
      note.textContent = `model ${pct(sig.p)} vs ask ${cents(sig.ask)} (+${Math.round(sig.gap * 100)} pts) at minute ${sig.minute} · hold to settlement` +
        (sig.result != null ? ` · ${money(sig.pnl)} per contract` : "");
    } else if (w.side) {
      const gp = w.gap == null ? "--" : `${w.gap >= 0 ? "+" : ""}${Math.round(w.gap * 100)} pts`;
      box.textContent = w.window_over ? "EDGE · none this window" : `EDGE · watching · ${w.side} ${pct(w.p)} vs ask ${cents(w.ask)} (${gp})`;
      box.className = "edge-box";
      note.textContent = w.window_over ? "no edge at any minute check this window: skipping is the right trade" :
        `buys only at ${Math.round(((e.rules || {}).min_p || 0.75) * 100)}%+ and +${Math.round((w.need || 0.08) * 100)} pts over the ask, at a minute check` + (w.next_check_in_s != null ? ` · next check in ${mmss(w.next_check_in_s)}` : "") +
        (w.usable ? "" : " · waiting for fresh quotes");
    } else {
      box.textContent = "EDGE · waiting for a window"; box.className = "edge-box"; note.textContent = "";
    }
    const n = (r.wins || 0) + (r.losses || 0);
    f(card, "edge-rec").textContent = n ? `Edge ${r.wins}–${r.losses} · ${money(r.pnl)} per contract in total` : "Edge: no settled signals yet";
  }

  function renderMeter(card, c, call, w, rules) {
    const m = c.meter, box = f(card, "meter"), note = f(card, "note");
    if (!m) { box.classList.add("hidden"); note.textContent = ""; return; }
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
    const range = (m.lo < 0.5 && m.hi > 0.5) ? "crossing Up/Down" : `${Math.round(Math.min(loSide, hiSide) * 100)}–${Math.round(Math.max(loSide, hiSide) * 100)}% ${side}`;
    let t;
    if (call) t = `called ${call.side} at ${pct(call.p_side)} with ${mmss(call.seconds_left)} left · now ${side} ${Math.round(pSide * 100)}% · locked`;
    else if ((w.status || "").startsWith("no call")) t = `${w.status} · the meter only shows what the model sees`;
    else if (w.status === "studying") t = `studying · no call for ${mmss(w.study_left_s)} · last 30s ${range}`;
    else if (w.confirming_s != null && rules.hold_s) t = `over the ${Math.round(line * 100)}% line · confirming ${w.confirming_s}/${rules.hold_s}s`;
    else if (m.disagree) t = "model and Kalshi far apart: no confident call yet";
    else t = `needs ${Math.round(line * 100)}%` + (rules.hold_s ? ` for ${rules.hold_s}s` : "") + ` · last 30s ${range}` + (w.deadline_in_s != null ? ` · lean call in ${mmss(w.deadline_in_s)}` :
      w.closes_in_s != null ? ` · ${mmss(w.closes_in_s)} left to get there, else no call` : "");
    if (m.model_up != null) t += ` · engine ${pct(m.model_up)} blended with Kalshi ${pct(m.market_up)}`;
    note.textContent = t;
  }

  function renderBoard(s) {
    state = s;
    const markets = s.markets || [];
    ensureCards(markets);
    markets.forEach(renderCard);
    const r = s.record || {}, n = (r.wins || 0) + (r.losses || 0);
    $("total-record").textContent = `${r.wins || 0}–${r.losses || 0}`;
    $("total-rate").textContent = n ? `${Math.round(100 * r.wins / n)}%` : "--";
    $("total-exp").textContent = n ? `${r.market_expected} / ${r.n_market}` : "--";
    const rr = s.record_rules || {}, rn = (rr.wins || 0) + (rr.losses || 0);
    const labels = s.mode_labels || {}, curMode = s.mode || "standard", modesOn = s.modes_on !== false;
    $("rules-label").textContent = modesOn ? `${labels[curMode] || "Standard"} mode` : `since ${rr.tag || "these rules"}`;
    $("rules-record").innerHTML = rr.since == null ? "--" : `${rr.wins || 0}–${rr.losses || 0}` + (rn ? ` <small>${Math.round(100 * rr.wins / rn)}%</small>` : "");
    $("shadow-note").textContent = modesOn ? "calls made in this mode" : "";
    renderModes(s);
    const po = s.pullout || {};
    $("po-record").textContent = po.n ? `${po.right}/${po.n}` : "--";
    renderBanner(markets);
    const er = s.edge || {}, en = (er.wins || 0) + (er.losses || 0);
    $("edge-record").textContent = `${er.wins || 0}–${er.losses || 0}`;
    $("edge-pnl").textContent = en ? money(er.pnl) : "--";  // the total for one contract on every signal, fees included
    $("edge-note").textContent = en ? `${er.pnl < 0 ? "−" : "+"}${Math.abs(100 * er.pnl / en).toFixed(1)}¢ a signal · 1 contract each` : "";
    const live = markets.find((m) => m.countdown && m.seconds_left != null);
    $("clock").textContent = live ? live.countdown : "--:--";
    const colors = Object.fromEntries(markets.map((m) => [m.key, m.color]));
    $("lines").innerHTML = (s.lines || []).map((l) => `<span class="pill" data-key="${esc(l.key)}"><span class="dot" style="background:${esc(colors[l.key] || "#888")}"></span>${esc(l.name)} <b>${l.wins}–${l.losses}</b></span>`).join("");
    $("board-foot").textContent = `${markets.length} markets · settlement calls are forecasts (v1.4: fewer, surer calls, about 9 in 10 on two months of history, but later and at higher prices, so bought at the ask they still break even); EDGE is the buy signal; PULL OUT is the safety net · ` +
      (s.requests != null ? `${s.requests} Kalshi requests so far` : "simulated markets") + (s.throttled ? ` · slowed down by Kalshi ${s.throttled}×` : "");
    if (openKey) renderDetailLive();
  }

  // the strictness switch: Standard / Strict / Very strict, each with its record on the same windows
  const MODE_HELP = { standard: "about 85% of windows called, ~90% right", strict: "about 75% called, ~94% right, calls ~1:20 later",
    very_strict: "about 65% called, ~97% right, calls ~2:30 later" };
  function renderModes(s) {
    const box = $("modes"), cur = s.mode || "standard", labels = s.mode_labels || {}, tot = s.modes || {};
    box.classList.toggle("hidden", s.modes_on === false);  // [tuning] enabled = false: v1.3's rules, no switch
    if (s.modes_on === false) return;
    box.innerHTML = ["standard", "strict", "very_strict"].map((k) => {
      const t = tot[k], n = t ? (t.wins || 0) + (t.losses || 0) : 0;
      const rec = t ? ` <span class="mode-rec">${t.wins || 0}–${t.losses || 0}${n ? ` · ${Math.round(100 * t.wins / n)}%` : ""}</span>` : "";
      return `<button type="button" class="mode-btn${k === cur ? " on" : ""}" data-mode="${k}" title="${esc(MODE_HELP[k])}">${esc(labels[k] || k)}${rec}</button>`;
    }).join("") + `<span class="mode-note">${esc(MODE_HELP[cur])} · records: all three modes on the same windows since v1.5</span>`;
  }

  // a PULL OUT anywhere on the board: a banner pinned to the top of the screen (the card may be scrolled away, on a phone)
  const baseTitle = document.title;
  let lastPulled = "";
  function renderBanner(markets) {
    const on = markets.filter((m) => { const a = ((m.pullout || {}).watch || {}).alarm; return a && a.result == null && m.market && a.ticker === m.market.ticker; });
    const ban = $("po-banner");
    if (!on.length) { ban.classList.add("hidden"); document.title = baseTitle; lastPulled = ""; return; }
    ban.classList.remove("hidden");
    ban.innerHTML = on.map((m) => { const a = m.pullout.watch.alarm;
      return `<button type="button" class="po-ban-item" data-key="${esc(m.key)}">⚠ PULL OUT · ${esc(m.name)} · sell your ${esc(a.side)} now` + (a.bid != null ? ` (bid ~${cents(a.bid)})` : "") + `</button>`; }).join("");
    document.title = `⚠ PULL OUT ${on.map((m) => m.name).join(", ")}`;
    const key = on.map((m) => m.key + m.pullout.watch.alarm.ticker).join(",");
    if (key !== lastPulled && navigator.vibrate) { try { navigator.vibrate([300, 150, 300]); } catch (e) { /* not supported */ } }
    lastPulled = key;
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
    $("d-title").textContent = `${m.name} · ${m.market && m.market.strike != null ? px(m.market.strike, sc.decimals) + " target" : "no window"}`;
    $("d-sub").textContent = `${m.series} · ${m.countdown} left` + (m.engine ? ` · ${m.engine}` : " · board desk");
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
    const opts = { width: Math.max(320, el.clientWidth || 700), height: 260, legend: { show: false },
      axes: [{ stroke: "#5b6372", grid: { stroke: "rgba(255,255,255,0.05)" }, ticks: { stroke: "rgba(255,255,255,0.08)" } },
             { stroke: "#5b6372", grid: { stroke: "rgba(255,255,255,0.05)" }, ticks: { stroke: "rgba(255,255,255,0.08)" }, size: 96, values: (u, v) => v.map((y) => px(y, sc.decimals)) }],
      series: [{}, { stroke: m.color || "#6aa8ff", width: 2, fill: "rgba(106,168,255,0.06)" }, { stroke: "#eef1f6", dash: [6, 6], width: 1 }] };
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
  $("modes").addEventListener("click", async (ev) => {
    const b = ev.target.closest(".mode-btn"); if (!b || b.classList.contains("on")) return;
    b.classList.add("busy");  // the next second's update shows the switch done
    try {
      const r = await fetch("/api/mode", { method: "POST", headers: { "Content-Type": "application/json", "X-CM15M": "1" },
        body: JSON.stringify({ mode: b.dataset.mode }) });
      if (!r.ok) b.classList.remove("busy");
    } catch (e) { b.classList.remove("busy"); }
  });
  $("po-banner").addEventListener("click", (ev) => {
    const b = ev.target.closest(".po-ban-item"); if (!b) return;
    const card = $(`mkt-${b.dataset.key}`); if (card) card.scrollIntoView({ behavior: "smooth", block: "center" });
  });
  $("detail").addEventListener("click", (ev) => { if (ev.target === $("detail")) closeDetail(); });
  $("lines").addEventListener("click", (ev) => { const p = ev.target.closest(".pill"); if (p) openDetail(p.dataset.key); });
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
