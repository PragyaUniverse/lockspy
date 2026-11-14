// profiler.h: per-lock contention stats. prof_* functions expect State::mu to be held.
#pragma once
#include "recorder.h"

namespace ls {

void prof_acquired(LockInfo* L, uint64_t wait_ns, bool contended);
void prof_released(LockInfo* L, uint64_t hold_ns);
void prof_cond(void* cv, uint64_t wait_ns);
void prof_sem_wait(void* sem, uint64_t wait_ns, bool contended);
void prof_sem_post(void* sem);
std::vector<LockInfo> prof_ranked(const std::vector<LockInfo>& locks);  // most contended first

}  // namespace ls
