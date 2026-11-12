// report.cpp - everything the user reads.
//  * report_emit: one finding, printed the moment it is found (synopsis format), with the four
//    Coffman conditions as evidence, the suggested lock order and the call stacks.
//  * report_final: end-of-run summary on stderr, plus lockspy_report.json, lockspy_graph.dot
//    (lock graph as a heat map) and lockspy_report.html when requested by LOCKSPY_FORMAT.
// OS concepts: Coffman's four necessary conditions for deadlock (mutual exclusion, hold and
// wait, no preemption, circular wait); prevention by a global lock order.
#include "report.h"
#include "config.h"
#include "lock_graph.h"
#include "profiler.h"
#include "symbolize.h"
#include <unistd.h>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace ls {

static SpyLock g_print_mu;

static std::string fmt(const char* f, double x) { char b[64]; snprintf(b, sizeof b, f, x); return b; }
static std::string ms(uint64_t ns) { return fmt("%.3f", ns / 1e6); }
static std::string LN(int id) { return lock_name(id); }
static std::string TN(int idx) { return thread_name(idx); }
static std::string mode_str(int lock, Mode m) {
    return lock_ptr(lock)->kind == K_RWLOCK ? (m == M_READ ? " (read)" : " (write)") : "";
}

static const char* kind_title(FindingKind k) {
    switch (k) {
        case F_POTENTIAL: return "POTENTIAL_DEADLOCK";
        case F_LIVE: return "LIVE_DEADLOCK";
        case F_RELOCK: return "RELOCK";
        case F_BAD_UNLOCK: return "BAD_UNLOCK";
        default: return "HELD_AT_EXIT";
    }
}

static std::string order_str(const std::vector<int>& order, const char* sep) {
    std::string s;
    for (size_t i = 0; i < order.size(); i++) s += (i ? sep : "") + LN(order[i]);
    return s;
}

static std::string fix_text(const Finding& f) {
    std::string s;
    if (f.order.size() == 2)
        s = "  Suggested fix: acquire " + LN(f.order[0]) + " before " + LN(f.order[1]) + " in every thread.\n";
    else if (f.order.size() > 2)
        s = "  Suggested fix: acquire the locks in the order " + order_str(f.order, " < ") + " in every thread.\n";
    for (const Step& v : f.violations)
        s += "    change " + site_of(v.stack, nullptr) + ": it takes " + LN(v.want) + " while holding " +
             LN(v.held) + "\n";
    return s;
}

static std::string stacks_text(const std::vector<Step>& steps, const char* what) {
    std::string s = "  Call stacks:\n";
    for (const Step& x : steps) {
        std::string start = thread_start(x.thread);
        s += "    thread " + TN(x.thread) + " (T" + std::to_string(x.thread) +
             (start.empty() ? "" : ", started in " + start + "()") + ") " + what + " " +
             LN(x.want >= 0 ? x.want : x.held) + ":\n";
        std::vector<std::string> lines = stack_lines(x.stack);
        if (lines.empty()) lines.push_back(site_of(x.stack, x.pc));
        for (size_t i = 0; i < lines.size(); i++) s += "      #" + std::to_string(i) + " " + lines[i] + "\n";
    }
    return s;
}

// The four necessary conditions for deadlock, each with the evidence from this finding.
static std::string coffman(const Finding& f) {
    std::vector<int> locks;
    for (const Step& x : f.steps) if (std::find(locks.begin(), locks.end(), x.want) == locks.end()) locks.push_back(x.want);
    std::string names, hw, cyc = TN(f.steps[0].thread);
    bool rw = false;
    for (size_t i = 0; i < locks.size(); i++) {
        names += (i ? ", " : "") + LN(locks[i]);
        rw = rw || lock_ptr(locks[i])->kind == K_RWLOCK;
    }
    for (size_t i = 0; i < f.steps.size(); i++) {
        const Step& x = f.steps[i];
        hw += (i ? "; " : "") + TN(x.thread) + " holds " + LN(x.held) + mode_str(x.held, x.held_mode) +
              (f.kind == F_LIVE ? " and is blocked on " : " while requesting ") + LN(x.want) + mode_str(x.want, x.want_mode);
        cyc += " -> " + LN(x.want) + " -> " + TN(f.kind == F_LIVE ? x.owner : f.steps[(i + 1) % f.steps.size()].thread);
    }
    std::string s = "  Coffman conditions (a deadlock needs all four):\n";
    s += "    ✔ Mutual exclusion  " + names + (rw ? ": a writer excludes everyone" : (locks.size() > 1 ? " are mutexes" : " is a mutex")) +
         ", only one owner at a time\n";
    s += "    ✔ Hold and wait     " + hw + "\n";
    s += "    ✔ No preemption     a pthread lock is never taken away; only its owner can unlock it\n";
    s += "    ✔ Circular wait     " + cyc +
         (f.kind == F_LIVE ? "   (happening now)\n" : "   (possible; it did not have to happen in this run)\n");
    return s;
}

static std::string format(const Finding& f) {
    std::ostringstream o;
    std::string when = "  (#" + std::to_string(f.id) + ", t=" + fmt("%.3f", f.t) + "s)";
    if (f.kind == F_POTENTIAL) {
        o << "[LockSpy] POTENTIAL DEADLOCK: lock-order cycle detected" << when << "\n";
        std::vector<std::string> left;
        size_t w = 0;
        for (const Step& x : f.steps) {
            left.push_back("Lock " + LN(x.held) + " -> Lock " + LN(x.want));
            w = std::max(w, left.back().size());
        }
        for (size_t i = 0; i < f.steps.size(); i++)
            o << "  " << left[i] << std::string(w - left[i].size() + 3, ' ') << "taken by thread " << TN(f.steps[i].thread)
              << " in " << site_of(f.steps[i].stack, nullptr) << "\n";
        o << fix_text(f) << coffman(f) << stacks_text(f.steps, "requesting");
    } else if (f.kind == F_LIVE) {
        size_t n = f.steps.size();
        o << "[LockSpy] LIVE DEADLOCK: " << (n == 1 ? "a thread is waiting for itself" : std::to_string(n) + " threads are blocked in a wait-for cycle")
          << when << "\n";
        for (const Step& x : f.steps)
            o << "  thread " << TN(x.thread) << " holds Lock " << LN(x.held) << " and waits for Lock " << LN(x.want)
              << " (owner: " << (x.owner == x.thread ? "itself" : "thread " + TN(x.owner)) << "), blocked "
              << fmt("%.2f", x.blocked_s) << "s in " << site_of(x.stack, nullptr) << "\n";
        o << coffman(f);
        if (f.predicted_id >= 0)
            o << "  This hang was predicted at t=" << fmt("%.3f", f.predicted_t) << "s (finding #" << f.predicted_id << ").\n";
        if (n == 1) o << "  Suggested fix: do not lock " << LN(f.steps[0].want) << " twice, or make it PTHREAD_MUTEX_RECURSIVE.\n";
        o << fix_text(f) << stacks_text(f.steps, "blocked on");
    } else if (f.kind == F_RELOCK) {
        const Step& x = f.steps[0];
        o << "[LockSpy] MISUSE: relock of a non-recursive lock" << when << "\n"
          << "  thread " << TN(x.thread) << " already holds Lock " << LN(x.held) << " (taken in "
          << frame_str(sym_pc(x.pc)) << ") and requests it again in " << site_of(x.stack, nullptr) << "\n"
          << "  A normal mutex now waits for itself forever; an error-checking mutex returns EDEADLK.\n"
          << stacks_text(f.steps, "relocking");
    } else if (f.kind == F_BAD_UNLOCK) {
        const Step& x = f.steps[0];
        o << "[LockSpy] MISUSE: unlock of a lock this thread does not hold" << when << "\n"
          << "  thread " << TN(x.thread) << " unlocks Lock " << LN(x.want) << " in " << site_of(x.stack, nullptr)
          << " (current owner: " << (x.owner < 0 ? std::string("nobody") : "thread " + TN(x.owner)) << ")\n"
          << stacks_text(f.steps, "unlocking");
    } else {
        const Step& x = f.steps[0];
        o << "[LockSpy] MISUSE: thread exited while holding a lock" << when << "\n"
          << "  thread " << TN(x.thread) << " exited still holding Lock " << LN(x.held) << ", taken in "
          << site_of(x.stack, x.pc) << "\n  Any thread that locks it later will block forever.\n"
          << stacks_text(f.steps, "holding");
    }
    return o.str();
}

void report_emit(Finding& f) {
    {
        Locked l(st().mu);
        f.id = ++st().seq;
    }
    f.text = format(f);
    if (!cfg().quiet) {
        Locked l(g_print_mu);
        fputs(f.text.c_str(), stderr);
        fflush(stderr);
    }
    Locked l(st().mu);
    st().findings.push_back(f);
}

// ---------------- end-of-run reports ----------------

struct Snapshot {
    std::vector<LockInfo> locks;
    std::vector<Edge> edges;
    std::vector<Finding> findings;
    std::vector<std::vector<int>> comps;
    std::vector<std::vector<int>> comp_order;
    std::vector<std::vector<Step>> comp_viol;
    std::unordered_map<uintptr_t, CondStat> conds;
    std::unordered_map<uintptr_t, SemStat> sems;
    int n_threads = 0, sup[3] = {0, 0, 0};
    size_t naive = 0;
    uint64_t events = 0;
    double runtime = 0;
};

static Snapshot snapshot() {
    Snapshot s;
    Locked l(st().mu);
    State& g = st();
    for (LockInfo* L : g.locks) s.locks.push_back(*L);
    s.edges = g.edges;
    s.findings = g.findings;
    s.comps = lg_components();
    for (auto& c : s.comps) {
        s.comp_order.emplace_back();
        s.comp_viol.emplace_back();
        lg_suggest(c, s.comp_order.back(), s.comp_viol.back());
    }
    s.conds = g.conds;
    s.sems = g.sems;
    s.n_threads = (int)g.threads.size();
    for (auto& kv : g.suppressed) if (!g.reported.count(kv.first)) s.sup[kv.second]++;
    s.naive = g.naive_seen.size();
    s.events = g.events;
    s.runtime = secs(now_ns());
    return s;
}

static std::string q(const std::string& in) {  // JSON string literal
    std::string o = "\"";
    for (unsigned char c : in) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c == '\n') o += "\\n";
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o + "\"";
}

static std::string out_path(const char* base, const char* ext) {
    std::string p = cfg().outdir + "/" + base;
    if (st().is_child) p += "." + std::to_string(getpid());
    return p + ext;
}

static std::string summary_text(const Snapshot& s) {
    std::ostringstream o;
    int count[5] = {0, 0, 0, 0, 0};
    for (const Finding& f : s.findings) count[f.kind]++;
    char prog[4096] = "?";
    ssize_t n = readlink("/proc/self/exe", prog, sizeof prog - 1);
    if (n > 0) prog[n] = 0;
    o << "==================== LockSpy summary ====================\n"
      << "program " << prog << "  pid " << getpid() << "  ran " << fmt("%.3f", s.runtime) << " s  ("
      << s.events << " lock events)\n"
      << "threads " << s.n_threads << ", locks " << s.locks.size() << ", lock-order edges " << s.edges.size() << "\n"
      << "findings: " << count[F_POTENTIAL] << " potential deadlock, " << count[F_LIVE] << " live deadlock, "
      << count[F_RELOCK] << " relock, " << count[F_BAD_UNLOCK] << " bad unlock, " << count[F_HELD_AT_EXIT]
      << " held at exit\n"
      << "suppressed as safe: " << (s.sup[0] + s.sup[1] + s.sup[2]) << " cycle(s)  (single thread " << s.sup[R_THREAD]
      << ", gate lock " << s.sup[R_GATE] << ", read-read " << s.sup[R_READ] << ");  a naive detector would report "
      << s.naive << " cycle(s)\n";
    for (const Finding& f : s.findings)
        o << "  #" << f.id << " " << kind_title(f.kind) << " at t=" << fmt("%.3f", f.t) << "s\n";
    if (!s.comps.empty()) {
        o << "lock-order components (Tarjan SCC) and the suggested global order:\n";
        for (size_t i = 0; i < s.comps.size(); i++) {
            o << "  {" << order_str(s.comps[i], ", ") << "}  order: " << order_str(s.comp_order[i], " < ") << "\n";
            for (const Step& v : s.comp_viol[i])
                o << "    violated in " << site_of(v.stack, nullptr) << ": takes " << LN(v.want) << " while holding " << LN(v.held) << "\n";
        }
    }
    std::vector<LockInfo> ranked = prof_ranked(s.locks);
    if (!ranked.empty()) {
        o << "top contended locks (by total wait time):\n";
        char line[512];
        snprintf(line, sizeof line, "  %-3s %-24s %10s %10s %12s %10s %12s %10s\n", "#", "lock", "acquired",
                 "contended", "wait ms", "max wait", "hold ms", "max hold");
        o << line;
        for (size_t i = 0; i < ranked.size() && (int)i < cfg().top; i++) {
            const LockInfo& L = ranked[i];
            snprintf(line, sizeof line, "  %-3zu %-24s %10llu %10llu %12s %10s %12s %10s\n", i + 1, LN(L.id).c_str(),
                     (unsigned long long)L.acq, (unsigned long long)L.contended, ms(L.wait_ns).c_str(),
                     ms(L.wait_max).c_str(), ms(L.hold_ns).c_str(), ms(L.hold_max).c_str());
            o << line;
        }
    }
    for (auto& kv : s.conds)
        o << "condition variable " << (void*)kv.first << ": " << kv.second.waits << " waits, " << ms(kv.second.wait_ns)
          << " ms asleep (max " << ms(kv.second.wait_max) << " ms)\n";
    for (auto& kv : s.sems)
        o << "semaphore " << (void*)kv.first << ": " << kv.second.waits << " waits (" << kv.second.contended
          << " blocked), " << kv.second.posts << " posts, " << ms(kv.second.wait_ns) << " ms waiting (max "
          << ms(kv.second.wait_max) << " ms)\n";
    return o.str();
}

static void write_json(const Snapshot& s, const std::string& path) {
    std::ofstream o(path);
    o << "{\n  \"tool\": \"LockSpy\",\n  \"pid\": " << getpid() << ",\n  \"runtime_s\": " << fmt("%.6f", s.runtime)
      << ",\n  \"events\": " << s.events << ",\n  \"threads\": [";
    for (int i = 0; i < s.n_threads; i++)
        o << (i ? ", " : "") << "{\"id\": " << i << ", \"name\": " << q(TN(i)) << ", \"start\": " << q(thread_start(i)) << "}";
    o << "],\n  \"locks\": [";
    for (size_t i = 0; i < s.locks.size(); i++) {
        const LockInfo& L = s.locks[i];
        o << (i ? "," : "") << "\n    {\"id\": " << L.id << ", \"name\": " << q(LN(L.id)) << ", \"kind\": "
          << q(L.kind == K_MUTEX ? "mutex" : "rwlock") << ", \"recursive\": " << (L.recursive ? "true" : "false")
          << ", \"acquisitions\": " << L.acq << ", \"contended\": " << L.contended << ", \"wait_ns\": " << L.wait_ns
          << ", \"max_wait_ns\": " << L.wait_max << ", \"hold_ns\": " << L.hold_ns << ", \"max_hold_ns\": " << L.hold_max << "}";
    }
    o << "\n  ],\n  \"edges\": [";
    for (size_t i = 0; i < s.edges.size(); i++) {
        const Edge& E = s.edges[i];
        o << (i ? "," : "") << "\n    {\"from\": " << E.from << ", \"to\": " << E.to << ", \"count\": " << E.count
          << ", \"threads\": [";
        for (size_t k = 0; k < E.threads.size(); k++) o << (k ? ", " : "") << E.threads[k];
        o << "], \"in_cycle\": " << (E.in_cycle ? "true" : "false") << ", \"suppressed\": "
          << (E.suppressed && !E.in_cycle ? "true" : "false") << ", \"site\": "
          << q(E.wit.empty() ? "" : site_of(E.wit[0].stack, nullptr)) << "}";
    }
    o << "\n  ],\n  \"findings\": [";
    for (size_t i = 0; i < s.findings.size(); i++) {
        const Finding& f = s.findings[i];
        o << (i ? "," : "") << "\n    {\"id\": " << f.id << ", \"type\": " << q(kind_title(f.kind)) << ", \"t\": "
          << fmt("%.6f", f.t) << ", \"predicted_by\": " << f.predicted_id << ", \"suggested_order\": [";
        for (size_t k = 0; k < f.order.size(); k++) o << (k ? ", " : "") << q(LN(f.order[k]));
        o << "], \"steps\": [";
        for (size_t k = 0; k < f.steps.size(); k++) {
            const Step& x = f.steps[k];
            o << (k ? ", " : "") << "{\"thread\": " << q(TN(x.thread)) << ", \"holds\": "
              << q(x.held >= 0 ? LN(x.held) : "") << ", \"wants\": " << q(x.want >= 0 ? LN(x.want) : "")
              << ", \"stack\": [";
            std::vector<std::string> lines = stack_lines(x.stack);
            for (size_t j = 0; j < lines.size(); j++) o << (j ? ", " : "") << q(lines[j]);
            o << "]}";
        }
        o << "], \"text\": " << q(f.text) << "}";
    }
    o << "\n  ],\n  \"suppressed\": {\"single_thread\": " << s.sup[R_THREAD] << ", \"gate_lock\": " << s.sup[R_GATE]
      << ", \"read_read\": " << s.sup[R_READ] << "},\n  \"naive_cycles\": " << s.naive << "\n}\n";
}

// Heat map: fill colour by share of the total wait time; red bold = edge of a reported cycle;
// dashed grey = edge of a cycle that was suppressed as safe.
static void write_dot(const Snapshot& s, const std::string& path) {
    const char* heat[] = {"#ffffff", "#fee5d9", "#fcae91", "#fb6a4a", "#de2d26"};
    uint64_t max_wait = 1;
    for (const LockInfo& L : s.locks) max_wait = std::max(max_wait, L.wait_ns);
    std::ofstream o(path);
    o << "digraph lockspy {\n  rankdir=LR;\n  node [shape=box, style=\"rounded,filled\", fontname=\"Helvetica\"];\n"
      << "  edge [fontname=\"Helvetica\", fontsize=10];\n";
    for (const LockInfo& L : s.locks) {
        int h = L.wait_ns == 0 ? 0 : 1 + (int)(3.0 * L.wait_ns / max_wait + 0.0001);
        if (h > 4) h = 4;
        o << "  L" << L.id << " [label=\"" << LN(L.id) << "\\n" << L.acq << " acq, wait " << ms(L.wait_ns)
          << " ms\", fillcolor=\"" << heat[h] << "\"" << (L.dead ? ", color=gray" : "") << "];\n";
    }
    for (const Edge& E : s.edges) {
        std::string label;
        for (size_t k = 0; k < E.threads.size() && k < 3; k++) label += (k ? "," : "") + TN(E.threads[k]);
        if (E.threads.size() > 3) label += ",...";
        label += " x" + std::to_string(E.count);
        o << "  L" << E.from << " -> L" << E.to << " [label=\"" << label << "\"";
        if (E.in_cycle) o << ", color=red, penwidth=2.5, fontcolor=red";
        else if (E.suppressed) o << ", style=dashed, color=gray50, fontcolor=gray50";
        o << "];\n";
    }
    o << "}\n";
}

static std::string html_escape(const std::string& in) {
    std::string o;
    for (char c : in) o += c == '<' ? "&lt;" : c == '>' ? "&gt;" : c == '&' ? "&amp;" : std::string(1, c);
    return o;
}

static void write_html(const Snapshot& s, const std::string& summary, const std::string& dot_path) {
    std::string svg = run_capture({"dot", "-Tsvg", dot_path});
    size_t p = svg.find("<svg");
    svg = p == std::string::npos ? "<p>(Graphviz 'dot' not found: no graph)</p>" : svg.substr(p);
    std::ofstream o(out_path("lockspy_report", ".html"));
    o << "<!doctype html><html><head><meta charset=\"utf-8\"><title>LockSpy report</title><style>"
         "body{font-family:sans-serif;max-width:1100px;margin:2em auto;padding:0 1em;background:#fff;color:#222}"
         "pre{background:#f6f6f6;padding:1em;overflow-x:auto;font-size:13px}svg{max-width:100%;height:auto}"
         "</style></head><body><h1>LockSpy report</h1><h2>Lock graph</h2>"
      << svg << "<h2>Summary</h2><pre>" << html_escape(summary) << "</pre><h2>Findings</h2>";
    for (const Finding& f : s.findings) o << "<pre>" << html_escape(f.text) << "</pre>";
    if (s.findings.empty()) o << "<p>No findings.</p>";
    o << "</body></html>\n";
}

void report_final() {
    static bool done = false;
    if (done) return;
    done = true;
    Snapshot s = snapshot();
    std::string summary = summary_text(s);
    std::string written;
    if (cfg().json) { write_json(s, out_path("lockspy_report", ".json")); written += " " + out_path("lockspy_report", ".json"); }
    if (cfg().dot || cfg().html) { write_dot(s, out_path("lockspy_graph", ".dot")); written += " " + out_path("lockspy_graph", ".dot"); }
    if (cfg().html) { write_html(s, summary, out_path("lockspy_graph", ".dot")); written += " " + out_path("lockspy_report", ".html"); }
    if (!written.empty()) summary += "reports written:" + written + "\n";
    if (cfg().text && !cfg().quiet) {
        Locked l(g_print_mu);
        fputs(summary.c_str(), stderr);
        fflush(stderr);
    }
}

}  // namespace ls
