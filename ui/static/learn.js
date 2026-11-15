// learn.js: the interactive deadlock lesson. Each step describes the state of the scene
// (who holds which lock, who is waiting for what, which code line each thread is on) and the
// page animates from one state to the next.
"use strict";

const $ = (s) => document.querySelector(s);

// ------------------------------------------------------------------ the two programs
const CODE = {
  bad: {
    P: ["lock(A);", "lock(B);", "move money A → B", "unlock(B);", "unlock(A);"],
    Q: ["lock(B);", "lock(A);", "move money B → A", "unlock(A);", "unlock(B);"],
  },
  fixed: {
    P: ["lock(A);", "lock(B);", "move money A → B", "unlock(B);", "unlock(A);"],
    Q: ["lock(A);", "lock(B);", "move money B → A", "unlock(B);", "unlock(A);"],
  },
};

// small lock-order graph used in the "LockSpy sees" panel
function orderGraph(ab, ba, red) {
  const c = red ? "var(--red)" : "var(--muted)";
  return `<svg viewBox="0 0 260 120" class="og" aria-label="Lock-order graph">
    <defs><marker id="og-a" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto"><path d="M0 0L10 5L0 10z" fill="${c}"/></marker></defs>
    ${ab ? `<path d="M78 46 Q130 18 182 46" fill="none" stroke="${c}" stroke-width="2.4" marker-end="url(#og-a)"/><text x="130" y="22" class="og-l" fill="${c}">P: A → B</text>` : ""}
    ${ba ? `<path d="M182 74 Q130 102 78 74" fill="none" stroke="${c}" stroke-width="2.4" marker-end="url(#og-a)"/><text x="130" y="113" class="og-l" fill="${c}">Q: B → A</text>` : ""}
    <g class="og-n"><rect x="20" y="40" width="58" height="40" rx="9"/><text x="49" y="60">A</text></g>
    <g class="og-n"><rect x="182" y="40" width="58" height="40" rx="9"/><text x="211" y="60">B</text></g>
  </svg>`;
}
const term = (lines) => `<pre class="mini-term">${lines}</pre>`;

// ------------------------------------------------------------------ the lesson
// holds: owner of each lock, wants: lock each thread is waiting for,
// status: text under each thread, line: current code line (1-based, 0 = none, 6 = finished)
const PARTS = ["How a deadlock happens", "How LockSpy catches it", "The fix"];
const STEPS = [
  {
    part: 0, title: "Two threads, two locks", code: "bad",
    holds: {}, wants: {}, status: { P: "ready", Q: "ready" }, line: { P: 1, Q: 1 },
    text: `A bank moves money between accounts <b>A</b> and <b>B</b>. Each account has a <b>lock</b>, so only one
      thread changes it at a time. Thread <b class="cp-t">P</b> moves money A → B and thread <b class="cq-t">Q</b>
      moves money B → A. Look at their code on the right: P locks A first, Q locks B first.`,
    spy: `<h4>Why locks?</h4><p>Without a lock, two threads could change the same balance at once and money
      would disappear. A lock (mutex) makes the others wait until it's released.</p>`,
  },
  {
    part: 0, title: "P locks A", code: "bad",
    holds: { A: "P" }, wants: {}, status: { P: "holds A", Q: "ready" }, line: { P: 2, Q: 1 },
    text: `P runs <code>lock(A)</code>. Lock A was free, so P gets it right away. The solid arrow means
      "A is held by P".`,
    spy: `<h4>Rule</h4><p>Only one thread can hold a lock. Anyone else who asks for A now has to wait.</p>`,
  },
  {
    part: 0, title: "Q locks B, at the same moment", code: "bad",
    holds: { A: "P", B: "Q" }, wants: {}, status: { P: "holds A", Q: "holds B" }, line: { P: 2, Q: 2 },
    text: `Before P gets further, Q runs <code>lock(B)</code>. B was free too, so Q gets it. Each thread now
      holds one lock and needs the other.`,
    spy: `<h4>Timing</h4><p>This only happens if both threads reach line 1 at almost the same time. Usually
      one of them finishes first and everything is fine.</p>`,
  },
  {
    part: 0, title: "P asks for B and waits", code: "bad",
    holds: { A: "P", B: "Q" }, wants: { P: "B" }, status: { P: "waiting for B", Q: "holds B" }, line: { P: 2, Q: 2 },
    text: `P runs <code>lock(B)</code>, but Q has B. So P stops and waits (dashed arrow) until Q releases B.`,
    spy: `<h4>Still OK?</h4><p>So far, yes. Q will finish, release B, and P can continue... as long as Q
      doesn't need anything P is holding.</p>`,
  },
  {
    part: 0, title: "Q asks for A: deadlock", code: "bad", cycle: true,
    holds: { A: "P", B: "Q" }, wants: { P: "B", Q: "A" }, status: { P: "waiting for B", Q: "waiting for A" }, line: { P: 2, Q: 2 },
    text: `Q runs <code>lock(A)</code>, but P has A. Now P waits for Q and Q waits for P. Follow the arrows:
      P → B → Q → A → P, a <b class="red">circle</b>. Neither thread can ever continue. The program freezes.`,
    spy: `<h4>This is a deadlock</h4><p>No error, no crash: the program just stops doing anything, forever.</p>`,
  },
  {
    part: 0, title: "The four conditions", code: "bad", cycle: true,
    holds: { A: "P", B: "Q" }, wants: { P: "B", Q: "A" }, status: { P: "waiting for B", Q: "waiting for A" }, line: { P: 2, Q: 2 },
    text: `From the OS course: a deadlock happens only when all four of these are true at once. Here they are.`,
    spy: `<ul class="checks">
      <li><b>Mutual exclusion</b> only one thread can hold a lock</li>
      <li><b>Hold and wait</b> P holds A while waiting for B (Q too)</li>
      <li><b>No preemption</b> nobody can take A away from P</li>
      <li><b>Circular wait</b> P → B → Q → A → P</li></ul>
      <p>Break any one of them and the deadlock is impossible.</p>`,
  },
  {
    part: 1, title: "LockSpy sits in the middle", code: "bad",
    holds: {}, wants: {}, status: { P: "ready", Q: "ready" }, line: { P: 1, Q: 1 },
    text: `We run the program with LockSpy loaded (using <code>LD_PRELOAD</code>). Every <code>lock()</code> and
      <code>unlock()</code> call now goes through LockSpy first, which writes down who took which lock, then
      passes the call on. The program itself isn't changed.`,
    spy: `<div class="flow"><span>program</span><i>→</i><span class="hl">LockSpy</span><i>→</i><span>system library</span></div>
      <p>LockSpy keeps a list of the locks each thread holds right now.</p>`,
  },
  {
    part: 1, title: "A lucky run: P goes first", code: "bad",
    holds: {}, wants: {}, status: { P: "done", Q: "ready" }, line: { P: 6, Q: 1 },
    text: `This time the timing is lucky: P runs all its lines before Q starts. While P held A it asked for B,
      so LockSpy draws an arrow <b>A → B</b> ("someone took B while holding A").`,
    spy: `<h4>LockSpy's lock-order graph</h4>${orderGraph(true, false, false)}`,
  },
  {
    part: 1, title: "Q runs later, and LockSpy warns", code: "bad",
    holds: {}, wants: {}, status: { P: "done", Q: "done" }, line: { P: 6, Q: 6 },
    text: `Then Q runs, takes B, then A: arrow <b>B → A</b>. Nothing froze, the program finished fine. But the
      arrows now form a circle, so LockSpy knows a different timing <i>would</i> freeze, and warns you.`,
    spy: `<h4>LockSpy's lock-order graph</h4>${orderGraph(true, true, true)}
      ${term(`<b class="r">POTENTIAL DEADLOCK</b>: lock-order cycle
  Lock A -> Lock B  by thread P
  Lock B -> Lock A  by thread Q
  <b class="g">Suggested fix: acquire A before B
  in every thread.</b>`)}`,
  },
  {
    part: 1, title: "An unlucky run: the watchdog catches it", code: "bad", cycle: true,
    holds: { A: "P", B: "Q" }, wants: { P: "B", Q: "A" }, status: { P: "waiting for B", Q: "waiting for A" }, line: { P: 2, Q: 2 },
    text: `If the program does freeze, LockSpy's <b>watchdog</b> (a helper thread that checks every half
      second) sees P waiting for Q and Q waiting for P, a circle, and reports it, even saying it was predicted.`,
    spy: term(`<b class="r">LIVE DEADLOCK</b>: 2 threads blocked
  P holds A, waits for B (owner Q)
  Q holds B, waits for A (owner P)
  <b class="g">This hang was predicted at t=0.000s.</b>`),
  },
  {
    part: 2, title: "Fix: same order everywhere", code: "fixed",
    holds: { A: "P" }, wants: { Q: "A" }, status: { P: "holds A", Q: "waiting for A" }, line: { P: 2, Q: 1 },
    text: `The fix LockSpy suggested: every thread takes A before B. Q's code now starts with
      <code>lock(A)</code> too. P gets A first, so Q waits for A. But Q holds nothing, so nobody is waiting for Q.`,
    spy: `<h4>No circle possible</h4><p>Arrows only ever go A → B, never B → A, so they can't form a circle.
      This breaks the <b>circular wait</b> condition.</p>`,
  },
  {
    part: 2, title: "P finishes and lets go", code: "fixed",
    holds: { A: "Q" }, wants: {}, status: { P: "done", Q: "holds A" }, line: { P: 6, Q: 2 },
    text: `P takes B (free), moves the money, and unlocks both. Q was waiting for A, so it gets A now.`,
    spy: `<h4>Waiting is fine</h4><p>Q had to wait a little. That's normal <b>contention</b>, not a deadlock:
      the wait always ends.</p>`,
  },
  {
    part: 2, title: "Both threads finish", code: "fixed",
    holds: {}, wants: {}, status: { P: "done", Q: "done" }, line: { P: 6, Q: 6 },
    text: `Q takes B, moves its money, and unlocks. Both transfers are done, whatever the timing. Run this
      version under LockSpy and it reports nothing.`,
    spy: `<h4>LockSpy's lock-order graph</h4>${orderGraph(true, false, false)}
      ${term(`findings: 0 potential deadlock,
          0 live deadlock`)}
      <p class="done-links"><a class="btn primary" href="/">Try it in the dashboard →</a></p>`,
  },
];

// ------------------------------------------------------------------ drawing
const POS = { P: [80, 165], Q: [480, 165], A: [280, 62], B: [280, 268] };
const isThread = (n) => n === "P" || n === "Q";

function edgePoint(n, toward) {           // where a line from node n toward a point leaves n's shape
  const [x, y] = POS[n], dx = toward[0] - x, dy = toward[1] - y, len = Math.hypot(dx, dy);
  if (isThread(n)) return [x + (dx / len) * 42, y + (dy / len) * 42];
  const t = Math.min(66 / Math.abs(dx || 1e-9), 34 / Math.abs(dy || 1e-9));
  return [x + dx * t, y + dy * t];
}

function drawEdges(s) {
  const out = [];
  const line = (from, to, cls, marker, label) => {
    const a = edgePoint(from, POS[to]), b = edgePoint(to, POS[from]);
    const mx = (a[0] + b[0]) / 2, my = (a[1] + b[1]) / 2;
    out.push(`<line x1="${a[0].toFixed(1)}" y1="${a[1].toFixed(1)}" x2="${b[0].toFixed(1)}" y2="${b[1].toFixed(1)}" class="s-edge ${cls}" marker-end="url(#${marker})"/>`);
    const off = from === "P" || to === "P" ? -16 : 16;
    out.push(`<text class="s-elabel ${cls}" x="${(mx + off).toFixed(1)}" y="${(my - 6).toFixed(1)}">${label}</text>`);
  };
  for (const L of ["A", "B"]) {
    const t = s.holds[L];
    if (t) line(L, t, (s.cycle ? "red" : "own-" + t.toLowerCase()), s.cycle ? "m-red" : "m-" + t.toLowerCase(), "held by " + t);
  }
  for (const T of ["P", "Q"]) {
    const L = s.wants[T];
    if (L) line(T, L, "wait" + (s.cycle ? " red" : ""), s.cycle ? "m-red" : "m-grey", "wants " + L);
  }
  $("#edges").innerHTML = out.join("");
}

function drawScene(s) {
  for (const L of ["A", "B"]) {
    const g = $("#lock-" + L), owner = s.holds[L];
    g.classList.toggle("own-p", owner === "P");
    g.classList.toggle("own-q", owner === "Q");
    $("#lock-" + L + "-sub").textContent = owner ? "held by " + owner : "free";
  }
  for (const T of ["P", "Q"]) {
    const g = $("#th-" + T), st = s.status[T];
    g.classList.toggle("waiting", st.startsWith("waiting"));
    g.classList.toggle("done", st === "done");
    g.classList.toggle("stuck", !!s.cycle && st.startsWith("waiting"));
    $("#th-" + T + "-sub").textContent = st;
  }
  drawEdges(s);
  $("#banner").setAttribute("opacity", s.cycle && s.status.P.startsWith("waiting") ? "1" : "0");
  $("#stage").classList.toggle("is-cycle", !!s.cycle);
}

function drawCode(s) {
  for (const T of ["P", "Q"]) {
    const lines = CODE[s.code][T];
    const changed = s.code === "fixed" && T === "Q";
    $("#code-" + T).innerHTML = lines.map((l, k) => {
      const cur = s.line[T] === k + 1;
      const fixedLine = changed && k < 2;
      return `<li class="${cur ? "cur" + (s.status[T].startsWith("waiting") ? " blocked" : "") : ""}${fixedLine ? " fixed" : ""}">${l}</li>`;
    }).join("") + (s.line[T] === 6 ? `<li class="fin">✓ finished</li>` : "");
  }
}

// ------------------------------------------------------------------ navigation
let i = 0;
function show(n) {
  i = Math.max(0, Math.min(STEPS.length - 1, n));
  const s = STEPS[i];
  $("#l-part").textContent = `Part ${s.part + 1}: ${PARTS[s.part]}`;
  $("#l-title").textContent = s.title;
  $("#l-count").textContent = `Step ${i + 1} of ${STEPS.length}`;
  $("#caption").innerHTML = s.text;
  $("#spy").innerHTML = `<div class="spy-h">${s.part === 1 ? "What LockSpy sees" : s.part === 2 ? "Why it works" : "Good to know"}</div>${s.spy}`;
  drawScene(s);
  drawCode(s);
  $("#l-back").disabled = i === 0;
  $("#l-next").textContent = i === STEPS.length - 1 ? "Start again ↺" : "Next →";
  $("#l-dots").innerHTML = STEPS.map((x, k) =>
    `<button type="button" class="${k === i ? "on" : ""} p${x.part}" aria-label="Go to step ${k + 1}" data-k="${k}"></button>`).join("");
  $("#parts").innerHTML = PARTS.map((p, k) => {
    const first = STEPS.findIndex((x) => x.part === k);
    return `<button type="button" class="part${k === s.part ? " on" : ""}${k < s.part ? " done" : ""}" data-k="${first}"><span>${k + 1}</span>${p}</button>`;
  }).join("");
  try { history.replaceState(null, "", "#step-" + (i + 1)); } catch (e) { /* ignore */ }
}

$("#l-next").addEventListener("click", () => show(i === STEPS.length - 1 ? 0 : i + 1));
$("#l-back").addEventListener("click", () => show(i - 1));
document.addEventListener("click", (e) => {
  const b = e.target.closest("[data-k]");
  if (b) show(+b.dataset.k);
});
document.addEventListener("keydown", (e) => {
  if (e.target.closest("input, textarea")) return;
  if (e.key === "ArrowRight") show(i + 1);
  if (e.key === "ArrowLeft") show(i - 1);
});
function fromHash() {
  const m = /#step-(\d+)/.exec(location.hash);
  const n = m ? +m[1] - 1 : 0;
  if (n !== i || !$("#l-title").textContent) show(n);
}
window.addEventListener("hashchange", fromHash);
fromHash();
