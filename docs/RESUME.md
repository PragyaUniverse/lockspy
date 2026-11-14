# Resume notes

Numbers come from results/RESULTS.md (`make bench`). Pick whatever fits, and reword it so it
sounds like you.

**Title:** LockSpy: deadlock and lock-contention detector for Linux C/C++ programs (C++17, pthreads, LD_PRELOAD)

**Bullet points**

- Built a C++ shared library that intercepts pthread lock calls with LD_PRELOAD and detects
  potential and live deadlocks in programs without recompiling them (about 185 ns overhead per
  lock/unlock).
- Reduced false alarms to 0 on our 16-program test set by filtering out cycles protected by a common
  lock, single-thread cycles and trylock patterns. ThreadSanitizer gave 2 false alarms and Helgrind 3.
- Added a suggested lock order for every deadlock, computed with Tarjan's SCC algorithm, plus a web
  UI to run programs and look at the lock graph. Average slowdown 3.1x vs 4.3x for ThreadSanitizer.

**If someone asks "tell me about this project" (30 seconds)**

Deadlocks only happen with unlucky timing, so a program can pass all its tests and still hang
later. We wrote a library you load into any pthread program. It keeps track of which locks each
thread holds, and if two threads take the same locks in opposite orders it warns you, even if that
run didn't hang. If it does hang, it shows you the threads that are stuck, and it tells you which
lock order would fix it. On our tests it caught all 9 bugs with no false alarms.

**Things we learned**

1. How the dynamic linker finds functions, and how LD_PRELOAD and `dlsym(RTLD_NEXT)` let you get in
   between a program and libc (and gotchas like the two versions of `pthread_cond_wait`).
2. Code that runs inside someone else's process has to be careful: no calling yourself by
   accident, watch out for constructor order, don't do slow things while holding a lock.
3. The difference between a potential and an actual deadlock, and why some cycles (gate locks,
   trylock, one thread) can never deadlock.
4. Graph algorithms on real data: DFS for cycles, Tarjan's SCC, a greedy ordering heuristic.
5. Benchmarking fairly against other tools, including writing down where ThreadSanitizer beat us.
