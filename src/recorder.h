// recorder.h - LockSpy's data model and the event-recording API used by the interceptor.
// It holds every lock LockSpy has seen, every thread with its held-lock set, the lock-order
// edges and the findings. All of it is protected by one private lock (State::mu).
// OS concepts: threads (per-thread state, kernel thread IDs), mutual exclusion (our own lock
// must not be intercepted), thread-local storage, monotonic clocks.
#pragma once
#include <pthread.h>
#include <semaphore.h>
#include <sys/types.h>
#include <cstdint>
#include <ctime>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ls {

// Pointers to the real libc functions, filled by dlsym(RTLD_NEXT, ...) in interpose.cpp.
struct RealFns {
    int (*mutex_init)(pthread_mutex_t*, const pthread_mutexattr_t*);
    int (*mutex_destroy)(pthread_mutex_t*);
    int (*mutex_lock)(pthread_mutex_t*);
    int (*mutex_trylock)(pthread_mutex_t*);
    int (*mutex_timedlock)(pthread_mutex_t*, const struct timespec*);
    int (*mutex_unlock)(pthread_mutex_t*);
    int (*cond_wait)(pthread_cond_t*, pthread_mutex_t*);
    int (*cond_timedwait)(pthread_cond_t*, pthread_mutex_t*, const struct timespec*);
    int (*rw_init)(pthread_rwlock_t*, const pthread_rwlockattr_t*);
    int (*rw_destroy)(pthread_rwlock_t*);
    int (*rw_rdlock)(pthread_rwlock_t*);
    int (*rw_wrlock)(pthread_rwlock_t*);
    int (*rw_tryrdlock)(pthread_rwlock_t*);
    int (*rw_trywrlock)(pthread_rwlock_t*);
    int (*rw_timedrdlock)(pthread_rwlock_t*, const struct timespec*);
    int (*rw_timedwrlock)(pthread_rwlock_t*, const struct timespec*);
    int (*rw_unlock)(pthread_rwlock_t*);
    int (*sem_wait)(sem_t*);
    int (*sem_trywait)(sem_t*);
    int (*sem_timedwait)(sem_t*, const struct timespec*);
    int (*sem_post)(sem_t*);
    int (*thread_create)(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);
    void (*thread_exit)(void*);
};
extern RealFns real;
extern bool g_ready;  // true between our constructor and destructor

// Re-entrancy guard. While it is set, every wrapper calls the real function and does nothing
// else, so LockSpy's own locking and allocation never re-enter LockSpy. "initial-exec" TLS is
// a fixed offset from the thread pointer: no allocation and no function call on access.
extern __thread bool t_in_spy __attribute__((tls_model("initial-exec")));
struct Guard {
    bool entered;
    Guard() : entered(g_ready && !t_in_spy) { if (entered) t_in_spy = true; }
    ~Guard() { if (entered) t_in_spy = false; }
};

// A private mutex that calls the real pthread functions directly (never our wrappers).
class SpyLock {
    pthread_mutex_t m_ = PTHREAD_MUTEX_INITIALIZER;
public:
    void lock() { real.mutex_lock(&m_); }
    void unlock() { real.mutex_unlock(&m_); }
};
struct Locked {
    SpyLock& l;
    explicit Locked(SpyLock& x) : l(x) { l.lock(); }
    ~Locked() { l.unlock(); }
};

enum LockKind : uint8_t { K_MUTEX, K_RWLOCK };
enum Mode : uint8_t { M_WRITE, M_READ };  // mutexes are always M_WRITE (exclusive)
typedef std::vector<void*> Stack;          // raw return addresses, symbolized only when reporting

struct LockInfo {
    int id = 0;
    uintptr_t addr = 0;
    LockKind kind = K_MUTEX;
    bool recursive = false, dead = false;    // dead = destroyed (its node leaves the live graph)
    void* init_pc = nullptr;                 // where pthread_mutex_init was called (for naming)
    void* first_pc = nullptr;                // where it was first locked
    std::vector<std::pair<int, Mode>> holders;  // threads holding it now (lock -> owner edges)
    // contention profile (profiler.cpp)
    uint64_t acq = 0, contended = 0, wait_ns = 0, wait_max = 0, hold_ns = 0, hold_max = 0;
    std::string name;                        // filled lazily by symbolize.cpp
};

struct Held { int lock; Mode mode; int count; uint64_t t_acq; void* pc; };

struct ThreadState {
    int idx = 0;                 // 0 = main thread, then 1, 2, ... in order of first appearance
    pid_t tid = 0;               // kernel thread id (gettid), as in /proc/self/task/<tid>
    void* start_fn = nullptr;    // start routine passed to pthread_create
    bool alive = true, exit_checked = false;
    char comm[32] = {0};         // thread name from /proc/self/task/<tid>/comm
    std::vector<Held> held;      // the held-lock set
    int wait_lock = -1;          // wait-for edge: lock this thread is blocked on (-1 = none)
    Mode wait_mode = M_WRITE;
    uint64_t wait_since = 0;
    Stack wait_stack;
};

// One observation of an edge H->L: which thread, in which modes, holding which other locks.
struct Witness { int thread; Mode held_mode, want_mode; std::vector<int> gates; Stack stack; };

struct Edge {                    // lock-order edge from -> to: "to" was requested while "from" was held
    int from, to;
    uint64_t count = 0;
    double t_first = 0;
    std::vector<int> threads;    // threads that took this edge (for DOT labels)
    std::vector<Witness> wit;    // distinct observations (at most 8)
    bool in_cycle = false, suppressed = false;
};

enum FindingKind { F_POTENTIAL, F_LIVE, F_RELOCK, F_BAD_UNLOCK, F_HELD_AT_EXIT };
enum SuppressReason { R_THREAD, R_GATE, R_READ };

struct Step {                    // one thread's part in a finding
    int thread = -1, held = -1, want = -1, owner = -1;
    Mode held_mode = M_WRITE, want_mode = M_WRITE;
    double blocked_s = 0;
    void* pc = nullptr;          // acquisition site of "held"
    Stack stack;
};

struct Finding {
    int id = 0;
    FindingKind kind = F_POTENTIAL;
    double t = 0;                // seconds since program start
    std::vector<Step> steps;
    std::vector<int> order;      // suggested lock order (lock ids)
    std::vector<Step> violations;  // edges that break the suggested order
    int predicted_id = -1;       // LIVE: earlier finding that predicted this hang
    double predicted_t = 0;
    std::string text;            // formatted report
};

struct CondStat { uint64_t waits = 0, wait_ns = 0, wait_max = 0; };
struct SemStat { uint64_t waits = 0, contended = 0, posts = 0, wait_ns = 0, wait_max = 0; };

struct State {
    SpyLock mu;
    uint64_t t0 = 0;
    std::vector<LockInfo*> locks;                  // by lock id (never freed)
    std::unordered_map<uintptr_t, int> by_addr;    // live locks only
    std::vector<ThreadState*> threads;             // by thread idx (never freed)
    std::vector<Edge> edges;
    std::unordered_map<uint64_t, int> edge_of;     // (from,to) -> edge index
    std::vector<std::vector<int>> out;             // adjacency list: lock id -> edge indices
    std::set<std::vector<int>> reported, naive_seen;  // cycles as sorted edge-id lists
    std::map<std::vector<int>, SuppressReason> suppressed;
    std::unordered_set<uint64_t> misuse_seen;      // dedupe of misuse reports
    std::set<std::vector<int>> live_seen;          // dedupe of live deadlocks (sorted thread idx)
    std::unordered_map<void*, Stack> site_stacks;  // call site pc -> first stack seen there
    std::vector<Finding> findings;
    int seq = 0;
    std::unordered_map<uintptr_t, CondStat> conds;
    std::unordered_map<uintptr_t, SemStat> sems;
    uint64_t events = 0;
    bool is_child = false;
};
State& st();

uint64_t now_ns();
double secs(uint64_t t);   // ns timestamp -> seconds since start
Stack capture_stack();
LockInfo* lock_ptr(int id);      // take the lock briefly to read the table safely
ThreadState* thread_ptr(int idx);

// Work found while holding State::mu that must finish after releasing it.
struct Todo { std::vector<std::pair<int, int>> wit; void* site_pc = nullptr; };

// ---- API used by interpose.cpp ----
struct WaitCtx { ThreadState* t; LockInfo* L; Mode m; uint64_t t0; };
void rec_acquired(void* lock, LockKind k, Mode m, bool no_edges, void* pc, uint64_t wait_ns, bool contended);
WaitCtx rec_before_block(void* lock, LockKind k, Mode m, void* pc);
void rec_after_block(WaitCtx& w, int rc, void* pc);
void rec_release(void* lock, LockKind k, void* pc);
void rec_init(void* lock, LockKind k, bool recursive, void* pc);
void rec_destroy(void* lock);
int rec_cond_release(void* mutex);
void rec_cond_reacquire(void* mutex, void* cv, uint64_t waited, int count, void* pc);
void rec_sem_wait(void* sem, uint64_t waited, bool contended);
void rec_sem_post(void* sem);
void rec_thread_start(void* fn);
void rec_thread_exit();
void rec_init_state();
void rec_atfork_prepare();
void rec_atfork_parent();
void rec_atfork_child();

}  // namespace ls
