// profiler.cpp - contention statistics. For each lock: how often it was acquired, how often a
// thread had to wait (the trylock fast path failed), total/max wait time and total/max hold
// time. Condition-variable sleeps and semaphore waits are counted separately, because waiting
// for a signal is not lock contention.
// OS concepts: blocking on a mutex, condition variables (monitors), counting semaphores.
#include "profiler.h"
#include <algorithm>

namespace ls {

void prof_acquired(LockInfo* L, uint64_t wait_ns, bool contended) {
    L->acq++;
    if (contended) L->contended++;
    L->wait_ns += wait_ns;
    L->wait_max = std::max(L->wait_max, wait_ns);
}

void prof_released(LockInfo* L, uint64_t hold_ns) {
    L->hold_ns += hold_ns;
    L->hold_max = std::max(L->hold_max, hold_ns);
}

void prof_cond(void* cv, uint64_t wait_ns) {
    CondStat& c = st().conds[(uintptr_t)cv];
    c.waits++;
    c.wait_ns += wait_ns;
    c.wait_max = std::max(c.wait_max, wait_ns);
}

void prof_sem_wait(void* sem, uint64_t wait_ns, bool contended) {
    SemStat& s = st().sems[(uintptr_t)sem];
    s.waits++;
    if (contended) s.contended++;
    s.wait_ns += wait_ns;
    s.wait_max = std::max(s.wait_max, wait_ns);
}

void prof_sem_post(void* sem) { st().sems[(uintptr_t)sem].posts++; }

// Rank by total wait time (the time threads lost because of this lock), then by acquisitions.
std::vector<LockInfo> prof_ranked(const std::vector<LockInfo>& locks) {
    std::vector<LockInfo> r;
    for (const LockInfo& L : locks) if (L.acq > 0) r.push_back(L);
    std::stable_sort(r.begin(), r.end(), [](const LockInfo& a, const LockInfo& b) {
        if (a.wait_ns != b.wait_ns) return a.wait_ns > b.wait_ns;
        return a.acq > b.acq;
    });
    return r;
}

}  // namespace ls
