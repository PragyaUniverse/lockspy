// tour.js: the step-by-step guide. Each step highlights part of the page, explains it, and can
// first do something (open a program, switch tab) so the reader sees a real example.
// Uses programs / results / select / showTab / runOne from app.js.
"use strict";

(function () {
  const $ = (s) => document.querySelector(s);
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

  // Make sure a program has a finished result (the page runs everything on load, so usually it does).
  async function ensureRun(id) {
    const p = programs.find((x) => x.id === id) || programs.find((x) => x.program === id);
    if (!p) return null;
    for (let i = 0; i < 150; i++) {
      const r = results[p.id];
      if (r && r !== "running") return p;
      if (!r && !busy) { await runOne(p); return p; }
      setWait(true);
      await sleep(200);
    }
    return p;
  }
  async function open(id, tab) {
    resetFilter();
    const p = await ensureRun(id);
    setWait(false);
    if (p) select(p);
    if (tab) showTab(tab);
  }
  function resetFilter() {
    const all = document.querySelector('.chip[data-f="all"]');
    if (all && !all.classList.contains("on")) all.click();
  }

  const STEPS = [
    {
      title: "Welcome to LockSpy",
      body: `<p>LockSpy watches a multithreaded C/C++ program while it runs and tells you about
        <b>deadlocks</b> (threads waiting for each other forever), <b>lock mistakes</b> and <b>slow locks</b>.</p>
        <p>This guide takes about a minute. It will click around the page for you to show real results.</p>`,
    },
    {
      target: ".kpis", place: "below",
      title: "The score",
      body: `<p>Every test program runs automatically when the page opens. These cards sum it up:</p>
        <ul><li><b>Bugs caught</b>: buggy programs where LockSpy found the right problem</li>
        <li><b>False alarms</b>: warnings on programs that are actually fine (should be 0)</li>
        <li><b>Tests passed</b>: all 18 runs</li>
        <li><b>Average slowdown</b>: how much slower programs run under LockSpy</li></ul>`,
    },
    {
      target: ".list-card", place: "right",
      title: "The test programs",
      body: `<p>Each row is a small C++ program from <code>tests/</code>. A green <b>✓</b> means LockSpy reported
        exactly what we expected; a red <b>!</b> means it didn't.</p>
        <ul><li><b>Buggy</b>: have a real bug and must be flagged</li>
        <li><b>Correct</b>: fixed versions, must stay quiet</li>
        <li><b>Tricky</b>: look buggy but are safe, must stay quiet</li></ul>
        <p>Use the chips at the top to filter.</p>`,
    },
    {
      target: ".detail-head", place: "below", before: () => open("buggy/ab_ba", "graph"),
      title: "Example: the AB-BA bug",
      body: `<p>We opened <b>ab_ba</b>. Thread <b>P</b> locks A then B, thread <b>Q</b> locks B then A.
        If both grab their first lock at the same time, they wait for each other forever.</p>
        <p>In this run they didn't overlap, so the program finished fine. <b>Run</b> runs it again.</p>`,
    },
    {
      target: "#strip", place: "below",
      title: "The result",
      body: `<p>LockSpy still found a <span class="tag bad">Potential deadlock</span>: it noticed the two
        threads take the locks in opposite orders, so another run <i>could</i> freeze.</p>
        <p>Found matches expected, so the test passes.</p>`,
    },
    {
      target: "#pane-graph", place: "above", before: () => showTab("graph"),
      title: "The lock graph",
      body: `<p>Each box is a lock. An arrow <b>A → B</b> means "some thread asked for B while holding A".
        The label says which thread and how many times.</p>
        <p>The arrows here go in a <b style="color:var(--red)">red circle</b>, A → B → A. A circle means the
        locks are taken in inconsistent orders, which is exactly what can deadlock.</p>`,
    },
    {
      target: "#pane-report", place: "above", before: () => showTab("report"),
      title: "The report and the fix",
      body: `<p>Who holds what and wants what, then the <b>suggested fix</b> in green: <i>"acquire A before B in
        every thread"</i>, plus the exact line to change.</p>
        <p>Below are the four conditions every deadlock needs (from the OS course): mutual exclusion, hold and
        wait, no preemption, circular wait. LockSpy shows the evidence for each one.</p>`,
    },
    {
      target: ".detail-card", place: "left", before: () => open("buggy/philosophers_naive", "graph"),
      title: "A real freeze: dining philosophers",
      body: `<p>Five philosophers each pick up their left fork, then wait for the right one. This program
        <b>really</b> deadlocks.</p>
        <p>LockSpy warned about the circle first (<span class="tag bad">Potential deadlock</span>), then its
        watchdog noticed all five threads stuck and reported a <span class="tag bad">Live deadlock</span>,
        stopping the program after about a second. The graph shows the 5-lock circle.</p>`,
    },
    {
      target: ".toggle", place: "below", before: () => open("tricky/gate_lock_safe", "report"),
      title: "No false alarms",
      body: `<p><b>gate_lock_safe</b> takes A and B in both orders, but always while holding a third lock G first,
        so the two parts can never overlap. LockSpy knows this and stays quiet ("cycles skipped as safe").</p>
        <p>Turn on <b>Naive mode</b> and press Run to see what a basic checker would say: a false alarm.</p>`,
    },
    {
      target: "#pane-contention", place: "above", before: () => open("perf/contention_heavy", "contention"),
      title: "Slow locks",
      body: `<p>The <b>Contention</b> tab ranks locks by how long threads spent waiting for them. In this program
        <code>hot_lock</code> is the bottleneck. That's the lock to fix first if the program is slow.</p>`,
    },
    {
      target: "#pane-code", place: "above", before: () => open("buggy/ab_ba", "code"),
      title: "The code",
      body: `<p>The program's source. The highlighted lines are the ones LockSpy pointed at in its report.
        Line numbers in the report and the Log tab are links that jump here.</p>`,
    },
    {
      target: ".bench", place: "above",
      title: "Compared with other tools",
      body: `<p>From our benchmark (<code>make bench</code>), on the same 16 programs:</p>
        <ul><li>Left: bugs caught (green), missed (orange) and false alarms (red) for LockSpy, ThreadSanitizer and Helgrind</li>
        <li>Right: average slowdown. ThreadSanitizer also needs the program recompiled; LockSpy doesn't.</li></ul>`,
    },
    {
      target: ".own", place: "right",
      title: "Try your own program",
      body: `<p>Type the path of any program inside the project folder, with arguments, and press Run. For example
        <code>./build/tests/buggy/ab_ba --hang</code>.</p>
        <p>If it freezes, the watchdog reports the deadlock and stops it.</p>`,
    },
    {
      target: "#runall-btn", place: "below",
      title: "That's it",
      body: `<p><b>Run all tests</b> runs the whole suite again. You can reopen this guide any time with the
        <b>Guide</b> button.</p>
        <p>Want the details? See <code>docs/INTRO.md</code> in the project.</p>`,
    },
  ];

  let i = 0, active = false, seq = 0;
  const tour = $("#tour"), spot = $("#tour-spot"), pop = $("#tour-pop");
  // four dark panels around the highlighted area (top, bottom, left, right)
  const shades = [0, 1, 2, 3].map(() => { const d = document.createElement("div"); d.className = "tour-shade"; tour.insertBefore(d, spot); return d; });
  function shade(x, y, w, h) {
    const vw = window.innerWidth, vh = window.innerHeight;
    const box = (d, l, t, ww, hh) => Object.assign(d.style, { left: l + "px", top: t + "px", width: Math.max(0, ww) + "px", height: Math.max(0, hh) + "px" });
    box(shades[0], 0, 0, vw, y);
    box(shades[1], 0, y + h, vw, vh - y - h);
    box(shades[2], 0, y, x, h);
    box(shades[3], x + w, y, vw - x - w, h);
  }

  function setWait(on) {
    const w = pop.querySelector(".tour-wait");
    if (on && !w) $("#tour-body").insertAdjacentHTML("beforeend", `<p class="tour-wait">Waiting for the test to finish…</p>`);
    if (!on && w) w.remove();
  }

  function place() {
    if (!active) return;
    const s = STEPS[i];
    const el = s.target && document.querySelector(s.target);
    const vw = window.innerWidth, vh = window.innerHeight, pw = pop.offsetWidth, ph = pop.offsetHeight, gap = 14;
    if (!el) {                                    // centred welcome card
      spot.classList.add("center");
      shade(vw / 2, vh / 2, 0, 0);
      pop.style.left = (vw - pw) / 2 + "px";
      pop.style.top = Math.max(20, (vh - ph) / 2) + "px";
      return;
    }
    spot.classList.remove("center");
    const r = el.getBoundingClientRect();
    const top = Math.max(r.top, 6), bottom = Math.min(r.bottom, vh - 6);
    spot.style.left = r.left - 6 + "px";
    spot.style.top = top - 6 + "px";
    spot.style.width = r.width + 12 + "px";
    spot.style.height = Math.max(0, bottom - top) + 12 + "px";
    shade(r.left - 6, top - 6, r.width + 12, Math.max(0, bottom - top) + 12);
    let x, y;
    const fits = { below: vh - bottom > ph + gap, above: top > ph + gap, right: vw - r.right > pw + gap, left: r.left > pw + gap };
    const order = [s.place, "below", "above", "right", "left"];
    const where = order.find((o) => fits[o]) || "inside";
    if (where === "below") { x = r.left; y = bottom + gap; }
    else if (where === "above") { x = r.left; y = top - ph - gap; }
    else if (where === "right") { x = r.right + gap; y = top; }
    else if (where === "left") { x = r.left - pw - gap; y = top; }
    else { x = vw - pw - 20; y = vh - ph - 20; }  // target fills the screen: corner
    pop.style.left = Math.min(Math.max(12, x), vw - pw - 12) + "px";
    pop.style.top = Math.min(Math.max(12, y), vh - ph - 12) + "px";
  }

  async function go(n) {
    const my = ++seq;                             // a newer click cancels this step
    i = Math.max(0, Math.min(STEPS.length - 1, n));
    const s = STEPS[i];
    $("#tour-step").textContent = `Step ${i + 1} of ${STEPS.length}`;
    $("#tour-title").textContent = s.title;
    $("#tour-body").innerHTML = s.body;
    $("#tour-back").disabled = i === 0;
    $("#tour-next").textContent = i === STEPS.length - 1 ? "Done" : "Next";
    $("#tour-dots").innerHTML = STEPS.map((_, k) => `<i class="${k === i ? "on" : ""}"></i>`).join("");
    if (s.before) await s.before();
    if (my !== seq || !active) return;
    const el = s.target && document.querySelector(s.target);
    if (el) el.scrollIntoView({ block: el.offsetHeight > window.innerHeight * 0.7 ? "start" : "center" });
    await sleep(60);
    if (my !== seq) return;
    place();
    $("#tour-next").focus();
  }

  function start() {
    active = true;
    tour.hidden = false;
    go(0);
  }
  function stop() {
    active = false;
    tour.hidden = true;
    try { localStorage.setItem("lockspy-tour-seen", "1"); } catch (e) { /* ignore */ }
  }

  $("#guide-btn").addEventListener("click", start);
  $("#intro-tour").addEventListener("click", start);
  $("#tour-next").addEventListener("click", () => (i === STEPS.length - 1 ? stop() : go(i + 1)));
  $("#tour-back").addEventListener("click", () => go(i - 1));
  $("#tour-skip").addEventListener("click", stop);
  document.addEventListener("keydown", (e) => {
    if (!active) return;
    if (e.key === "Escape") stop();
    else if (e.key === "ArrowRight") $("#tour-next").click();
    else if (e.key === "ArrowLeft" && i > 0) go(i - 1);
  });
  window.addEventListener("resize", place);
  window.addEventListener("scroll", place, { passive: true });
})();
