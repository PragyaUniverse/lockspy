// symbolize.h - addresses -> readable names (functions with file:line, locks, threads).
// OS concepts: dynamic linking (dladdr), ELF symbol tables, /proc/self/task/<tid>/comm.
#pragma once
#include "recorder.h"
#include <string>
#include <vector>

namespace ls {

struct Frame {
    std::string func, file;   // "worker_p", "ab_ba.cpp"
    int line = 0;
    bool ours = false;        // inside libLockSpy.so
    bool sys = false;         // inside libc / libstdc++ / the dynamic loader
};

void sym_init();
void sym_lock();
void sym_unlock();
Frame sym_pc(void* pc, bool return_addr = true);
std::string frame_str(const Frame& f);                  // "worker_p() (ab_ba.cpp:27)"
std::string site_of(const Stack& s, void* fallback_pc);  // first frame in the user's own code
std::vector<std::string> stack_lines(const Stack& s);   // user-visible frames, innermost first
std::string lock_name(int id);
std::string thread_name(int idx);
std::string thread_start(int idx);                      // name of the start routine, "" if none
std::string run_capture(const std::vector<std::string>& argv);  // run a helper, return stdout

}  // namespace ls
