// app.js: the web page. Gets the program list from ui/server.py, runs programs through /api/run
// and shows the JSON report LockSpy writes.
"use strict";

const $ = (s) => document.querySelector(s);
const esc = (s) => String(s ?? "").replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));
const KIND = {
  POTENTIAL_DEADLOCK: "Potential deadlock",
  LIVE_DEADLOCK: "Live deadlock",
  RELOCK: "Relock of a non-recursive lock",
  BAD_UNLOCK: "Unlock of a lock not held",
  HELD_AT_EXIT: "Lock held at thread exit",
};
const GROUPS = { buggy: "Buggy programs", correct: "Correct programs", tricky: "Tricky (safe, but look buggy)", perf: "Performance" };

let PROGRAMS = [];
let current = null;           // selected program object (or a custom one)
const results = {};           // program id -> last run result
let busy = false;

// ---------------------------------------------------------------- helpers
async function api(path, body) {
  const opt = body === undefined ? {} : { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) };
  const r = await fetch(path, opt);
  const j = await r.json().catch(() => ({ error: "bad response from the server" }));
  if (!r.ok) throw new Error(j.error || r.statusText);
  return j;
}
const sameSet = (a, b) => a.length === b.length && [...a].sort().join() === [...b].sort().join();
const verdictOf = (p, res) => (p && p.expected && !p.custom ? (sameSet(res.found, p.expected) && !res.timed_out ? "pass" : "fail") : "info");
const pills = (kinds) => (kinds.length ? kinds.map((k) => `<span class="pill bad">${k}</span>`).join(" ") : `<span class="pill none">NONE</span>`);
const ms = (ns) => (ns / 1e6).toFixed(3);
function setStatus(t) { $("#status").textContent = t; }
function setBusy(b) {
  busy = b;
  for (const id of ["#run-btn", "#runall-btn", "#build-btn", "#custom-btn"]) $(id).disabled = b;
}

// ---------------------------------------------------------------- sidebar
function renderList() {
  const box = $("#prog-list");
  box.innerHTML = "";
  for (const g of Object.keys(GROUPS)) {
    const items = PROGRAMS.filter((p) => p.group === g);
    if (!items.length) continue;
    box.insertAdjacentHTML("beforeend", `<div class="side-title">${GROUPS[g]}</div>`);
    for (const p of items) {
      const b = document.createElement("button");
      b.type = "button";
      b.className = "prog" + (current && current.id === p.id ? " on" : "");
      b.dataset.id = p.id;
      const r = results[p.id];
      const dot = r === "running" ? "running" : r ? verdictOf(p, r) : "";
      b.innerHTML = `<span class="dot ${dot}"></span><span><div class="pname">${esc(p.id.replace(/^[a-z]+\//, ""))}</div>
        <div class="pexp">expects ${p.expected.length ? p.expected.join(", ").toLowerCase().replace(/_/g, " ") : "no warnings"}</div></span>`;
      b.addEventListener("click", () => select(p));
      box.appendChild(b);
    }
  }
}

// ---------------------------------------------------------------- program view
function select(p) {
  current = p;
  $("#view-all").hidden = true;
  $("#view-program").hidden = false;
  $("#p-group").textContent = p.custom ? "custom" : p.group;
  $("#p-name").textContent = p.id;
  $("#p-desc").textContent = p.description || "";
  $("#p-expect").innerHTML = p.custom ? "" : `Expected: ${pills(p.expected)}`;
  renderList();
  const r = results[p.id];
  if (r && r !== "running") showResult(p, r);
  else clearResult(r === "running" ? "Running..." : null);
  loadSource(p);
}

function clearResult(msg) {
  const v = $("#verdict");
  if (msg) { v.hidden = false; v.className = "verdict running"; v.innerHTML = `<b>${esc(msg)}</b>`; } else v.hidden = true;
  $("#tab-findings").innerHTML = `<p class="empty">Click <b>Run</b> to run this program under LockSpy.</p>`;
  $("#tab-graph").innerHTML = `<p class="empty">The lock-order graph appears after a run.</p>`;
  $("#tab-contention").innerHTML = `<p class="empty">Per-lock wait and hold times appear after a run.</p>`;
  $("#tab-output").innerHTML = `<p class="empty">LockSpy's stderr appears after a run.</p>`;
  setTabCount(0);
}

async function runCurrent() {
  if (!current || busy) return;
  const p = current;
  setBusy(true);
  results[p.id] = "running";
  renderList();
  clearResult(p.mode === "hang" ? "Running... this one really deadlocks, the watchdog should stop it in about a second." : "Running...");
  setStatus(`Running ${p.id}…`);
  try {
    const res = await api("/api/run", {
      program: p.program, args: p.args, mode: p.mode,
      naive: $("#opt-naive").checked, watchdog_ms: +$("#opt-watchdog").value || 500,
    });
    results[p.id] = res;
    setStatus(`${p.id}: done in ${res.seconds}s`);
    if (current === p) showResult(p, res);
  } catch (e) {
    delete results[p.id];
    setStatus("Error: " + e.message);
    if (current === p) { const v = $("#verdict"); v.hidden = false; v.className = "verdict fail"; v.innerHTML = `<b>Could not run:</b> ${esc(e.message)}`; }
  } finally {
    setBusy(false);
    renderList();
  }
}

function showResult(p, res) {
  const v = $("#verdict");
  const verdict = verdictOf(p, res);
  v.hidden = false;
  v.className = "verdict " + verdict;
  const label = verdict === "pass" ? `<span class="pill pass">PASS</span>` : verdict === "fail" ? `<span class="pill fail">FAIL</span>` : "";
  const how = res.aborted ? "watchdog aborted the hung program" : res.timed_out ? "killed after the timeout" : `exit code ${res.rc}`;
  v.innerHTML = `${label}<span>Reported: ${pills(res.found)}</span>
    <span class="meta">${res.seconds.toFixed(3)} s · ${how}${$("#opt-naive").checked ? " · naive mode" : ""}</span>`;
  const rep = res.report;
  renderFindings(rep, p);
  renderGraph(rep);
  renderContention(rep);
  $("#tab-output").innerHTML = `<pre class="term">${linkify(esc(res.stderr || "(no output on stderr)"))}</pre>` +
    (res.stdout ? `<p class="sub-h">Program stdout</p><pre class="term">${esc(res.stdout)}</pre>` : "");
  setTabCount(rep ? rep.findings.length : 0);
  highlightSource();
}

function setTabCount(n) {
  const b = document.querySelector('.tabs button[data-tab="findings"]');
  b.innerHTML = "Findings" + (n ? `<span class="count">${n}</span>` : "");
}

// Turn "(ab_ba.cpp:21)" into a link that jumps to that line in the Source tab.
function linkify(html) {
  return html
    .replace(/\(([\w.-]+\.(?:cpp|cc|c|h)):(\d+)\)/g, (m, f, l) => `(<a class="src" data-line="${l}">${f}:${l}</a>)`)
    .replace(/^(\[LockSpy\] [A-Z ]+:?)/gm, `<span class="k">$1</span>`)
    .replace(/(✔)/g, `<span class="ok">$1</span>`);
}

// ---------------------------------------------------------------- findings tab
function renderFindings(rep, p) {
  const box = $("#tab-findings");
  if (!rep) { box.innerHTML = `<p class="empty">No report was written (did the program start?). See Raw output.</p>`; return; }
  const sup = rep.suppressed || {};
  const supTotal = (sup.single_thread || 0) + (sup.gate_lock || 0) + (sup.read_read || 0);
  const supLine = `<p class="sup">Cycles skipped as safe: ${supTotal} (gate lock ${sup.gate_lock || 0}, single thread ${sup.single_thread || 0}, read-read ${sup.read_read || 0}). Without filtering: ${rep.naive_cycles}.</p>`;
  if (!rep.findings.length) {
    box.innerHTML = `<div class="clean-box"><h3>Nothing found</h3><p>${rep.locks.length} lock(s), ${rep.edges.length} lock-order edge(s), ${rep.threads.length} thread(s), ${rep.events} lock events.</p></div>${supLine}`;
    return;
  }
  box.innerHTML = rep.findings.map((f) => findingCard(f)).join("") + supLine;
}

function findingCard(f) {
  const lines = f.text.split("\n");
  const fixLines = lines.filter((l) => /Suggested fix:|^\s+change /.test(l)).map((l) => l.trim());
  const cof = lines.filter((l) => l.includes("✔")).map((l) => {
    const m = l.trim().replace(/^✔\s*/, "").match(/^(Mutual exclusion|Hold and wait|No preemption|Circular wait)\s+(.*)$/);
    return m ? `<div><b>✔ ${m[1]}:</b><span>${esc(m[2])}</span></div>` : "";
  }).join("");
  const steps = f.steps.map((s) => {
    if (f.type === "POTENTIAL_DEADLOCK" || f.type === "LIVE_DEADLOCK")
      return `<div class="step"><span class="chip thread">${esc(s.thread)}</span><span class="arrow">holds</span><span class="chip lock">${esc(s.holds)}</span><span class="arrow">${f.type === "LIVE_DEADLOCK" ? "blocked on" : "wants"}</span><span class="chip lock">${esc(s.wants)}</span></div>`;
    const lock = s.wants || s.holds;
    return `<div class="step"><span class="chip thread">${esc(s.thread)}</span><span class="arrow">·</span><span class="chip lock">${esc(lock)}</span>${s.stack[0] ? `<span class="arrow">at</span><code>${linkify(esc(s.stack[0]))}</code>` : ""}</div>`;
  }).join("");
  const predicted = f.predicted_by >= 0 ? `<div class="fix">LockSpy warned about this earlier (finding #${f.predicted_by}).</div>` : "";
  return `<article class="finding">
    <div class="finding-head"><span class="pill bad">${f.type}</span><h3>${KIND[f.type] || f.type}</h3><span class="t">#${f.id} · t=${f.t.toFixed(3)}s</span></div>
    <div class="finding-body">
      <div class="steps-list">${steps}</div>
      ${fixLines.length ? `<div class="fix">${fixLines.map(esc).join("<br>")}</div>` : ""}
      ${predicted}
      ${cof ? `<div class="coffman">${cof}</div>` : ""}
      <details class="full"><summary>show full report</summary><pre class="term">${linkify(esc(f.text))}</pre></details>
    </div></article>`;
}

// ---------------------------------------------------------------- graph tab
function renderGraph(rep) {
  const box = $("#tab-graph");
  if (!rep) { box.innerHTML = `<p class="empty">No report.</p>`; return; }
  const used = new Set();
  rep.edges.forEach((e) => { used.add(e.from); used.add(e.to); });
  rep.locks.forEach((l) => { if (l.acquisitions > 0) used.add(l.id); });
  const nodes = rep.locks.filter((l) => used.has(l.id));
  if (!nodes.length) { box.innerHTML = `<p class="empty">This program took no locks.</p>`; return; }
  // Lay the locks out on an ellipse that uses the whole drawing area.
  const sizes = nodes.map((n) => {
    const sub = `${n.acquisitions} acq · ${ms(n.wait_ns)} ms`;
    return Math.max(n.name.length * 9.5, sub.length * 7) + 26;
  });
  const maxHalf = Math.max(...sizes) / 2;
  const W = Math.max(620, nodes.length * 2 * maxHalf * 0.9), H = Math.max(360, 140 + nodes.length * 34), cx = W / 2, cy = H / 2;
  const RX = nodes.length === 1 ? 0 : W / 2 - maxHalf - 14, RY = nodes.length === 1 ? 0 : H / 2 - 40;
  const maxWait = Math.max(1, ...nodes.map((n) => n.wait_ns));
  const pos = {};
  nodes.forEach((n, i) => {
    const a = -Math.PI / 2 + (2 * Math.PI * i) / nodes.length;
    pos[n.id] = { x: cx + RX * Math.cos(a), y: cy + RY * Math.sin(a), hw: sizes[i] / 2, hh: 26, n };
  });
  const threadName = (id) => (rep.threads.find((t) => t.id === id) || {}).name || "T" + id;
  const has = new Set(rep.edges.map((e) => e.from + ">" + e.to));
  const clip = (p, dx, dy) => {               // point where a ray from the box centre leaves the box
    const t = Math.min(p.hw / Math.abs(dx || 1e-9), p.hh / Math.abs(dy || 1e-9));
    return [p.x + dx * t, p.y + dy * t];
  };
  let edges = "", labels = "";
  for (const e of rep.edges) {
    const a = pos[e.from], b = pos[e.to];
    if (!a || !b) continue;
    const dx = b.x - a.x, dy = b.y - a.y, len = Math.hypot(dx, dy) || 1;
    const bend = has.has(e.to + ">" + e.from) ? 38 : 0;          // curve both edges of an A<->B pair
    const nx = -dy / len, ny = dx / len;
    const qx = (a.x + b.x) / 2 + nx * bend, qy = (a.y + b.y) / 2 + ny * bend;
    const [x1, y1] = clip(a, qx - a.x, qy - a.y);
    const [x2, y2] = clip(b, qx - b.x, qy - b.y);
    const cls = e.in_cycle ? "cycle" : e.suppressed ? "sup" : "";
    edges += `<path class="g-edge ${cls}" d="M${x1.toFixed(1)},${y1.toFixed(1)} Q${qx.toFixed(1)},${qy.toFixed(1)} ${x2.toFixed(1)},${y2.toFixed(1)}" marker-end="url(#${e.in_cycle ? "ga-red" : "ga"})"><title>${esc(e.site)}</title></path>`;
    const names = e.threads.slice(0, 3).map(threadName).join(",") + (e.threads.length > 3 ? ",…" : "");
    const lx = (x1 + 2 * qx + x2) / 4 + nx * 10, ly = (y1 + 2 * qy + y2) / 4 + ny * 10;
    labels += `<text class="g-label ${e.in_cycle ? "cycle" : ""}" x="${lx.toFixed(1)}" y="${ly.toFixed(1)}">${esc(names)} ×${e.count}</text>`;
  }
  let nodeSvg = "";
  for (const id in pos) {
    const p = pos[id], n = p.n, heat = n.wait_ns / maxWait;
    nodeSvg += `<g class="g-node"><title>${esc(n.name)}: ${n.acquisitions} acquisitions, ${ms(n.wait_ns)} ms waiting</title>
      <rect x="${(p.x - p.hw).toFixed(1)}" y="${(p.y - p.hh).toFixed(1)}" width="${(2 * p.hw).toFixed(1)}" height="${2 * p.hh}" rx="7"
        style="fill: color-mix(in srgb, #ff8182 ${Math.round(heat * 70)}%, var(--brass-fill))"/>
      <text x="${p.x.toFixed(1)}" y="${(p.y - 7).toFixed(1)}">${esc(n.name)}</text>
      <text class="sub" x="${p.x.toFixed(1)}" y="${(p.y + 11).toFixed(1)}">${n.acquisitions} acq · ${ms(n.wait_ns)} ms</text></g>`;
  }
  box.innerHTML = `<div class="graph-wrap"><svg viewBox="0 0 ${W} ${H}" role="img" aria-label="Lock-order graph">
    <defs>
      <marker id="ga" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse"><path d="M0 0L10 5L0 10z"/></marker>
      <marker id="ga-red" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse"><path d="M0 0L10 5L0 10z"/></marker>
    </defs>${edges}${labels}${nodeSvg}</svg></div>
    <div class="legend"><span><i class="red"></i>deadlock cycle</span><span><i class="dash"></i>cycle skipped as safe</span>
    <span><i></i>other lock order (label: thread × count)</span><span>redder box = more waiting</span></div>`;
}

// ---------------------------------------------------------------- contention tab
function renderContention(rep) {
  const box = $("#tab-contention");
  if (!rep) { box.innerHTML = `<p class="empty">No report.</p>`; return; }
  const locks = rep.locks.filter((l) => l.acquisitions > 0).sort((a, b) => b.wait_ns - a.wait_ns || b.acquisitions - a.acquisitions);
  if (!locks.length) { box.innerHTML = `<p class="empty">No lock was acquired.</p>`; return; }
  const maxWait = Math.max(1, ...locks.map((l) => l.wait_ns));
  const rows = locks.map((l, i) => `<tr><td class="num">${i + 1}</td><td class="mono">${esc(l.name)}</td><td>${l.kind}${l.recursive ? " (recursive)" : ""}</td>
    <td class="num">${l.acquisitions}</td><td class="num">${l.contended}</td>
    <td><span class="heat" style="width:${Math.max(2, Math.round((l.wait_ns / maxWait) * 90))}px"></span>${ms(l.wait_ns)}</td>
    <td class="num">${ms(l.max_wait_ns)}</td><td class="num">${ms(l.hold_ns)}</td><td class="num">${ms(l.max_hold_ns)}</td></tr>`).join("");
  const threads = rep.threads.map((t) => `<tr><td class="num">T${t.id}</td><td class="mono">${esc(t.name)}</td><td class="mono">${esc(t.start || "")}</td></tr>`).join("");
  box.innerHTML = `<p class="desc">Sorted by total time threads spent waiting for each lock.</p>
    <div class="table-wrap"><table><thead><tr><th class="num">#</th><th>Lock</th><th>Kind</th><th class="num">Acquired</th><th class="num">Contended</th>
    <th>Wait ms</th><th class="num">Max wait</th><th class="num">Hold ms</th><th class="num">Max hold</th></tr></thead><tbody>${rows}</tbody></table></div>
    <p class="sub-h">Threads</p>
    <div class="table-wrap"><table><thead><tr><th class="num">Id</th><th>Name</th><th>Started in</th></tr></thead><tbody>${threads}</tbody></table></div>`;
}

// ---------------------------------------------------------------- source tab
let sourceText = "";
async function loadSource(p) {
  const box = $("#tab-source");
  sourceText = "";
  if (!p.source) { box.innerHTML = `<p class="empty">No source file for a custom program.</p>`; return; }
  try {
    const s = await api("/api/source?path=" + encodeURIComponent(p.source));
    if (current !== p) return;
    sourceText = s.text;
    highlightSource();
  } catch (e) { box.innerHTML = `<p class="empty">${esc(e.message)}</p>`; }
}

function highlightSource() {
  const box = $("#tab-source");
  if (!sourceText) return;
  const res = current && results[current.id];
  const file = current.source ? current.source.split("/").pop() : "";
  const hot = new Set();
  if (res && res !== "running") {
    const re = new RegExp("\\(" + file.replace(/\./g, "\\.") + ":(\\d+)\\)", "g");
    let m;
    while ((m = re.exec(res.stderr))) hot.add(+m[1]);
  }
  const lines = sourceText.split("\n").map((l, i) => {
    let h = esc(l);
    if (/^\s*\/\//.test(l)) h = `<span class="c">${h}</span>`;
    else h = h.replace(/\b(pthread_(?:mutex|rwlock|cond)_\w+|sem_\w+|lock_guard)\b/g, `<span class="lk">$1</span>`)
              .replace(/(\/\/.*)$/, `<span class="c">$1</span>`);
    return `<div class="ln${hot.has(i + 1) ? " hl" : ""}" id="L${i + 1}"><span>${i + 1}</span><span>${h || " "}</span></div>`;
  }).join("");
  box.innerHTML = (hot.size ? `<p class="desc" style="margin-bottom:10px">Highlighted: lines LockSpy named in its report (${[...hot].sort((a, b) => a - b).join(", ")}).</p>` : "") +
    `<div class="code">${lines}</div>`;
}

// ---------------------------------------------------------------- tabs
function showTab(name) {
  document.querySelectorAll(".tabs button").forEach((b) => b.classList.toggle("on", b.dataset.tab === name));
  document.querySelectorAll(".tab-body").forEach((d) => (d.hidden = d.id !== "tab-" + name));
}
document.querySelectorAll(".tabs button").forEach((b) => b.addEventListener("click", () => showTab(b.dataset.tab)));
document.addEventListener("click", (e) => {
  const a = e.target.closest("a.src");
  if (!a) return;
  showTab("source");
  const row = document.getElementById("L" + a.dataset.line);
  if (row) row.scrollIntoView({ block: "center", behavior: "smooth" });
});

// ---------------------------------------------------------------- run all
async function runAll() {
  if (busy) return;
  setBusy(true);
  current = null;
  $("#view-program").hidden = true;
  $("#view-all").hidden = false;
  const tbody = $("#suite-table tbody");
  tbody.innerHTML = PROGRAMS.map((p) => `<tr data-id="${esc(p.id)}"><td class="mono">${esc(p.id)}</td><td>${pills(p.expected)}</td><td class="rep">…</td><td class="num t"></td><td class="res">waiting</td></tr>`).join("");
  let pass = 0, done = 0;
  const score = () => ($("#score").innerHTML = `${pass}/${PROGRAMS.length} <small>passed${done < PROGRAMS.length ? ` (running ${done + 1} of ${PROGRAMS.length})` : ""}</small>`);
  score();
  for (const p of PROGRAMS) {
    const row = tbody.querySelector(`tr[data-id="${CSS.escape(p.id)}"]`);
    row.querySelector(".res").textContent = "running…";
    results[p.id] = "running";
    renderList();
    setStatus(`Running ${p.id}…`);
    try {
      const res = await api("/api/run", { program: p.program, args: p.args, mode: p.mode, naive: $("#opt-naive").checked, watchdog_ms: 500 });
      results[p.id] = res;
      const v = verdictOf(p, res);
      if (v === "pass") pass++;
      row.querySelector(".rep").innerHTML = pills(res.found);
      row.querySelector(".t").textContent = res.seconds.toFixed(2) + " s";
      row.querySelector(".res").innerHTML = `<span class="pill ${v}">${v.toUpperCase()}</span>`;
    } catch (e) {
      delete results[p.id];
      row.querySelector(".res").innerHTML = `<span class="pill fail">ERROR</span> ${esc(e.message)}`;
    }
    done++;
    score();
    renderList();
  }
  setStatus(`Suite finished: ${pass}/${PROGRAMS.length} passed`);
  setBusy(false);
}
$("#suite-table tbody").addEventListener("click", (e) => {
  const tr = e.target.closest("tr");
  const p = tr && PROGRAMS.find((x) => x.id === tr.dataset.id);
  if (p && !busy) select(p);
});

// ---------------------------------------------------------------- build / custom / boot
async function build() {
  setBusy(true);
  setStatus("Building with make…");
  try {
    const r = await api("/api/build", {});
    setStatus(r.ok ? "Build finished" : "Build failed (see Raw output)");
    if (!r.ok) { showTab("output"); $("#tab-output").innerHTML = `<pre class="term">${esc(r.output)}</pre>`; }
    await loadPrograms();
  } catch (e) { setStatus("Build error: " + e.message); }
  setBusy(false);
}

function runCustom() {
  const path = $("#custom-path").value.trim();
  if (!path) { $("#custom-path").focus(); return; }
  const args = $("#custom-args").value.trim();
  const p = { id: (path + " " + args).trim(), program: path, args, mode: "hang", custom: true, expected: [], description: "Your own program, run under LockSpy. If it deadlocks, the watchdog reports it and stops it." };
  select(p);
  runCurrent();
}

async function loadPrograms() {
  const r = await api("/api/programs");
  PROGRAMS = r.programs;
  renderList();
  setStatus(r.lib_built ? `${PROGRAMS.length} programs ready` : "LockSpy is not built yet: press Build");
  return r;
}

$("#run-btn").addEventListener("click", runCurrent);
$("#runall-btn").addEventListener("click", runAll);
$("#build-btn").addEventListener("click", build);
$("#custom-btn").addEventListener("click", runCustom);
$("#custom-args").addEventListener("keydown", (e) => { if (e.key === "Enter") runCustom(); });

loadPrograms().then(() => {
  const first = PROGRAMS.find((p) => p.id === "buggy/ab_ba") || PROGRAMS[0];
  if (first) select(first);
}).catch((e) => setStatus("Cannot reach the server: " + e.message));
