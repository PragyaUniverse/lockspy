# LockSpy on a resume

All numbers below are from [results/RESULTS.md](../results/RESULTS.md) (`make bench`).

**Project title:** LockSpy — runtime deadlock and lock-contention detector for unmodified Linux C/C++ programs (C++17, POSIX threads, LD_PRELOAD)

**Bullets**

- Built an `LD_PRELOAD` shim in C++17 that intercepts pthread mutex, rwlock, condition-variable and semaphore calls in unmodified binaries and detects potential deadlocks (lock-order graph + DFS) and live deadlocks (wait-for graph + watchdog thread), with about 185 ns overhead per uncontended lock/unlock pair.
- Cut false alarms to zero on a 16-program benchmark (precision 1.00, recall 1.00) with gate-lock, single-thread and trylock filtering, against 3 false alarms for the same detector without filters, 2 for ThreadSanitizer and 3 for Valgrind Helgrind.
- Turned each report into a fix: Tarjan SCC plus greedy ordering suggests a global lock order and names the call site that violates it; each report also prints evidence for the four Coffman conditions. Geometric-mean slowdown 3.1× vs 4.3× for ThreadSanitizer and 122.5× for Helgrind, with no recompile.

**30-second pitch**

> Deadlocks depend on timing, so a program can pass every test and still freeze later. LockSpy is a
> library you preload into any pthread program, with no recompile. It records which locks each thread
> holds and builds a graph of lock orders. If two code paths take locks in opposite orders, it warns
> you even when that run didn't hang, and when a hang does happen it shows that it predicted it.
> Then it tells you the fix: the lock order to use and the exact line that breaks it. On our benchmark
> it caught all 9 bugs with no false alarms, where ThreadSanitizer missed 3 and Helgrind raised 3 false alarms.

**What I learned**

1. How the dynamic linker resolves symbols, and how `LD_PRELOAD` and `dlsym(RTLD_NEXT)` let you sit
   between a program and libc, including traps like versioned symbols (`pthread_cond_wait`).
2. Writing code that runs *inside* someone else's process: re-entrancy guards, thread-local storage,
   initialisation order of constructors, and never doing slow work while holding a lock.
3. Deadlock theory in practice: the four Coffman conditions, why a lock-order cycle is only a
   *potential* deadlock, and why gate locks, trylock and single-thread cycles are not real ones.
4. Graph algorithms on real data: DFS cycle detection, Tarjan's strongly connected components and a
   greedy feedback-arc-set heuristic to suggest a lock order.
5. Honest benchmarking: comparing against ThreadSanitizer and Helgrind, measuring medians, and
   reporting the case where a competitor was faster (TSan on a tight uncontended loop).
