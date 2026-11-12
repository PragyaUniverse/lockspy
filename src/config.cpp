// config.cpp - parses the LOCKSPY_* environment variables (see config.h for the list).
// OS concept: process environment, inherited from the parent shell or the bin/lockspy wrapper.
#include "config.h"
#include <cstdlib>
#include <cstring>

namespace ls {

// Heap-allocated on first use, for the same initialisation-order reason as State (recorder.cpp).
static Config* g_cfg = nullptr;
static Config& mut() {
    if (!g_cfg) g_cfg = new Config();
    return *g_cfg;
}
const Config& cfg() { return mut(); }

static int env_int(const char* name, int def, int lo, int hi) {
    const char* v = getenv(name);
    if (!v || !*v) return def;
    int x = atoi(v);
    return x < lo ? lo : (x > hi ? hi : x);
}

static bool env_bool(const char* name) {
    const char* v = getenv(name);
    return v && *v && strcmp(v, "0") != 0;
}

void config_load() {
    Config& c = mut();
    if (const char* f = getenv("LOCKSPY_FORMAT")) {
        std::string s = f;
        c.text = s.find("text") != std::string::npos;
        c.json = s.find("json") != std::string::npos;
        c.dot = s.find("dot") != std::string::npos;
        c.html = s.find("html") != std::string::npos;
    }
    if (const char* d = getenv("LOCKSPY_OUTDIR")) c.outdir = d;
    if (const char* n = getenv("LOCKSPY_NAMES")) c.names_file = n;
    if (const char* a = getenv("LOCKSPY_ON_DEADLOCK")) c.abort_on_deadlock = strcmp(a, "abort") == 0;
    c.stack_depth = env_int("LOCKSPY_STACK_DEPTH", 8, 1, 64);
    c.top = env_int("LOCKSPY_TOP", 5, 1, 1000);
    c.watchdog_ms = env_int("LOCKSPY_WATCHDOG_MS", 500, 0, 600000);
    c.quiet = env_bool("LOCKSPY_QUIET");
    c.trace = env_bool("LOCKSPY_TRACE");
    c.naive = env_bool("LOCKSPY_NAIVE");
}

}  // namespace ls
