// recorder.cpp - records every lock event: who (thread), what (lock), when (timestamp) and
// where (call site / stack). It keeps each thread's held-lock set, feeds the lock-order graph,
// registers wait-for edges, updates the profiler and detects misuse (relock, bad unlock,
// lock held at thread exit).
// OS concepts: threads and thread IDs (gettid, /proc/self/task/<tid>/comm), mutual exclusion
// (one private lock, short critical sections), fork() handling with pthread_atfork.
//
// Pattern used everywhere: do the bookkeeping while holding State::mu, but do anything slow
// (capturing a stack, symbolizing, printing) only after releasing it.
#include "recorder.h"
#include "config.h"
#include "lock_graph.h"
#include "profiler.h"
#include "report.h"
#include "symbolize.h"
#include "waitfor.h"
#include <execinfo.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace ls {

RealFns real;
bool g_ready = false;
__thread bool t_in_spy = false;
static __thread ThreadState* t_self __attribute__((tls_model("initial-exec"))) = nullptr;
// Allocated on first use and never freed. A plain global object would be built by a C++
// initializer that may run after our constructor, and destroyed before our destructor runs.
static State* g_state = nullptr;
State& st() {
    if (!g_state) g_state = new State();
    return *g_state;
}

uint64_t now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}
double secs(uint64_t t) { return (double)(t - st().t0) / 1e9; }

// Raw return addresses only (cheap). A few extra frames cover LockSpy's own frames, which
// the symbolizer drops later.
Stack capture_stack() {
    int n = cfg().stack_depth + 4;
    Stack s(n);
    n = backtrace(s.data(), n);
    s.resize(n > 0 ? n : 0);
    return s;
}

LockInfo* lock_ptr(int id) { Locked l(st().mu); return st().locks[id]; }
ThreadState* thread_ptr(int idx) { Locked l(st().mu); return st().threads[idx]; }

static void read_comm(ThreadState* t) {
    char path[64];
    snprintf(path, sizeof path, "/proc/self/task/%d/comm", (int)t->tid);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return;
    ssize_t n = read(fd, t->comm, sizeof t->comm - 1);
    close(fd);
    if (n > 0) { t->comm[n] = 0; if (char* nl = strchr(t->comm, '\n')) *nl = 0; }
}

// The calling thread's state, created on its first event. Caller holds State::mu.
static ThreadState* self_locked() {
    if (t_self) return t_self;
    ThreadState* t = new ThreadState();
    t->idx = (int)st().threads.size();
    t->tid = (pid_t)syscall(SYS_gettid);
    read_comm(t);
    st().threads.push_back(t);
    t_self = t;
    return t;
}

// Lock table lookup; a lock seen for the first time (e.g. PTHREAD_MUTEX_INITIALIZER) is added.
static LockInfo* lock_locked(void* addr, LockKind k, void* pc) {
    State& s = st();
    auto it = s.by_addr.find((uintptr_t)addr);
    if (it != s.by_addr.end()) return s.locks[it->second];
    LockInfo* L = new LockInfo();
    L->id = (int)s.locks.size();
    L->addr = (uintptr_t)addr;
    L->kind = k;
    L->first_pc = pc;
    // Statically initialised mutexes never pass through pthread_mutex_init, so read the type
    // glibc stores inside the mutex (PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP sets kind 1).
    if (k == K_MUTEX) L->recursive = (((pthread_mutex_t*)addr)->__data.__kind & 3) == PTHREAD_MUTEX_RECURSIVE;
    s.locks.push_back(L);
    s.by_addr[L->addr] = L->id;
    s.out.emplace_back();
    return L;
}

static Held* find_held(ThreadState* t, int lock) {
    for (Held& h : t->held) if (h.lock == lock) return &h;
    return nullptr;
}

static void trace(const char* what, ThreadState* t, LockInfo* L, void* pc) {
    st().events++;
    if (!cfg().trace) return;
    fprintf(stderr, "[LockSpy trace] t=%.6f tid=%d T%d %-8s lock=%#lx (#%d) at %p\n",
            secs(now_ns()), (int)t->tid, t->idx, what, (unsigned long)L->addr, L->id, pc);
}

// Add L to the held set (or bump the count for a recursive/read re-entry). Caller holds mu.
static void add_held(ThreadState* t, LockInfo* L, Mode m, void* pc, uint64_t wait, bool contended, Todo& todo) {
    if (Held* h = find_held(t, L->id)) {
        h->count++;
    } else {
        t->held.push_back(Held{L->id, m, 1, now_ns(), pc});
        L->holders.push_back({t->idx, m});
    }
    if (!st().site_stacks.count(pc)) todo.site_pc = pc;
    prof_acquired(L, wait, contended);
}

// Second half of the "bookkeeping now, slow work later" pattern: capture one stack, attach it
// to the new lock-order edges, run the DFS cycle check and print what it finds.
static void finish(Todo& todo, Stack* have = nullptr) {
    if (todo.wit.empty() && !todo.site_pc) return;
    Stack stack = have ? *have : capture_stack();
    std::vector<Finding> found;
    {
        Locked l(st().mu);
        for (auto& p : todo.wit) st().edges[p.first].wit[p.second].stack = stack;
        if (todo.site_pc && !st().site_stacks.count(todo.site_pc)) st().site_stacks[todo.site_pc] = stack;
        lg_check(todo, found);
    }
    for (Finding& f : found) report_emit(f);
}

static bool first_misuse(FindingKind k, int lock, void* pc) {
    uint64_t key = ((uint64_t)k << 60) ^ ((uint64_t)lock << 40) ^ (uint64_t)(uintptr_t)pc;
    return st().misuse_seen.insert(key).second;
}

// Fast path: the lock was obtained without blocking (a successful trylock).
void rec_acquired(void* lock, LockKind k, Mode m, bool no_edges, void* pc, uint64_t wait, bool contended) {
    Todo todo;
    {
        Locked l(st().mu);
        ThreadState* t = self_locked();
        LockInfo* L = lock_locked(lock, k, pc);
        trace(no_edges ? "TRYLOCK" : (m == M_READ ? "RDLOCK" : "LOCK"), t, L, pc);
        // trylock cannot block, so it cannot be part of a deadlock: no edge (Linux lockdep rule)
        if (!find_held(t, L->id) && (!no_edges || cfg().naive)) lg_note_edges(t, L, m, todo);
        add_held(t, L, m, pc, wait, contended, todo);
    }
    finish(todo);
}

// Slow path, before calling the real (blocking) lock: relock check, lock-order edges and DFS,
// then register the wait-for edge "this thread waits for L" for the watchdog.
WaitCtx rec_before_block(void* lock, LockKind k, Mode m, void* pc) {
    Todo todo;
    Finding relock;
    bool is_relock = false;
    ThreadState* t;
    LockInfo* L;
    {
        Locked l(st().mu);
        t = self_locked();
        L = lock_locked(lock, k, pc);
        trace("WAIT", t, L, pc);
        if (Held* h = find_held(t, L->id)) {
            bool read_read = (k == K_RWLOCK && m == M_READ && h->mode == M_READ);
            if (!read_read && !(k == K_MUTEX && L->recursive) && first_misuse(F_RELOCK, L->id, pc)) {
                is_relock = true;
                relock.kind = F_RELOCK;
                relock.t = secs(now_ns());
                Step s; s.thread = t->idx; s.held = s.want = L->id; s.pc = h->pc; s.want_mode = m; s.held_mode = h->mode;
                relock.steps.push_back(s);
            }
        } else {
            lg_note_edges(t, L, m, todo);
        }
    }
    Stack stack = capture_stack();
    finish(todo, &stack);
    if (is_relock) { relock.steps[0].stack = stack; report_emit(relock); }
    uint64_t t0 = now_ns();
    {
        Locked l(st().mu);
        t->wait_lock = L->id;
        t->wait_mode = m;
        t->wait_since = t0;
        t->wait_stack = stack;
    }
    return WaitCtx{t, L, m, t0};
}

// Slow path, after the real lock returned: remove the wait-for edge, record the acquisition.
void rec_after_block(WaitCtx& w, int rc, void* pc) {
    uint64_t waited = now_ns() - w.t0;
    Todo todo;
    {
        Locked l(st().mu);
        w.t->wait_lock = -1;
        if (rc == 0) {
            trace("ACQUIRED", w.t, w.L, pc);
            add_held(w.t, w.L, w.m, pc, waited, true, todo);
            if (todo.site_pc) st().site_stacks[todo.site_pc] = w.t->wait_stack;
        }
    }
}

void rec_release(void* lock, LockKind k, void* pc) {
    Finding bad;
    bool is_bad = false;
    {
        Locked l(st().mu);
        ThreadState* t = self_locked();
        LockInfo* L = lock_locked(lock, k, pc);
        trace("UNLOCK", t, L, pc);
        Held* h = find_held(t, L->id);
        if (!h) {
            if (first_misuse(F_BAD_UNLOCK, L->id, pc)) {
                is_bad = true;
                bad.kind = F_BAD_UNLOCK;
                bad.t = secs(now_ns());
                Step s; s.thread = t->idx; s.want = L->id;
                s.owner = L->holders.empty() ? -1 : L->holders[0].first;
                bad.steps.push_back(s);
            }
        } else if (--h->count == 0) {
            prof_released(L, now_ns() - h->t_acq);
            t->held.erase(t->held.begin() + (h - t->held.data()));
            for (size_t i = 0; i < L->holders.size(); i++)
                if (L->holders[i].first == t->idx) { L->holders.erase(L->holders.begin() + i); break; }
        }
    }
    if (is_bad) { bad.steps[0].stack = capture_stack(); report_emit(bad); }
}

void rec_init(void* lock, LockKind k, bool recursive, void* pc) {
    Locked l(st().mu);
    auto it = st().by_addr.find((uintptr_t)lock);
    if (it != st().by_addr.end()) { st().locks[it->second]->dead = true; st().by_addr.erase(it); }
    LockInfo* L = lock_locked(lock, k, pc);
    L->init_pc = pc;
    L->first_pc = nullptr;
    L->recursive = recursive;
}

// The node stays in the history (for reports) but leaves the live graph, so a new lock that
// reuses this address gets a fresh node and is not confused with the old one.
void rec_destroy(void* lock) {
    Locked l(st().mu);
    auto it = st().by_addr.find((uintptr_t)lock);
    if (it == st().by_addr.end()) return;
    st().locks[it->second]->dead = true;
    st().by_addr.erase(it);
}

// pthread_cond_wait releases the mutex while sleeping and re-acquires it before returning.
// Mirror that in the held set, or the sleeping thread would look like the mutex owner.
int rec_cond_release(void* mutex) {
    Locked l(st().mu);
    ThreadState* t = self_locked();
    auto it = st().by_addr.find((uintptr_t)mutex);
    if (it == st().by_addr.end()) return 0;
    LockInfo* L = st().locks[it->second];
    Held* h = find_held(t, L->id);
    if (!h) return 0;
    int count = h->count;
    prof_released(L, now_ns() - h->t_acq);
    t->held.erase(t->held.begin() + (h - t->held.data()));
    for (size_t i = 0; i < L->holders.size(); i++)
        if (L->holders[i].first == t->idx) { L->holders.erase(L->holders.begin() + i); break; }
    return count;
}

void rec_cond_reacquire(void* mutex, void* cv, uint64_t waited, int count, void* pc) {
    Locked l(st().mu);
    prof_cond(cv, waited);
    if (count == 0) return;
    ThreadState* t = self_locked();
    LockInfo* L = lock_locked(mutex, K_MUTEX, pc);
    t->held.push_back(Held{L->id, M_WRITE, count, now_ns(), pc});
    L->holders.push_back({t->idx, M_WRITE});
}

void rec_sem_wait(void* sem, uint64_t waited, bool contended) { Locked l(st().mu); prof_sem_wait(sem, waited, contended); }
void rec_sem_post(void* sem) { Locked l(st().mu); prof_sem_post(sem); }

void rec_thread_start(void* fn) {
    Locked l(st().mu);
    self_locked()->start_fn = fn;
}

// Called when a thread returns from its start routine or calls pthread_exit.
void rec_thread_exit() {
    std::vector<Finding> found;
    {
        Locked l(st().mu);
        ThreadState* t = self_locked();
        if (t->exit_checked) return;
        t->exit_checked = true;
        read_comm(t);
        for (Held& h : t->held) {
            if (!first_misuse(F_HELD_AT_EXIT, h.lock, h.pc)) continue;
            Finding f;
            f.kind = F_HELD_AT_EXIT;
            f.t = secs(now_ns());
            Step s; s.thread = t->idx; s.held = h.lock; s.pc = h.pc; s.held_mode = h.mode;
            auto it = st().site_stacks.find(h.pc);
            if (it != st().site_stacks.end()) s.stack = it->second;
            f.steps.push_back(s);
            found.push_back(f);
        }
        t->alive = false;
    }
    for (Finding& f : found) report_emit(f);
}

void rec_init_state() {
    st().t0 = now_ns();
    Locked l(st().mu);
    self_locked();  // the thread running the constructor is the main thread: idx 0
}

// fork(): only the calling thread exists in the child. Hold our lock across fork so the child
// never inherits it in a half-updated state, then start the child with a fresh analysis.
void rec_atfork_prepare() { sym_lock(); st().mu.lock(); }
void rec_atfork_parent() { st().mu.unlock(); sym_unlock(); }
void rec_atfork_child() {
    State& s = st();
    ThreadState* me = t_self;
    for (ThreadState* t : s.threads) if (t != me) { t->alive = false; t->wait_lock = -1; t->held.clear(); }
    for (LockInfo* L : s.locks) {
        L->holders.clear();
        L->acq = L->contended = L->wait_ns = L->wait_max = L->hold_ns = L->hold_max = 0;
    }
    if (me) for (Held& h : me->held) s.locks[h.lock]->holders.push_back({me->idx, h.mode});
    s.edges.clear(); s.edge_of.clear();
    for (auto& o : s.out) o.clear();
    s.reported.clear(); s.naive_seen.clear(); s.suppressed.clear();
    s.misuse_seen.clear(); s.live_seen.clear(); s.findings.clear();
    s.conds.clear(); s.sems.clear();
    s.is_child = true;
    s.mu.unlock();
    sym_unlock();
    wf_start();
}

}  // namespace ls
