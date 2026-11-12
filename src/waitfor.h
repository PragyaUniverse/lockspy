// waitfor.h - live wait-for graph (thread -> lock it waits for -> owner thread) and the
// watchdog thread that looks for cycles in it.
// OS concepts: wait-for graph, deadlock detection after the fact, a helper thread.
#pragma once

namespace ls {

void wf_start();   // start the watchdog thread (uses the real pthread_create)
void wf_stop();
void wf_check();   // one scan of the wait-for graph (the watchdog calls this periodically)

}  // namespace ls
