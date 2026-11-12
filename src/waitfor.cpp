// waitfor.cpp - the live wait-for graph and the watchdog thread.
// Each blocked thread has an edge to the lock it waits for (ThreadState::wait_lock), and each
// lock has edges to the threads that own it (LockInfo::holders). Following thread -> lock ->
// owner gives a thread -> thread graph; a cycle in it is a deadlock that is happening now.
// The watchdog wakes every LOCKSPY_WATCHDOG_MS, only considers threads blocked longer than
// that (short waits are normal contention), and reports each cycle once.
// OS concepts: wait-for graph, deadlock detection and recovery (report or abort), a helper
// thread created with the real pthread_create and excluded from all analysis.
#include "waitfor.h"
#include "config.h"
#include "lock_graph.h"
#include "recorder.h"
#include "report.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <functional>

namespace ls {

static std::atomic<bool> g_stop{false};

static void* watchdog_main(void*) {
    t_in_spy = true;  // the watchdog itself is never analysed
    while (!g_stop.load()) {
        timespec ts{cfg().watchdog_ms / 1000, (long)(cfg().watchdog_ms % 1000) * 1000000L};
        while (nanosleep(&ts, &ts) != 0) {}
        if (g_stop.load()) break;
        wf_check();
    }
    return nullptr;
}

void wf_start() {
    if (cfg().watchdog_ms <= 0) return;
    g_stop = false;
    pthread_t th;
    if (real.thread_create(&th, nullptr, watchdog_main, nullptr) == 0) pthread_detach(th);
}

void wf_stop() { g_stop = true; }

// Build a LIVE finding for the cycle c0 -> c1 -> ... (ci waits for a lock owned by ci+1).
static Finding make_live(const std::vector<int>& cyc, uint64_t now) {
    State& s = st();
    Finding f;
    f.kind = F_LIVE;
    f.t = secs(now);
    size_t n = cyc.size();
    std::vector<int> locks;
    for (size_t i = 0; i < n; i++) {
        ThreadState* t = s.threads[cyc[i]];
        ThreadState* prev = s.threads[cyc[(i + n - 1) % n]];
        Step st1;
        st1.thread = t->idx;
        st1.want = t->wait_lock;
        st1.want_mode = t->wait_mode;
        st1.held = prev->wait_lock;  // the lock the previous thread is waiting for: t owns it
        st1.owner = cyc[(i + 1) % n];
        st1.blocked_s = (double)(now - t->wait_since) / 1e9;
        st1.stack = t->wait_stack;
        for (const Held& h : t->held) if (h.lock == st1.held) { st1.pc = h.pc; st1.held_mode = h.mode; }
        f.steps.push_back(st1);
        locks.push_back(t->wait_lock);
    }
    std::sort(locks.begin(), locks.end());
    // Predict-then-confirm: was this exact set of locks reported earlier as a potential deadlock
    // (or, for a thread waiting on itself, as a relock)?
    for (const Finding& p : s.findings) {
        std::vector<int> pl;
        for (const Step& x : p.steps) pl.push_back(x.want);
        std::sort(pl.begin(), pl.end());
        bool match = (p.kind == F_POTENTIAL && pl == locks) ||
                     (p.kind == F_RELOCK && n == 1 && pl == locks && p.steps[0].thread == cyc[0]);
        if (match) { f.predicted_id = p.id; f.predicted_t = p.t; break; }
    }
    if (n > 1) lg_suggest(locks, f.order, f.violations);
    return f;
}

void wf_check() {
    std::vector<Finding> found;
    {
        Locked l(st().mu);
        State& s = st();
        uint64_t now = now_ns(), threshold = (uint64_t)cfg().watchdog_ms * 1000000ull;
        size_t n = s.threads.size();
        std::vector<std::vector<int>> waits_for(n);  // thread -> owner threads
        for (ThreadState* t : s.threads) {
            if (!t->alive || t->wait_lock < 0 || now - t->wait_since < threshold) continue;
            for (auto& h : s.locks[t->wait_lock]->holders)
                if (!(t->wait_mode == M_READ && h.second == M_READ)) waits_for[t->idx].push_back(h.first);
        }
        // DFS with colours: 1 = on the current path, 2 = done. A grey successor closes a cycle.
        std::vector<int> colour(n, 0), path;
        std::function<void(int)> dfs = [&](int u) {
            colour[u] = 1;
            path.push_back(u);
            for (int v : waits_for[u]) {
                if (colour[v] == 1) {
                    std::vector<int> cyc(std::find(path.begin(), path.end(), v), path.end());
                    std::vector<int> key = cyc;
                    std::sort(key.begin(), key.end());
                    if (s.live_seen.insert(key).second) found.push_back(make_live(cyc, now));
                } else if (colour[v] == 0) {
                    dfs(v);
                }
            }
            path.pop_back();
            colour[u] = 2;
        };
        for (size_t u = 0; u < n; u++) if (colour[u] == 0 && !waits_for[u].empty()) dfs((int)u);
    }
    for (Finding& f : found) report_emit(f);
    if (!found.empty() && cfg().abort_on_deadlock) {
        report_final();
        if (!cfg().quiet) fprintf(stderr, "[LockSpy] LOCKSPY_ON_DEADLOCK=abort: calling abort()\n");
        abort();
    }
}

}  // namespace ls
