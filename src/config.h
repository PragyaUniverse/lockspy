// config.h - LockSpy settings, read once from LOCKSPY_* environment variables.
// OS concept: the process environment. The bin/lockspy wrapper turns command-line flags into
// environment variables, and the preloaded library reads them in its constructor, because a
// preloaded library has no argv of its own.
#pragma once
#include <string>

namespace ls {

struct Config {
    bool text = true, json = false, dot = false, html = false;  // LOCKSPY_FORMAT=text,json,dot,html
    std::string outdir = ".";                                    // LOCKSPY_OUTDIR
    int stack_depth = 8;                                         // LOCKSPY_STACK_DEPTH
    int top = 5;                                                 // LOCKSPY_TOP
    int watchdog_ms = 500;                                       // LOCKSPY_WATCHDOG_MS (0 = off)
    bool abort_on_deadlock = false;                              // LOCKSPY_ON_DEADLOCK=report|abort
    bool quiet = false;                                          // LOCKSPY_QUIET=1: no stderr output
    bool trace = false;                                          // LOCKSPY_TRACE=1: print every event
    bool naive = false;  // LOCKSPY_NAIVE=1: no gate/thread/read filter, trylock adds edges (for comparison)
    std::string names_file;                                      // LOCKSPY_NAMES
};

const Config& cfg();
void config_load();

}  // namespace ls
