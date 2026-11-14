// profiler.cpp: counts acquisitions, wait time and hold time per lock. Condition variable sleeps
// and semaphore waits are counted separately (waiting for a signal isn't lock contention).
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
