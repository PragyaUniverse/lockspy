// interpose.cpp - the LD_PRELOAD shim. It defines pthread_mutex_lock & co. with the same names
// as libc. The dynamic linker searches LD_PRELOAD libraries first, so the program's calls land
// here; each wrapper records the event and forwards to the real function, which it finds with
// dlsym(RTLD_NEXT, name): "the next definition after this library", i.e. libc's.
// OS concepts: dynamic linking and symbol interposition, LD_PRELOAD, library constructors and
// destructors, thread creation, re-entrancy, fork handlers.
//
// The lock wrappers use a fast path: try the lock first. If it is free we never block, so no
// wait-for edge is needed and the cost is small. Only when it is busy do we record the wait.
#include "config.h"
#include "recorder.h"
#include "report.h"
#include "symbolize.h"
#include "waitfor.h"
#include <dlfcn.h>
#include <errno.h>
#include <execinfo.h>
#include <cstdlib>

using namespace ls;

static void resolve_all() {
    static bool resolving = false;  // dlsym itself must not recurse into us while we resolve
    if (resolving) return;
    resolving = true;
#define R(field, name) real.field = (decltype(real.field))dlsym(RTLD_NEXT, name)
    R(mutex_init, "pthread_mutex_init");
    R(mutex_destroy, "pthread_mutex_destroy");
    R(mutex_lock, "pthread_mutex_lock");
    R(mutex_trylock, "pthread_mutex_trylock");
    R(mutex_timedlock, "pthread_mutex_timedlock");
    R(mutex_unlock, "pthread_mutex_unlock");
    R(rw_init, "pthread_rwlock_init");
    R(rw_destroy, "pthread_rwlock_destroy");
    R(rw_rdlock, "pthread_rwlock_rdlock");
    R(rw_wrlock, "pthread_rwlock_wrlock");
    R(rw_tryrdlock, "pthread_rwlock_tryrdlock");
    R(rw_trywrlock, "pthread_rwlock_trywrlock");
    R(rw_timedrdlock, "pthread_rwlock_timedrdlock");
    R(rw_timedwrlock, "pthread_rwlock_timedwrlock");
    R(rw_unlock, "pthread_rwlock_unlock");
    R(sem_wait, "sem_wait");
    R(sem_trywait, "sem_trywait");
    R(sem_timedwait, "sem_timedwait");
    R(sem_post, "sem_post");
    R(thread_create, "pthread_create");
    R(thread_exit, "pthread_exit");
    // On x86-64 an unversioned dlsym("pthread_cond_wait") returns the OLD GLIBC_2.2.5 condvar
    // implementation, which is incompatible with condvars made by current code and hangs.
    // Ask for the current version explicitly; fall back for platforms with a single version.
    real.cond_wait = (decltype(real.cond_wait))dlvsym(RTLD_NEXT, "pthread_cond_wait", "GLIBC_2.3.2");
    real.cond_timedwait = (decltype(real.cond_timedwait))dlvsym(RTLD_NEXT, "pthread_cond_timedwait", "GLIBC_2.3.2");
    if (!real.cond_wait) R(cond_wait, "pthread_cond_wait");
    if (!real.cond_timedwait) R(cond_timedwait, "pthread_cond_timedwait");
#undef R
    resolving = false;
}

// Before our constructor has run (another library's constructor may lock first), resolve on
// demand. If even that is impossible (we are inside dlsym), report success: only one thread
// exists that early, so nothing can be contended.
#define PASS(fn, ...)                              \
    do {                                           \
        if (!real.fn) resolve_all();               \
        return real.fn ? real.fn(__VA_ARGS__) : 0; \
    } while (0)

#define CALLER __builtin_return_address(0)

// ---------------- mutexes ----------------

extern "C" int pthread_mutex_lock(pthread_mutex_t* m) {
    Guard g;
    if (!g.entered) PASS(mutex_lock, m);
    void* pc = CALLER;
    int rc = real.mutex_trylock(m);
    if (rc == 0) { rec_acquired(m, K_MUTEX, M_WRITE, false, pc, 0, false); return 0; }
    if (rc != EBUSY) return rc;
    WaitCtx w = rec_before_block(m, K_MUTEX, M_WRITE, pc);  // relock check, edges, wait-for edge
    rc = real.mutex_lock(m);
    rec_after_block(w, rc, pc);                             // acquired: held set + statistics
    return rc;
}

extern "C" int pthread_mutex_trylock(pthread_mutex_t* m) {
    Guard g;
    if (!g.entered) PASS(mutex_trylock, m);
    int rc = real.mutex_trylock(m);
    if (rc == 0) rec_acquired(m, K_MUTEX, M_WRITE, true, CALLER, 0, false);
    return rc;
}

// A timed lock gives up after a timeout, so it can never block forever: treat it like trylock
// (held set and wait time, but no lock-order edge and no wait-for edge).
extern "C" int pthread_mutex_timedlock(pthread_mutex_t* m, const struct timespec* abs) {
    Guard g;
    if (!g.entered) PASS(mutex_timedlock, m, abs);
    uint64_t t0 = now_ns();
    int rc = real.mutex_trylock(m);
    bool contended = rc == EBUSY;
    if (contended) rc = real.mutex_timedlock(m, abs);
    if (rc == 0) rec_acquired(m, K_MUTEX, M_WRITE, true, CALLER, now_ns() - t0, contended);
    return rc;
}

extern "C" int pthread_mutex_unlock(pthread_mutex_t* m) {
    Guard g;
    if (!g.entered) PASS(mutex_unlock, m);
    rec_release(m, K_MUTEX, CALLER);  // bad-unlock check, hold time; then really unlock
    return real.mutex_unlock(m);
}

extern "C" int pthread_mutex_init(pthread_mutex_t* m, const pthread_mutexattr_t* a) {
    Guard g;
    if (!g.entered) PASS(mutex_init, m, a);
    int rc = real.mutex_init(m, a);
    int type = PTHREAD_MUTEX_DEFAULT;
    if (a) pthread_mutexattr_gettype(a, &type);
    if (rc == 0) rec_init(m, K_MUTEX, type == PTHREAD_MUTEX_RECURSIVE, CALLER);
    return rc;
}

extern "C" int pthread_mutex_destroy(pthread_mutex_t* m) {
    Guard g;
    if (!g.entered) PASS(mutex_destroy, m);
    rec_destroy(m);
    return real.mutex_destroy(m);
}

// ---------------- condition variables ----------------
// cond_wait atomically releases the mutex, sleeps, and re-acquires it. Mirror that in the
// held set, and count the sleep as condition-variable time, not lock wait time.

extern "C" int pthread_cond_wait(pthread_cond_t* c, pthread_mutex_t* m) {
    Guard g;
    if (!g.entered) PASS(cond_wait, c, m);
    int count = rec_cond_release(m);
    uint64_t t0 = now_ns();
    int rc = real.cond_wait(c, m);
    rec_cond_reacquire(m, c, now_ns() - t0, count, CALLER);
    return rc;
}

extern "C" int pthread_cond_timedwait(pthread_cond_t* c, pthread_mutex_t* m, const struct timespec* abs) {
    Guard g;
    if (!g.entered) PASS(cond_timedwait, c, m, abs);
    int count = rec_cond_release(m);
    uint64_t t0 = now_ns();
    int rc = real.cond_timedwait(c, m, abs);
    rec_cond_reacquire(m, c, now_ns() - t0, count, CALLER);
    return rc;
}

// ---------------- reader-writer locks ----------------

static int rw_lock(pthread_rwlock_t* l, Mode m, void* pc) {
    int rc = m == M_READ ? real.rw_tryrdlock(l) : real.rw_trywrlock(l);
    if (rc == 0) { rec_acquired(l, K_RWLOCK, m, false, pc, 0, false); return 0; }
    if (rc != EBUSY) return rc;
    WaitCtx w = rec_before_block(l, K_RWLOCK, m, pc);
    rc = m == M_READ ? real.rw_rdlock(l) : real.rw_wrlock(l);
    rec_after_block(w, rc, pc);
    return rc;
}

extern "C" int pthread_rwlock_rdlock(pthread_rwlock_t* l) {
    Guard g;
    if (!g.entered) PASS(rw_rdlock, l);
    return rw_lock(l, M_READ, CALLER);
}

extern "C" int pthread_rwlock_wrlock(pthread_rwlock_t* l) {
    Guard g;
    if (!g.entered) PASS(rw_wrlock, l);
    return rw_lock(l, M_WRITE, CALLER);
}

extern "C" int pthread_rwlock_tryrdlock(pthread_rwlock_t* l) {
    Guard g;
    if (!g.entered) PASS(rw_tryrdlock, l);
    int rc = real.rw_tryrdlock(l);
    if (rc == 0) rec_acquired(l, K_RWLOCK, M_READ, true, CALLER, 0, false);
    return rc;
}

extern "C" int pthread_rwlock_trywrlock(pthread_rwlock_t* l) {
    Guard g;
    if (!g.entered) PASS(rw_trywrlock, l);
    int rc = real.rw_trywrlock(l);
    if (rc == 0) rec_acquired(l, K_RWLOCK, M_WRITE, true, CALLER, 0, false);
    return rc;
}

extern "C" int pthread_rwlock_timedrdlock(pthread_rwlock_t* l, const struct timespec* abs) {
    Guard g;
    if (!g.entered) PASS(rw_timedrdlock, l, abs);
    uint64_t t0 = now_ns();
    int rc = real.rw_timedrdlock(l, abs);
    if (rc == 0) rec_acquired(l, K_RWLOCK, M_READ, true, CALLER, now_ns() - t0, false);
    return rc;
}

extern "C" int pthread_rwlock_timedwrlock(pthread_rwlock_t* l, const struct timespec* abs) {
    Guard g;
    if (!g.entered) PASS(rw_timedwrlock, l, abs);
    uint64_t t0 = now_ns();
    int rc = real.rw_timedwrlock(l, abs);
    if (rc == 0) rec_acquired(l, K_RWLOCK, M_WRITE, true, CALLER, now_ns() - t0, false);
    return rc;
}

extern "C" int pthread_rwlock_unlock(pthread_rwlock_t* l) {
    Guard g;
    if (!g.entered) PASS(rw_unlock, l);
    rec_release(l, K_RWLOCK, CALLER);
    return real.rw_unlock(l);
}

extern "C" int pthread_rwlock_init(pthread_rwlock_t* l, const pthread_rwlockattr_t* a) {
    Guard g;
    if (!g.entered) PASS(rw_init, l, a);
    int rc = real.rw_init(l, a);
    if (rc == 0) rec_init(l, K_RWLOCK, false, CALLER);
    return rc;
}

extern "C" int pthread_rwlock_destroy(pthread_rwlock_t* l) {
    Guard g;
    if (!g.entered) PASS(rw_destroy, l);
    rec_destroy(l);
    return real.rw_destroy(l);
}

// ---------------- semaphores (profiling only: a semaphore has no owner) ----------------

extern "C" int sem_wait(sem_t* s) {
    Guard g;
    if (!g.entered) PASS(sem_wait, s);
    uint64_t t0 = now_ns();
    int rc = real.sem_trywait(s);
    bool contended = rc != 0 && errno == EAGAIN;
    if (contended) rc = real.sem_wait(s);
    int saved = errno;
    if (rc == 0) rec_sem_wait(s, now_ns() - t0, contended);
    errno = saved;
    return rc;
}

extern "C" int sem_trywait(sem_t* s) {
    Guard g;
    if (!g.entered) PASS(sem_trywait, s);
    int rc = real.sem_trywait(s);
    int saved = errno;
    if (rc == 0) rec_sem_wait(s, 0, false);
    errno = saved;
    return rc;
}

extern "C" int sem_timedwait(sem_t* s, const struct timespec* abs) {
    Guard g;
    if (!g.entered) PASS(sem_timedwait, s, abs);
    uint64_t t0 = now_ns();
    int rc = real.sem_timedwait(s, abs);
    int saved = errno;
    if (rc == 0) rec_sem_wait(s, now_ns() - t0, now_ns() - t0 > 1000);
    errno = saved;
    return rc;
}

extern "C" int sem_post(sem_t* s) {
    Guard g;
    if (!g.entered) PASS(sem_post, s);
    rec_sem_post(s);
    return real.sem_post(s);
}

// ---------------- threads ----------------
// Wrap the start routine so we know each thread's entry function, and can check its held-lock
// set when it returns ("lock held at thread exit").

struct Trampoline { void* (*fn)(void*); void* arg; };

static void* trampoline(void* p) {
    Trampoline tr = *(Trampoline*)p;
    {
        Guard g;
        free(p);
        if (g.entered) rec_thread_start((void*)tr.fn);
    }
    void* ret = tr.fn(tr.arg);
    Guard g;
    if (g.entered) rec_thread_exit();
    return ret;
}

extern "C" int pthread_create(pthread_t* th, const pthread_attr_t* a, void* (*fn)(void*), void* arg) {
    Guard g;
    if (!g.entered) PASS(thread_create, th, a, fn, arg);
    Trampoline* tr = (Trampoline*)malloc(sizeof(Trampoline));
    if (!tr) return real.thread_create(th, a, fn, arg);
    tr->fn = fn;
    tr->arg = arg;
    int rc = real.thread_create(th, a, trampoline, tr);
    if (rc != 0) free(tr);
    return rc;
}

extern "C" void pthread_exit(void* ret) {
    {
        Guard g;
        if (g.entered) rec_thread_exit();
    }
    if (!real.thread_exit) resolve_all();
    real.thread_exit(ret);
    __builtin_unreachable();
}

// ---------------- start-up and shut-down ----------------

__attribute__((constructor)) static void lockspy_init() {
    t_in_spy = true;
    resolve_all();
    config_load();
    void* warm[4];
    backtrace(warm, 4);  // the first backtrace() loads libgcc_s and allocates: do it now
    sym_init();
    rec_init_state();
    pthread_atfork(rec_atfork_prepare, rec_atfork_parent, rec_atfork_child);
    wf_start();
    g_ready = true;
    t_in_spy = false;
}

__attribute__((destructor)) static void lockspy_fini() {
    if (!g_ready) return;
    t_in_spy = true;  // from here on this thread's calls pass straight through
    wf_stop();
    report_final();
    g_ready = false;
}
