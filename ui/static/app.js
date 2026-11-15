// app.js: the LockSpy dashboard. Talks to ui/server.py:
//   GET /api/programs, /api/source, /api/bench    POST /api/run, /api/build
// A run returns the JSON report LockSpy writes (lockspy_report.json) plus its stderr.
"use strict";

const $ = (s) => document.querySelector(s);
const esc = (s) => String(s ?? "").replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));
const KIND = {
  POTENTIAL_DEADLOCK: "Potential deadlock", LIVE_DEADLOCK: "Live deadlock", RELOCK: "Relock",
  BAD_UNLOCK: "Bad unlock", HELD_AT_EXIT: "Held at exit",
};
const nice = (k) => KIND[k] || k;

// Plain-English names for the test programs (the file name is still shown, smaller).
const NAMES = {
  "buggy/ab_ba": "AB–BA deadlock (no freeze)",
  "buggy/ab_ba --hang": "AB–BA deadlock (freezes)",
  "buggy/philosophers_naive": "Dining philosophers, freezes",
  "buggy/philosophers_naive --no-hang": "Dining philosophers, one at a time",
  "buggy/producer_consumer_bad": "Producer–consumer, wrong lock order",
  "buggy/readers_writers_bad": "Readers–writers, wrong lock order",
  "buggy/recursive_lock": "Locking the same lock twice",
  "buggy/bad_unlock": "Unlocking a lock you don't hold",
  "buggy/lock_held_at_exit": "Forgot to unlock before exiting",
  "correct/philosophers_ordered": "Dining philosophers, fixed",
  "correct/producer_consumer_ok": "Producer–consumer, fixed",
  "correct/readers_writers_ok": "Readers–writers, fixed",
  "correct/consistent_order": "AB–BA fixed (same lock order)",
  "tricky/gate_lock_safe": "Both orders, but behind a gate lock",
  "tricky/trylock_backoff": "Both orders, but with try-lock",
  "tricky/single_thread_both_orders": "Both orders, but only one thread",
  "perf/contention_heavy 2000": "Busy locks, 8 threads",
  "perf/uncontended_loop 200000": "Fast lock/unlock loop",
};
const title = (p) => (p.custom ? p.id : NAMES[p.id] || p.program.split("/")[1] + (p.args ? " " + p.args : ""));
const fileName = (p) => p.program.split("/")[1] + ".cpp" + (p.args ? " " + p.args : "");

let programs = [];
let selected = null;
let filter = "all";
let busy = false;
const results = {};       // program id -> run result, or "running"

// ------------------------------------------------------------------ helpers
async function api(path, body) {
  const opt = body === undefined ? {} : { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) };
  const r = await fetch(path, opt);
  const j = await r.json().catch(() => ({ error: "bad response from the server" }));
  if (!r.ok) throw new Error(j.error || r.statusText);
  return j;
}
const sameSet = (a, b) => a.length === b.length && [...a].sort().join() === [...b].sort().join();
const verdict = (p, r) => (!r || r === "running" ? "" : p.custom ? "info" : sameSet(r.found, p.expected) && !r.timed_out ? "pass" : "fail");
const ms = (ns) => (ns / 1e6).toFixed(2);
const tags = (kinds) => (kinds.length ? kinds.map((k) => `<span class="tag bad">${nice(k)}</span>`).join(" ") : `<span class="tag ok">No warnings</span>`);
const setStatus = (t) => ($("#status").textContent = t);
function setBusy(b) {
  busy = b;
  ["#run-btn", "#runall-btn", "#build-btn"].forEach((id) => ($(id).disabled = b));
}
function progress(frac) { $("#progress-bar").style.width = frac === null ? "0" : Math.round(frac * 100) + "%"; }

// ------------------------------------------------------------------ KPI cards
function setKpi(id, value, of, mood) {
  const el = $(id);
  el.querySelector(".v").textContent = value;
  el.querySelector(".of").textContent = of || "";
  el.classList.remove("good", "bad");
  if (mood) el.classList.add(mood);
}
function updateKpis() {
  const done = programs.filter((p) => results[p.id] && results[p.id] !== "running");
  if (!done.length) return;
  const buggy = programs.filter((p) => p.group === "buggy");
  const safe = programs.filter((p) => p.group === "correct" || p.group === "tricky");
  const caught = buggy.filter((p) => verdict(p, results[p.id]) === "pass").length;
  const fps = safe.filter((p) => results[p.id] && results[p.id] !== "running" && results[p.id].found.length).length;
  const passed = programs.filter((p) => verdict(p, results[p.id]) === "pass").length;
  const allDone = done.length === programs.length;
  setKpi("#kpi-bugs", caught, `/ ${buggy.length}`, caught === buggy.length ? "good" : allDone ? "bad" : "");
  setKpi("#kpi-fp", fps, `/ ${safe.length}`, fps === 0 ? "good" : "bad");
  setKpi("#kpi-pass", passed, `/ ${programs.length}`, passed === programs.length ? "good" : allDone ? "bad" : "");
  $("#kpi-pass .kpi-bar div").style.width = (passed / programs.length) * 100 + "%";
}

// ------------------------------------------------------------------ program list
function renderList() {
  const ul = $("#plist");
  ul.innerHTML = "";
  for (const p of programs.filter((x) => filter === "all" || x.group === filter)) {
    const r = results[p.id];
    const v = verdict(p, r);
    const ico = r === "running" ? `<span class="ico run"></span>` : v === "pass" ? `<span class="ico pass">✓</span>` : v === "fail" ? `<span class="ico fail">!</span>` : `<span class="ico">·</span>`;
    const name = esc(title(p));
    const meta = r && r !== "running" ? (r.found.length ? r.found.map(nice).join(", ") : "no warnings")
      : `expects ${p.expected.length ? p.expected.map(nice).join(", ").toLowerCase() : "no warnings"}`;
    const li = document.createElement("li");
    li.innerHTML = `<button type="button" class="prow${selected && selected.id === p.id ? " on" : ""}">${ico}
      <span class="pmain"><div class="pname">${name}</div><div class="pmeta"><code class="pfile">${esc(fileName(p))}</code> · ${esc(meta)}</div></span>
      <span class="ptime">${r && r !== "running" ? r.seconds.toFixed(2) + "s" : ""}</span></button>`;
    li.firstElementChild.addEventListener("click", () => select(p));
    ul.appendChild(li);
  }
}
document.querySelectorAll(".chip").forEach((c) => c.addEventListener("click", () => {
  filter = c.dataset.f;
  document.querySelectorAll(".chip").forEach((x) => x.classList.toggle("on", x === c));
  renderList();
}));

// ------------------------------------------------------------------ detail panel
function select(p) {
  selected = p;
  $("#d-group").textContent = p.custom ? "your program" : `${p.group} · ${fileName(p)}`;
  $("#d-name").textContent = title(p);
  $("#d-desc").textContent = p.description || "";
  renderList();
  renderDetail();
  loadCode(p);
}

function renderDetail() {
  const p = selected;
  if (!p) return;
  const r = results[p.id];
  const strip = $("#strip");
  if (!r) {
    strip.className = "result-strip";
    strip.innerHTML = `Expected: ${tags(p.expected || [])}`;
    for (const k of ["graph", "report", "contention", "log"]) $("#pane-" + k).innerHTML = `<p class="empty">Not run yet. Press <b>Run</b>.</p>`;
    tabCount(0);
    return;
  }
  if (r === "running") {
    strip.className = "result-strip run";
    strip.innerHTML = `<span class="big">Running…</span>${p.mode === "hang" ? " this program really deadlocks; the watchdog stops it after about a second." : ""}`;
    return;
  }
  const v = verdict(p, r);
  strip.className = "result-strip " + (v === "info" ? "" : v);
  const how = r.aborted ? "stopped by the watchdog" : r.timed_out ? "killed after timeout" : `exit code ${r.rc}`;
  strip.innerHTML = (v === "pass" ? `<span class="big pass-t">✓ Pass</span>` : v === "fail" ? `<span class="big fail-t">✗ Fail</span>` : "") +
    `<span>Found: ${tags(r.found)}</span>` + (p.custom ? "" : `<span class="muted">expected ${p.expected.length ? p.expected.map(nice).join(", ") : "none"}</span>`) +
    `<span class="right">${r.seconds.toFixed(3)} s · ${how}${r.naive ? " · naive" : ""}</span>`;
  renderGraph(r.report);
  renderReport(r.report);
  renderContention(r.report);
  $("#pane-log").innerHTML = `<pre class="term">${linkify(esc(r.stderr || "(nothing on stderr)"))}</pre>` +
    (r.stdout ? `<h4>Program output</h4><pre class="term">${esc(r.stdout)}</pre>` : "");
  tabCount(r.report ? r.report.findings.length : 0);
  highlightCode();
}

function tabCount(n) {
  document.querySelector('.tabs [data-tab="report"]').innerHTML = "Report" + (n ? `<span class="count">${n}</span>` : "");
}
function showTab(name) {
  document.querySelectorAll(".tabs button").forEach((b) => b.classList.toggle("on", b.dataset.tab === name));
  document.querySelectorAll(".pane").forEach((d) => (d.hidden = d.id !== "pane-" + name));
}
document.querySelectorAll(".tabs button").forEach((b) => b.addEventListener("click", () => showTab(b.dataset.tab)));

function linkify(html) {
  return html
    .replace(/\(([\w.-]+\.(?:cpp|cc|c|h)):(\d+)\)/g, (m, f, l) => `(<a class="src" data-line="${l}">${f}:${l}</a>)`)
    .replace(/^(\[LockSpy\] [A-Z ]+:?)/gm, `<span class="k">$1</span>`)
    .replace(/✔/g, `<span class="ok">✔</span>`);
}
document.addEventListener("click", (e) => {
  const a = e.target.closest("a.src");
  if (!a) return;
  showTab("code");
  const row = document.getElementById("L" + a.dataset.line);
  if (row) row.scrollIntoView({ block: "center", behavior: "smooth" });
});

// ------------------------------------------------------------------ run
async function runOne(p) {
  results[p.id] = "running";
  renderList();
  if (selected === p) renderDetail();
  const naive = $("#opt-naive").checked;
  try {
    const r = await api("/api/run", { program: p.program, args: p.args, mode: p.mode, naive, watchdog_ms: 500 });
    r.naive = naive;
    results[p.id] = r;
  } catch (e) {
    delete results[p.id];
    setStatus("Error: " + e.message);
  }
  renderList();
  updateKpis();
  if (selected === p) renderDetail();
}

async function runSelected() {
  if (!selected || busy) return;
  setBusy(true);
  setStatus(`Running ${selected.id}…`);
  await runOne(selected);
  const r = results[selected.id];
  if (r && r !== "running") setStatus(`${selected.id} finished in ${r.seconds.toFixed(2)} s`);
  setBusy(false);
}

async function runAll() {
  if (busy) return;
  setBusy(true);
  for (let i = 0; i < programs.length; i++) {
    progress(i / programs.length);
    setStatus(`Running ${i + 1}/${programs.length}: ${programs[i].id}`);
    await runOne(programs[i]);
  }
  progress(1);
  setTimeout(() => progress(null), 600);
  const passed = programs.filter((p) => verdict(p, results[p.id]) === "pass").length;
  setStatus(`All tests done: ${passed}/${programs.length} passed`);
  setBusy(false);
}

// ------------------------------------------------------------------ report tab
function renderReport(rep) {
  const box = $("#pane-report");
  if (!rep) { box.innerHTML = `<p class="empty">No report was written. Check the Log tab.</p>`; return; }
  const sup = rep.suppressed || {};
  const skipped = (sup.single_thread || 0) + (sup.gate_lock || 0) + (sup.read_read || 0);
  const note = `<p class="note">${skipped} cycle(s) skipped as safe (gate lock ${sup.gate_lock || 0}, single thread ${sup.single_thread || 0}, read-read ${sup.read_read || 0}). A detector without filters would report ${rep.naive_cycles}.</p>`;
  if (!rep.findings.length) {
    box.innerHTML = `<div class="clean"><div class="big-ico">✓</div><h3>No problems found</h3>
      <p>${rep.threads.length} threads · ${rep.locks.length} locks · ${rep.edges.length} lock-order edges · ${rep.events} lock events</p></div>${note}`;
    return;
  }
  box.innerHTML = rep.findings.map(findingCard).join("") + note;
}

function findingCard(f) {
  const lines = f.text.split("\n");
  const fix = lines.filter((l) => /Suggested fix:|^\s+change /.test(l)).map((l) => esc(l.trim()));
  const cof = lines.filter((l) => l.includes("✔")).map((l) => {
    const m = l.trim().replace(/^✔\s*/, "").match(/^(Mutual exclusion|Hold and wait|No preemption|Circular wait)\s+(.*)$/);
    return m ? `<div><b>✔ ${m[1]}</b>${esc(m[2])}</div>` : "";
  }).join("");
  const steps = f.steps.map((s) => {
    if (f.type === "POTENTIAL_DEADLOCK" || f.type === "LIVE_DEADLOCK")
      return `<div class="step"><span class="th">${esc(s.thread)}</span>holds<span class="lk">${esc(s.holds)}</span>${f.type === "LIVE_DEADLOCK" ? "blocked on" : "wants"}<span class="lk">${esc(s.wants)}</span></div>`;
    return `<div class="step"><span class="th">${esc(s.thread)}</span><span class="lk">${esc(s.wants || s.holds)}</span>${s.stack[0] ? "at <code>" + linkify(esc(s.stack[0])) + "</code>" : ""}</div>`;
  }).join("");
  return `<article class="finding">
    <div class="finding-head"><span class="tag bad">${nice(f.type)}</span><h3>${headline(f)}</h3><span class="when">#${f.id} · ${f.t.toFixed(3)}s</span></div>
    <div class="finding-body">
      <div>${steps}</div>
      ${fix.length ? `<div class="fix">${fix.join("<br>")}</div>` : ""}
      ${f.predicted_by >= 0 ? `<div class="fix">This hang was predicted earlier by finding <b>#${f.predicted_by}</b>.</div>` : ""}
      ${cof ? `<div class="coffman">${cof}</div>` : ""}
      <details><summary>Full text report</summary><pre class="term">${linkify(esc(f.text))}</pre></details>
    </div></article>`;
}
function headline(f) {
  const first = f.text.split("\n")[0].replace(/^\[LockSpy\]\s*/, "").replace(/\s+\(#\d+.*$/, "");
  return esc(first.replace(/^[A-Z ]+:\s*/, "").replace(/^./, (c) => c.toUpperCase()));
}

// ------------------------------------------------------------------ graph tab
function renderGraph(rep) {
  const box = $("#pane-graph");
  if (!rep) { box.innerHTML = `<p class="empty">No report.</p>`; return; }
  const used = new Set();
  rep.edges.forEach((e) => { used.add(e.from); used.add(e.to); });
  rep.locks.forEach((l) => l.acquisitions > 0 && used.add(l.id));
  const nodes = rep.locks.filter((l) => used.has(l.id));
  if (!nodes.length) { box.innerHTML = `<p class="empty">This program didn't take any locks.</p>`; return; }
  const sizes = nodes.map((n) => Math.max(n.name.length * 9.5, `${n.acquisitions} acquisitions`.length * 6.4) + 30);
  const half = Math.max(...sizes) / 2;
  const two = nodes.length === 2;                  // two locks: side by side instead of top/bottom
  const W = Math.max(640, nodes.length * 2 * half * 0.9), H = two ? 260 : Math.max(340, 120 + nodes.length * 34);
  const RX = nodes.length === 1 ? 0 : W / 2 - half - 16, RY = nodes.length === 1 ? 0 : H / 2 - 42;
  const maxWait = Math.max(1, ...nodes.map((n) => n.wait_ns));
  const pos = {};
  nodes.forEach((n, i) => {
    const a = (two ? Math.PI : -Math.PI / 2) + (2 * Math.PI * i) / nodes.length;
    pos[n.id] = { x: W / 2 + RX * Math.cos(a), y: H / 2 + RY * Math.sin(a), hw: sizes[i] / 2, hh: 27, n };
  });
  const tname = (id) => (rep.threads.find((t) => t.id === id) || {}).name || "T" + id;
  const has = new Set(rep.edges.map((e) => e.from + ">" + e.to));
  const clip = (p, dx, dy) => { const t = Math.min(p.hw / Math.abs(dx || 1e-9), p.hh / Math.abs(dy || 1e-9)); return [p.x + dx * t, p.y + dy * t]; };
  let edges = "", labels = "", boxes = "";
  for (const e of rep.edges) {
    const a = pos[e.from], b = pos[e.to];
    if (!a || !b) continue;
    const dx = b.x - a.x, dy = b.y - a.y, len = Math.hypot(dx, dy) || 1, nx = -dy / len, ny = dx / len;
    const bend = has.has(e.to + ">" + e.from) ? 40 : 0;
    const qx = (a.x + b.x) / 2 + nx * bend, qy = (a.y + b.y) / 2 + ny * bend;
    const [x1, y1] = clip(a, qx - a.x, qy - a.y), [x2, y2] = clip(b, qx - b.x, qy - b.y);
    const cls = e.in_cycle ? "cycle" : e.suppressed ? "sup" : "";
    edges += `<path class="g-edge ${cls}" d="M${x1.toFixed(1)},${y1.toFixed(1)} Q${qx.toFixed(1)},${qy.toFixed(1)} ${x2.toFixed(1)},${y2.toFixed(1)}" marker-end="url(#${e.in_cycle ? "ga-red" : "ga"})"><title>${esc(e.site)}</title></path>`;
    const who = e.threads.slice(0, 3).map(tname).join(", ") + (e.threads.length > 3 ? "…" : "");
    labels += `<text class="g-label ${cls}" x="${((x1 + 2 * qx + x2) / 4 + nx * 12).toFixed(1)}" y="${((y1 + 2 * qy + y2) / 4 + ny * 12).toFixed(1)}">${esc(who)} ×${e.count}</text>`;
  }
  for (const id in pos) {
    const p = pos[id], n = p.n, heat = n.wait_ns / maxWait;
    boxes += `<g class="g-node"><title>${esc(n.name)}: waited ${ms(n.wait_ns)} ms in total</title>
      <rect x="${(p.x - p.hw).toFixed(1)}" y="${(p.y - p.hh).toFixed(1)}" width="${(2 * p.hw).toFixed(1)}" height="${2 * p.hh}" rx="9"
        style="fill: color-mix(in srgb, var(--red) ${Math.round(heat * 45)}%, var(--lock-soft))"/>
      <text x="${p.x.toFixed(1)}" y="${(p.y - 7).toFixed(1)}">${esc(n.name)}</text>
      <text class="sub" x="${p.x.toFixed(1)}" y="${(p.y + 11).toFixed(1)}">${n.acquisitions} acquisitions</text></g>`;
  }
  box.innerHTML = `<div class="graph"><svg viewBox="0 0 ${W} ${H}" role="img" aria-label="Lock-order graph">
    <defs><marker id="ga" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse"><path d="M0 0L10 5L0 10z"/></marker>
    <marker id="ga-red" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse"><path d="M0 0L10 5L0 10z"/></marker></defs>
    ${edges}${labels}${boxes}</svg></div>
    <div class="legend"><span><i class="red"></i>deadlock cycle</span><span><i class="dash"></i>cycle skipped as safe</span><span><i></i>lock order (thread × times)</span><span>redder box = more waiting</span></div>`;
}

// ------------------------------------------------------------------ contention tab
function renderContention(rep) {
  const box = $("#pane-contention");
  if (!rep) { box.innerHTML = `<p class="empty">No report.</p>`; return; }
  const locks = rep.locks.filter((l) => l.acquisitions > 0).sort((a, b) => b.wait_ns - a.wait_ns || b.acquisitions - a.acquisitions);
  if (!locks.length) { box.innerHTML = `<p class="empty">No locks were taken.</p>`; return; }
  const max = Math.max(1, ...locks.map((l) => l.wait_ns));
  box.innerHTML = `<div class="tbl-wrap"><table><thead><tr><th>Lock</th><th class="num">Acquired</th><th class="num">Had to wait</th><th>Total wait (ms)</th><th class="num">Max wait</th><th class="num">Total hold</th></tr></thead><tbody>
    ${locks.map((l) => `<tr><td class="mono">${esc(l.name)}</td><td class="num">${l.acquisitions}</td><td class="num">${l.contended}</td>
      <td><span class="meter"><i style="width:${(l.wait_ns / max) * 100}%"></i></span>${ms(l.wait_ns)}</td><td class="num">${ms(l.max_wait_ns)}</td><td class="num">${ms(l.hold_ns)}</td></tr>`).join("")}
    </tbody></table></div>
    <h4>Threads</h4><div class="tbl-wrap"><table><thead><tr><th>Name</th><th>Started in</th></tr></thead><tbody>
    ${rep.threads.map((t) => `<tr><td class="mono">${esc(t.name)}</td><td class="mono">${esc(t.start || "")}</td></tr>`).join("")}</tbody></table></div>`;
}

// ------------------------------------------------------------------ code tab
let code = "";
async function loadCode(p) {
  code = "";
  const box = $("#pane-code");
  if (!p.source) { box.innerHTML = `<p class="empty">No source available for your own program.</p>`; return; }
  try {
    const s = await api("/api/source?path=" + encodeURIComponent(p.source));
    if (selected !== p) return;
    code = s.text;
    highlightCode();
  } catch (e) { box.innerHTML = `<p class="empty">${esc(e.message)}</p>`; }
}
function highlightCode() {
  if (!code || !selected || !selected.source) return;
  const r = results[selected.id];
  const file = selected.source.split("/").pop().replace(/\./g, "\\.");
  const hot = new Set();
  if (r && r !== "running") { const re = new RegExp("\\(" + file + ":(\\d+)\\)", "g"); let m; while ((m = re.exec(r.stderr))) hot.add(+m[1]); }
  const html = code.split("\n").map((l, i) => {
    let h = esc(l);
    h = /^\s*\/\//.test(l) ? `<span class="c">${h}</span>`
      : h.replace(/\b(pthread_(?:mutex|rwlock|cond)_\w+|sem_\w+|lock_guard)\b/g, `<span class="api">$1</span>`).replace(/(\/\/.*)$/, `<span class="c">$1</span>`);
    return `<div class="ln${hot.has(i + 1) ? " hl" : ""}" id="L${i + 1}"><span>${i + 1}</span><span>${h || " "}</span></div>`;
  }).join("");
  $("#pane-code").innerHTML = (hot.size ? `<p class="note" style="margin:0 0 10px">Highlighted lines are the ones LockSpy reported.</p>` : "") + `<div class="code">${html}</div>`;
}

// ------------------------------------------------------------------ benchmark charts
async function loadBench() {
  let b;
  try { b = await api("/api/bench"); } catch (e) { b = { available: false }; }
  if (!b.available) {
    $("#chart-detect").innerHTML = $("#chart-slow").innerHTML = `<p class="empty">Run <code>make bench</code> to fill this in.</p>`;
    $("#kpi-slow-note").textContent = "run make bench first";
    return;
  }
  const T = b.tools;
  const label = { lockspy: "LockSpy", naive: "LockSpy (naive)", tsan: "ThreadSanitizer", helgrind: "Helgrind" };
  const order = ["lockspy", "naive", "tsan", "helgrind"].filter((t) => T[t]);
  const total = (t) => T[t].caught + T[t].missed + T[t].false_alarms + T[t].clean;
  $("#chart-detect").innerHTML = order.map((t) => {
    const x = T[t], n = total(t);
    return `<div class="crow${t === "lockspy" ? " us" : ""}"><span class="name">${label[t]}</span>
      <span class="stack" title="${x.caught} caught, ${x.missed} missed, ${x.false_alarms} false alarms">
        <i class="c" style="width:${(x.caught / n) * 100}%"></i><i class="m" style="width:${(x.missed / n) * 100}%"></i><i class="f" style="width:${(x.false_alarms / n) * 100}%"></i></span>
      <span class="val">${x.caught}/${x.caught + x.missed} · ${x.false_alarms} FA</span></div>`;
  }).join("") + `<div class="ckey"><span><i style="background:var(--green)"></i>bug caught</span><span><i style="background:var(--amber)"></i>bug missed</span><span><i style="background:var(--red)"></i>false alarm</span><span><i style="background:var(--line-2)"></i>correctly silent</span></div>`;
  const slow = ["lockspy", "tsan", "helgrind"].filter((t) => T[t] && T[t].slowdown);
  const lg = (v) => Math.log(v) / Math.log(200);
  $("#chart-slow").innerHTML = slow.map((t) => `<div class="crow${t === "lockspy" ? " us" : ""}"><span class="name">${label[t]}</span>
      <span class="bar"><i style="width:${Math.max(2, lg(T[t].slowdown) * 100)}%"></i></span><span class="val">${T[t].slowdown}×</span></div>`).join("") +
    `<div class="axis">${[1, 2, 5, 10, 20, 50, 100, 200].map((v) => `<span style="left:${lg(v) * 100}%">${v}×</span>`).join("")}</div>
     <p class="note">ThreadSanitizer needs a recompile; LockSpy and Helgrind don't. TSan was faster than LockSpy on the tight uncontended loop (see results/RESULTS.md).</p>`;
  if (T.lockspy && T.lockspy.slowdown) setKpi("#kpi-slow", T.lockspy.slowdown + "×", "", "");
  if (T.tsan && T.tsan.slowdown) $("#kpi-slow-note").textContent = `ThreadSanitizer ${T.tsan.slowdown}×, Helgrind ${T.helgrind ? T.helgrind.slowdown + "×" : "–"}`;
}

// ------------------------------------------------------------------ build / own program / start
async function rebuild() {
  setBusy(true);
  setStatus("Rebuilding…");
  try {
    const r = await api("/api/build", {});
    setStatus(r.ok ? "Build OK" : "Build failed, see the Log tab");
    if (!r.ok) { showTab("log"); $("#pane-log").innerHTML = `<pre class="term">${esc(r.output)}</pre>`; }
    await loadPrograms();
  } catch (e) { setStatus("Build error: " + e.message); }
  setBusy(false);
}

$("#own-form").addEventListener("submit", async (e) => {
  e.preventDefault();
  const parts = $("#own-path").value.trim().split(/\s+/);
  if (!parts[0]) return;
  const p = { id: parts.join(" "), program: parts[0], args: parts.slice(1).join(" "), mode: "hang", custom: true, expected: [],
    description: "Your own program. If it deadlocks, the watchdog reports it and stops it." };
  select(p);
  if (!busy) { setBusy(true); await runOne(p); setBusy(false); }
});

async function loadPrograms() {
  const r = await api("/api/programs");
  programs = r.programs;
  renderList();
  return r;
}

// intro panel: hide/show, remembered in this browser only
function showIntro(on) {
  $("#intro").hidden = !on;
  $("#intro-show").hidden = on;
  try { localStorage.setItem("lockspy-intro", on ? "1" : "0"); } catch (e) { /* storage blocked: fine */ }
}
$("#intro-hide").addEventListener("click", () => showIntro(false));
$("#intro-show").addEventListener("click", () => showIntro(true));
try { if (localStorage.getItem("lockspy-intro") === "0") showIntro(false); } catch (e) { /* ignore */ }

$("#run-btn").addEventListener("click", runSelected);
$("#runall-btn").addEventListener("click", runAll);
$("#build-btn").addEventListener("click", rebuild);

(async () => {
  try {
    const r = await loadPrograms();
    select(programs.find((p) => p.id === "buggy/ab_ba") || programs[0]);
    loadBench();
    if (!r.lib_built) { setStatus("LockSpy isn't built yet: press Rebuild"); return; }
    await runAll();          // fill the dashboard straight away
  } catch (e) { setStatus("Can't reach the server: " + e.message); }
})();
