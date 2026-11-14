// symbolize.cpp - turns raw addresses into names a person can read. Only used when reporting,
// never on the lock/unlock fast path.
//  * Code address -> "worker_p() (ab_ba.cpp:27)": dladdr() finds the loaded object (program or
//    .so) and its load address; the binutils tool addr2line maps the offset to function/file/line.
//  * Lock address -> name: (1) LOCKSPY_NAMES file, (2) the global variable that contains it,
//    found in the ELF symbol table with nm ("A", "forks[3]"), (3) where it was initialised or
//    first locked ("mutex@bank.cpp:12"), (4) "mutex@0x...".
//  * Thread -> name: /proc/self/task/<tid>/comm if the program named it, else T1, T2, ...
// OS concepts: dynamic linking and load addresses, ELF symbol tables, /proc, process creation
// (posix_spawn + pipe; LD_PRELOAD is removed so the helper tools are not traced themselves).
#include "symbolize.h"
#include "config.h"
#include <dlfcn.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

extern char** environ;

namespace ls {

struct Sym { uint64_t addr, size; std::string name; };
struct SymCache {                                   // heap-allocated: see st() in recorder.cpp
    std::map<void*, Frame> frames;
    std::map<std::string, bool> is_exec;
    std::map<std::string, std::vector<Sym>> data_syms;
    std::map<std::string, int> name_uses;
    std::map<std::string, std::string> user_names;  // LOCKSPY_NAMES: "file:line" or "func" -> name
    std::string proc_comm;
    void* our_base = nullptr;
};
static SymCache* C = nullptr;
static SpyLock g_mu;
void sym_lock() { g_mu.lock(); }
void sym_unlock() { g_mu.unlock(); }

void sym_init() {
    C = new SymCache();
    Dl_info di;
    if (dladdr((void*)&sym_init, &di)) C->our_base = di.dli_fbase;
    std::ifstream comm("/proc/self/comm");
    std::getline(comm, C->proc_comm);
    if (!cfg().names_file.empty()) {
        std::ifstream in(cfg().names_file);
        std::string line;
        while (std::getline(in, line)) {
            size_t eq = line.find('=');
            if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
            auto trim = [](std::string x) {
                size_t a = x.find_first_not_of(" \t"), b = x.find_last_not_of(" \t\r");
                return a == std::string::npos ? std::string() : x.substr(a, b - a + 1);
            };
            C->user_names[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
        }
    }
}

std::string run_capture(const std::vector<std::string>& args) {
    int fd[2];
    if (pipe2(fd, O_CLOEXEC) != 0) return "";
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fd[1], 1);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    std::vector<char*> argv, envp;
    for (const std::string& a : args) argv.push_back((char*)a.c_str());
    argv.push_back(nullptr);
    for (char** e = environ; *e; ++e) if (strncmp(*e, "LD_PRELOAD=", 11) != 0) envp.push_back(*e);
    envp.push_back(nullptr);
    pid_t pid;
    int rc = posix_spawnp(&pid, argv[0], &fa, nullptr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&fa);
    close(fd[1]);
    std::string out;
    if (rc == 0) {
        char buf[4096];
        ssize_t n;
        while ((n = read(fd[0], buf, sizeof buf)) > 0) out.append(buf, n);
        waitpid(pid, nullptr, 0);
    }
    close(fd[0]);
    return out;
}

static std::string object_path(const Dl_info& di) {
    if (di.dli_fname && strchr(di.dli_fname, '/')) return di.dli_fname;
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);  // the main program
    return n > 0 ? std::string(buf, n) : std::string();
}

// A non-PIE executable (ELF type ET_EXEC) uses absolute addresses; a .so or PIE uses offsets.
static bool is_exec(const std::string& path) {
    auto it = C->is_exec.find(path);
    if (it != C->is_exec.end()) return it->second;
    unsigned char h[18] = {0};
    if (FILE* f = fopen(path.c_str(), "rb")) { size_t got = fread(h, 1, sizeof h, f); (void)got; fclose(f); }
    return C->is_exec[path] = (h[16] | (h[17] << 8)) == 2;
}

static bool is_sys(const std::string& path) {
    const char* libs[] = {"libc.so", "libstdc++", "ld-linux", "libpthread", "libgcc_s", "libm.so"};
    for (const char* l : libs) if (path.find(l) != std::string::npos) return true;
    return false;
}

static std::string short_func(std::string f) {  // "worker_p(void*)" -> "worker_p"
    size_t p = f.find('(');
    if (p != std::string::npos && p > 0 && f.compare(0, 8, "operator") != 0) f = f.substr(0, p);
    return f;
}

static Frame sym_pc_locked(void* pc, bool return_addr) {
    auto it = C->frames.find(pc);
    if (it != C->frames.end()) return it->second;
    Frame f;
    Dl_info di;
    if (!dladdr(pc, &di)) { f.func = "??"; return C->frames[pc] = f; }
    f.ours = di.dli_fbase == C->our_base;
    std::string path = object_path(di);
    f.sys = is_sys(path);
    if (di.dli_sname) f.func = di.dli_sname;
    if (!f.ours && !f.sys) {
        uintptr_t a = (uintptr_t)pc - (return_addr ? 1 : 0);  // a return address points after the call
        if (!is_exec(path)) a -= (uintptr_t)di.dli_fbase;
        char hex[32];
        snprintf(hex, sizeof hex, "0x%lx", (unsigned long)a);
        std::istringstream out(run_capture({"addr2line", "-C", "-f", "-e", path, hex}));
        std::string func, loc;
        std::getline(out, func);
        std::getline(out, loc);
        if (!func.empty() && func != "??") f.func = func;
        size_t colon = loc.rfind(':');
        if (colon != std::string::npos && loc.compare(0, 2, "??") != 0) {
            std::string file = loc.substr(0, colon);
            f.file = file.substr(file.rfind('/') + 1);
            f.line = atoi(loc.c_str() + colon + 1);
        }
    }
    if (f.func.empty()) f.func = "??";
    f.func = short_func(f.func);
    return C->frames[pc] = f;
}

Frame sym_pc(void* pc, bool return_addr) {
    sym_lock();
    Frame f = sym_pc_locked(pc, return_addr);
    sym_unlock();
    return f;
}

std::string frame_str(const Frame& f) {
    std::string s = f.func + "()";
    if (!f.file.empty()) s += " (" + f.file + ":" + std::to_string(f.line) + ")";
    return s;
}

// Frames from the C++ standard library wrappers (std::mutex::lock, lock_guard, ...) are not
// where the programmer wrote the lock call, so skip them when choosing "the" call site.
static bool is_wrapper(const Frame& f) {
    return f.func.compare(0, 5, "std::") == 0 || f.func.compare(0, 9, "__gthread") == 0 ||
           f.func.compare(0, 9, "__gnu_cxx") == 0;
}

std::string site_of(const Stack& s, void* fallback_pc) {
    for (void* pc : s) {
        Frame f = sym_pc(pc);
        if (!f.ours && !f.sys && !is_wrapper(f)) return frame_str(f);
    }
    if (fallback_pc) return frame_str(sym_pc(fallback_pc));
    return "an unknown site";
}

std::vector<std::string> stack_lines(const Stack& s) {
    std::vector<std::string> r;
    for (void* pc : s) {
        Frame f = sym_pc(pc);
        if (f.ours) continue;
        if (f.sys && !r.empty()) break;  // stop at libc (start_thread, __libc_start_main, ...)
        if (f.sys) continue;
        r.push_back(frame_str(f));
        if ((int)r.size() >= cfg().stack_depth) break;
    }
    return r;
}

// Symbols of initialised/zeroed data (nm types b, B, d, D) of one object file, with sizes.
static const std::vector<Sym>& data_syms(const std::string& path) {
    auto it = C->data_syms.find(path);
    if (it != C->data_syms.end()) return it->second;
    std::vector<Sym>& v = C->data_syms[path];
    std::istringstream in(run_capture({"nm", "-S", "-C", "--defined-only", path}));
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string a, sz, type;
        if (!(ls >> a >> sz >> type) || type.size() != 1 || !strchr("bBdD", type[0])) continue;
        std::string name;
        std::getline(ls, name);
        if (!name.empty() && name[0] == ' ') name.erase(0, 1);
        v.push_back(Sym{strtoull(a.c_str(), nullptr, 16), strtoull(sz.c_str(), nullptr, 16), name});
    }
    return v;
}

// Name of the global variable that contains addr ("A", "forks[3]", "accounts+40"), or "".
static std::string global_name(uintptr_t addr, uint64_t elem) {
    Dl_info di;
    if (!dladdr((void*)addr, &di) || di.dli_fbase == C->our_base) return "";
    std::string path = object_path(di);
    uint64_t a = addr - (is_exec(path) ? 0 : (uintptr_t)di.dli_fbase);
    for (const Sym& s : data_syms(path)) {
        if (a < s.addr || a >= s.addr + s.size) continue;
        uint64_t off = a - s.addr;
        if (off == 0 && s.size == elem) return s.name;
        if (off % elem == 0 && s.size % elem == 0) return s.name + "[" + std::to_string(off / elem) + "]";
        return s.name + "+" + std::to_string(off);
    }
    return "";
}

std::string object_name(uintptr_t addr, uint64_t size) {
    sym_lock();
    std::string n = global_name(addr, size);
    sym_unlock();
    if (n.empty()) { char b[32]; snprintf(b, sizeof b, "%#lx", (unsigned long)addr); n = b; }
    return n;
}

static std::string user_name(void* pc) {
    if (!pc || C->user_names.empty()) return "";
    Frame f = sym_pc_locked(pc, true);
    auto it = C->user_names.find(f.file + ":" + std::to_string(f.line));
    if (it == C->user_names.end()) it = C->user_names.find(f.func);
    return it == C->user_names.end() ? "" : it->second;
}

std::string lock_name(int id) {
    LockInfo* L = lock_ptr(id);
    sym_lock();
    if (L->name.empty()) {
        const char* kind = L->kind == K_MUTEX ? "mutex" : "rwlock";
        std::string n = user_name(L->init_pc);
        if (n.empty()) n = user_name(L->first_pc);
        if (n.empty()) n = global_name(L->addr, L->kind == K_MUTEX ? sizeof(pthread_mutex_t) : sizeof(pthread_rwlock_t));
        if (n.empty()) {
            void* pc = L->init_pc ? L->init_pc : L->first_pc;
            Frame f = pc ? sym_pc_locked(pc, true) : Frame();
            if (!f.file.empty()) n = std::string(kind) + "@" + f.file + ":" + std::to_string(f.line);
        }
        if (n.empty()) {
            char buf[48];
            snprintf(buf, sizeof buf, "%s@%#lx", kind, (unsigned long)L->addr);
            n = buf;
        }
        int uses = ++C->name_uses[n];  // several locks from one init site: add #2, #3, ...
        if (uses > 1) n += "#" + std::to_string(uses);
        L->name = n;
    }
    std::string r = L->name;
    sym_unlock();
    return r;
}

std::string thread_name(int idx) {
    ThreadState* t = thread_ptr(idx);
    char comm[32];
    bool alive;
    pid_t tid;
    {
        Locked l(st().mu);
        memcpy(comm, t->comm, sizeof comm);
        alive = t->alive;
        tid = t->tid;
    }
    if (alive && !st().is_child) {  // the program may have renamed the thread since (pthread_setname_np)
        char path[64];
        snprintf(path, sizeof path, "/proc/self/task/%d/comm", (int)tid);
        std::ifstream in(path);
        std::string c;
        if (std::getline(in, c) && !c.empty()) snprintf(comm, sizeof comm, "%s", c.c_str());
    }
    if (idx == 0) return "main";
    if (comm[0] && C && C->proc_comm != comm) return comm;
    return "T" + std::to_string(idx);
}

std::string thread_start(int idx) {
    ThreadState* t = thread_ptr(idx);
    if (!t->start_fn) return idx == 0 ? "main" : "";
    return sym_pc(t->start_fn, false).func;
}

}  // namespace ls
