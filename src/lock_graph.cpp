// lock_graph.cpp - the lock-order graph: edge H->L means "some thread requested L while
// holding H". A cycle means the locks are taken in inconsistent orders, so some interleaving
// can deadlock, even if this run never hung (the idea behind Linux lockdep).
//
//  * New edge H->L: DFS from L looking for a path back to H (L can already reach H => cycle).
//  * Filter (fewer false positives): a cycle is only reported if its edges were taken by at
//    least two different threads, no two of those threads held a common third "gate" lock
//    (the gate serialises them, so they can never be inside at the same time), and no step
//    needs a reader to wait for a reader. Otherwise the cycle is counted as "suppressed (safe)".
//  * Fix: Tarjan's algorithm splits the graph into strongly connected components; inside each,
//    a greedy ordering that removes few back-edges gives a suggested global lock order, and
//    the back-edges are the call sites that violate it.
// OS concepts: deadlock detection on a resource graph, circular wait, prevention by ordering.
#include "lock_graph.h"
#include "config.h"
#include <algorithm>
#include <functional>

namespace ls {

static uint64_t edge_key(int a, int b) { return ((uint64_t)(uint32_t)a << 32) | (uint32_t)b; }

// Called with State::mu held, before (or right after) thread t acquires L.
void lg_note_edges(ThreadState* t, LockInfo* L, Mode want, Todo& todo) {
    State& s = st();
    for (size_t i = 0; i < t->held.size(); i++) {
        int h = t->held[i].lock;
        Mode hm = t->held[i].mode;
        if (h == L->id) continue;
        std::vector<int> gates;  // the other locks held at this moment
        for (const Held& o : t->held) if (o.lock != h) gates.push_back(o.lock);
        std::sort(gates.begin(), gates.end());

        int e;
        auto it = s.edge_of.find(edge_key(h, L->id));
        if (it == s.edge_of.end()) {
            e = (int)s.edges.size();
            Edge E;
            E.from = h; E.to = L->id; E.t_first = secs(now_ns());
            s.edges.push_back(E);
            s.edge_of[edge_key(h, L->id)] = e;
            s.out[h].push_back(e);
        } else {
            e = it->second;
        }
        Edge& E = s.edges[e];
        E.count++;
        if (std::find(E.threads.begin(), E.threads.end(), t->idx) == E.threads.end()) E.threads.push_back(t->idx);
        bool known = false;
        for (const Witness& w : E.wit)
            if (w.thread == t->idx && w.held_mode == hm && w.want_mode == want && w.gates == gates) known = true;
        // A new edge, or a new way of taking an old edge, may complete a cycle: check it later.
        if (!known && E.wit.size() < 8) {
            E.wit.push_back(Witness{t->idx, hm, want, gates, Stack()});
            todo.wit.push_back({e, (int)E.wit.size() - 1});
        }
    }
}

// All simple paths from node to target (bounded), as lists of edge indices.
static void find_paths(int node, int target, std::vector<int>& path, std::vector<char>& on_path,
                       std::vector<std::vector<int>>& out, int& budget) {
    if (--budget < 0 || out.size() >= 32) return;
    if (node == target) { out.push_back(path); return; }
    if (path.size() >= 16) return;
    on_path[node] = 1;
    for (int e : st().out[node]) {
        int next = st().edges[e].to;
        if (on_path[next] || st().locks[next]->dead) continue;
        path.push_back(e);
        find_paths(next, target, path, on_path, out, budget);
        path.pop_back();
    }
    on_path[node] = 0;
}

static bool share_lock(const std::vector<int>& a, const std::vector<int>& b) {
    for (int x : a) if (std::binary_search(b.begin(), b.end(), x)) return true;
    return false;
}

// Choose one witness per edge of the cycle so that the cycle could really happen.
static bool choose(const std::vector<int>& cyc, size_t i, std::vector<const Witness*>& sel,
                   bool use_gates, bool use_read, int& budget) {
    if (--budget < 0) return false;
    if (i == cyc.size()) {
        bool two_threads = false;
        for (const Witness* w : sel) if (w->thread != sel[0]->thread) two_threads = true;
        if (!two_threads) return false;
        if (use_read)
            for (size_t k = 0; k < sel.size(); k++)  // a reader never waits for another reader
                if (sel[k]->want_mode == M_READ && sel[(k + 1) % sel.size()]->held_mode == M_READ) return false;
        return true;
    }
    for (const Witness& w : st().edges[cyc[i]].wit) {
        bool clash = false;
        if (use_gates) for (const Witness* p : sel) if (share_lock(p->gates, w.gates)) clash = true;
        if (clash) continue;
        sel.push_back(&w);
        if (choose(cyc, i + 1, sel, use_gates, use_read, budget)) return true;
        sel.pop_back();
    }
    return false;
}

static void check_edge(int e, std::vector<Finding>& found) {
    State& s = st();
    const Edge E = s.edges[e];
    std::vector<int> path;
    std::vector<char> on_path(s.locks.size(), 0);
    std::vector<std::vector<int>> paths;
    int budget = 2000;
    find_paths(E.to, E.from, path, on_path, paths, budget);
    for (std::vector<int>& p : paths) {
        p.push_back(e);  // the cycle: L -> ... -> H -> L
        std::vector<int> key = p;
        std::sort(key.begin(), key.end());
        s.naive_seen.insert(key);
        if (s.reported.count(key)) continue;

        std::vector<const Witness*> sel;
        int b = 20000;
        bool ok;
        if (cfg().naive) {
            for (int x : p) sel.push_back(&s.edges[x].wit[0]);
            ok = true;
        } else {
            ok = choose(p, 0, sel, true, true, b);
        }
        if (!ok) {
            std::vector<const Witness*> tmp;
            int b1 = 20000, b2 = 20000;
            SuppressReason r;
            if (!choose(p, 0, tmp, false, false, b1)) {
                r = R_THREAD;  // every edge came from the same thread
            } else {
                tmp.clear();
                r = !choose(p, 0, tmp, true, false, b2) ? R_GATE : R_READ;
            }
            s.suppressed[key] = r;
            for (int x : p) s.edges[x].suppressed = true;
            continue;
        }
        s.suppressed.erase(key);
        s.reported.insert(key);
        Finding f;
        f.kind = F_POTENTIAL;
        f.t = secs(now_ns());
        std::vector<int> nodes;
        for (size_t k = 0; k < p.size(); k++) {
            Edge& X = s.edges[p[k]];
            X.in_cycle = true;
            Step st1;
            st1.thread = sel[k]->thread; st1.held = X.from; st1.want = X.to;
            st1.held_mode = sel[k]->held_mode; st1.want_mode = sel[k]->want_mode;
            st1.stack = sel[k]->stack;
            f.steps.push_back(st1);
            nodes.push_back(X.from);
        }
        lg_suggest(nodes, f.order, f.violations);
        // Print the cycle starting from the first lock of the suggested order.
        for (size_t k = 0; k < f.steps.size(); k++)
            if (!f.order.empty() && f.steps[k].held == f.order[0]) {
                std::rotate(f.steps.begin(), f.steps.begin() + k, f.steps.end());
                break;
            }
        found.push_back(f);
        return;  // one report per new edge is enough
    }
}

void lg_check(const Todo& todo, std::vector<Finding>& found) {
    for (auto& p : todo.wit) check_edge(p.first, found);
}

// Greedy ordering of "nodes" (a heuristic for the minimum feedback arc set): repeatedly take a
// lock with no remaining incoming edge, or else the one with the most (outgoing - incoming).
// Ties go to the lock created first. Edges that point backwards in the result are violations.
void lg_suggest(const std::vector<int>& nodes, std::vector<int>& order, std::vector<Step>& violations) {
    State& s = st();
    std::vector<int> left = nodes;
    std::sort(left.begin(), left.end());
    left.erase(std::unique(left.begin(), left.end()), left.end());
    auto inside = [&](int x) { return std::binary_search(left.begin(), left.end(), x); };
    std::vector<int> es;
    for (int u : left)
        for (int e : s.out[u])
            if (inside(s.edges[e].to) && (s.edges[e].in_cycle || !s.edges[e].suppressed)) es.push_back(e);
    order.clear();
    while (!left.empty()) {
        int best = -1, best_score = 0;
        for (int u : left) {
            int in = 0, out = 0;
            for (int e : es) {
                bool live = std::binary_search(left.begin(), left.end(), s.edges[e].from) &&
                            std::binary_search(left.begin(), left.end(), s.edges[e].to);
                if (!live) continue;
                if (s.edges[e].to == u) in++;
                if (s.edges[e].from == u) out++;
            }
            int score = (in == 0 ? 1000000 : 0) + out - in;
            if (best < 0 || score > best_score) { best = u; best_score = score; }
        }
        order.push_back(best);
        left.erase(std::find(left.begin(), left.end(), best));
    }
    std::vector<int> pos(s.locks.size(), 0);
    for (size_t i = 0; i < order.size(); i++) pos[order[i]] = (int)i;
    violations.clear();
    for (int e : es) {
        const Edge& E = s.edges[e];
        if (pos[E.from] <= pos[E.to]) continue;
        Step v;
        v.held = E.from; v.want = E.to;
        if (!E.wit.empty()) { v.thread = E.wit[0].thread; v.stack = E.wit[0].stack; }
        violations.push_back(v);
    }
}

// Tarjan's strongly connected components over the edges of reported cycles.
std::vector<std::vector<int>> lg_components() {
    State& s = st();
    int n = (int)s.locks.size(), counter = 0;
    std::vector<int> index(n, -1), low(n, 0), stack;
    std::vector<char> on_stack(n, 0);
    std::vector<std::vector<int>> comps;
    std::function<void(int)> visit = [&](int u) {
        index[u] = low[u] = counter++;
        stack.push_back(u);
        on_stack[u] = 1;
        for (int e : s.out[u]) {
            if (!s.edges[e].in_cycle) continue;
            int v = s.edges[e].to;
            if (index[v] < 0) { visit(v); low[u] = std::min(low[u], low[v]); }
            else if (on_stack[v]) low[u] = std::min(low[u], index[v]);
        }
        if (low[u] == index[u]) {
            std::vector<int> c;
            int v;
            do { v = stack.back(); stack.pop_back(); on_stack[v] = 0; c.push_back(v); } while (v != u);
            if (c.size() > 1) { std::sort(c.begin(), c.end()); comps.push_back(c); }
        }
    };
    for (int u = 0; u < n; u++) if (index[u] < 0) visit(u);
    return comps;
}

}  // namespace ls
