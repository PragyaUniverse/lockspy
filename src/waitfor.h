// waitfor.h: live deadlock detection (watchdog thread + wait-for graph).
#pragma once

namespace ls {

void wf_start();   // start the watchdog thread (uses the real pthread_create)
void wf_stop();
void wf_check();   // one scan of the wait-for graph (the watchdog calls this periodically)

}  // namespace ls
