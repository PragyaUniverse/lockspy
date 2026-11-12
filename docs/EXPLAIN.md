# LockSpy viva guide

## 1. The two-minute explanation

> Multithreaded programs protect shared data with mutexes. If two threads take the same two
> locks in opposite orders, they can deadlock, but only when the timing is unlucky, so the bug can
> pass every test and then freeze in production.
>
> LockSpy is a shared library that we load into an unmodified program with `LD_PRELOAD`. The
> dynamic linker then sends every `pthread_mutex_lock`, `unlock`, `trylock`, condition-variable
> and rwlock call to our wrapper first. The wrapper records who took which lock, when, and from
> where, and then calls the real function in libc, found with `dlsym(RTLD_NEXT)`.
>
> From those events we keep, for every thread, the set of locks it holds. Whenever a thread asks
> for lock L while holding H, we add an edge H→L to a global lock-order graph. A cycle in that graph
> means two code paths disagree about the order, so we report a POTENTIAL DEADLOCK, even if this
> run never hung. Separately, a watchdog thread looks at the live wait-for graph (thread waits for
> lock, lock is owned by thread); a cycle there is a deadlock that is happening right now.
>
> What makes LockSpy different from a basic cycle detector: it suppresses cycles that cannot
> happen (a common gate lock, a single thread, trylock), it proves each report with the four Coffman
> conditions, it suggests a concrete global lock order and names the line to change, and it links
> a live hang back to the earlier prediction. It also profiles which locks are contended, and
> writes text, JSON and a Graphviz heat map.

## 2. Follow one `pthread_mutex_lock` call through the code

Thread Q in `tests/buggy/ab_ba.cpp` holds `B` and calls `pthread_mutex_lock(&A)`.

1. **The dynamic linker picks our function.** Because `libLockSpy.so` is in `LD_PRELOAD`, the
   symbol `pthread_mutex_lock` resolves to `src/interpose.cpp:72`, not to libc.
2. **Re-entrancy guard.** `Guard g` (`src/recorder.h:54`) sets the thread-local flag `t_in_spy`.
   If it was already set (LockSpy itself is locking), we call the real function and stop.
3. **Fast path.** We call the real `pthread_mutex_trylock` (`interpose.cpp:76`). If it succeeds,
   `rec_acquired` (`src/recorder.cpp:145`) records the acquisition and we return. Here A is free,
   so this path is taken.
4. **Edges.** Inside `rec_acquired`, under the private lock `State::mu`, `lg_note_edges`
   (`src/lock_graph.cpp:24`) loops over Q's held set {B} and adds edge B→A with a *witness*: thread Q,
   modes, other held locks (none), stack to be filled. The edge is new, so it goes into the `Todo`.
5. **Held set and profile.** `add_held` puts A into Q's held set and into A's list of holders (the
   lock→owner part of the wait-for graph); `prof_acquired` (`src/profiler.cpp:11`) counts it.
6. **Slow work after unlocking.** `finish` (`recorder.cpp:126`) releases `State::mu`, captures the
   stack with `backtrace()`, re-locks, stores the stack in the witness and calls `lg_check`.
7. **DFS.** `check_edge` (`lock_graph.cpp:106`) asks: can A already reach B? `find_paths`
   (`lock_graph.cpp:61`) finds the edge A→B taken earlier by thread P. Cycle: A→B→A.
8. **Filter.** `choose` (`lock_graph.cpp:83`) picks one witness per edge: P for A→B, Q for B→A.
   Two different threads, no common gate lock, no read-read step: the cycle is real.
9. **Fix.** `lg_suggest` (`lock_graph.cpp:179`) orders {A, B}: tie, so the lock created first wins:
   A < B. The edge B→A points backwards: Q's call site is the one to change.
10. **Report.** `report_emit` (`src/report.cpp:152`) symbolizes (`addr2line`, `nm`), prints the
    synopsis-format report with the Coffman block (`report.cpp:76`) and stores the finding.
11. **Back in the wrapper**, `pthread_mutex_lock` returns 0 to the program.

If A had been busy, step 3 would go to the slow path instead: `rec_before_block`
(`recorder.cpp:161`) does the relock check and the edges, captures the stack and registers the
wait-for edge "Q waits for A"; then the real `pthread_mutex_lock` blocks; then `rec_after_block`
(`recorder.cpp:200`) removes the wait-for edge and records the acquisition with its wait time.
If Q never wakes up, the watchdog (`src/waitfor.cpp:82`) finds Q→A→P→B→Q and prints LIVE DEADLOCK.

`pthread_mutex_unlock` (`interpose.cpp:106`) calls `rec_release` (`recorder.cpp:214`): if A is not
in Q's held set it reports a bad unlock; otherwise it records the hold time and removes A. Then it
calls the real unlock.

## 3. OS concept → where it is in the code

| OS concept (syllabus) | Where in LockSpy |
|---|---|
| Linux shell, `LD_PRELOAD` | `bin/lockspy` sets `LD_PRELOAD=build/libLockSpy.so` and the `LOCKSPY_*` variables |
| Dynamic linking, symbol interposition | `src/interpose.cpp`: same-named wrappers; `resolve_all()` uses `dlsym(RTLD_NEXT)` / `dlvsym` |
| Library constructor / destructor | `lockspy_init` / `lockspy_fini` in `interpose.cpp` |
| Processes, `fork`, process creation | `pthread_atfork` handlers in `recorder.cpp`; `posix_spawnp` of `addr2line`/`nm` in `symbolize.cpp` |
| Threads (pthread), thread IDs | `pthread_create` trampoline, `ThreadState`, `gettid` in `recorder.cpp` |
| Thread-local storage | `t_in_spy`, `t_self` (`__thread`, initial-exec) |
| `/proc` | thread names from `/proc/self/task/<tid>/comm`; program path from `/proc/self/exe` |
| Mutex | intercepted lock/trylock/timedlock/unlock/init/destroy; LockSpy's own `SpyLock` |
| Monitor / condition variable | `pthread_cond_wait` wrapper releases and re-adds the mutex (`rec_cond_*`) |
| Semaphore | `sem_wait/trywait/timedwait/post` wrappers, profiling only |
| Classical problems | `tests/*/philosophers_*`, `producer_consumer_*`, `readers_writers_*` |
| Four deadlock conditions | `coffman()` in `report.cpp` prints evidence for each |
| Resource-allocation / wait-for graph | `waitfor.cpp` (live), `lock_graph.cpp` (lock order) |
| Deadlock detection (DFS) | `find_paths` (lock order), colour DFS in `wf_check` (live) |
| Deadlock prevention by ordering | `lg_suggest` + Tarjan `lg_components` in `lock_graph.cpp` |
| Deadlock recovery | `LOCKSPY_ON_DEADLOCK=abort` (terminate the process after reporting) |

## 4. Likely examiner questions

1. **Why `LD_PRELOAD` and not `ptrace` or recompiling?** `LD_PRELOAD` works on unmodified binaries
   and costs one extra function call per lock operation. `ptrace` stops the process on every event
   (two context switches each time), far too slow for lock calls. Recompiling (like
   ThreadSanitizer) needs the source and a rebuild.
2. **How do you call the real `pthread_mutex_lock`?** `dlsym(RTLD_NEXT, "pthread_mutex_lock")`
   returns the next definition after our library in the search order, which is libc's.
3. **Why can a run that never hung still be reported?** The lock-order graph remembers that some
   thread took A then B, and some other thread took B then A, even if at different times. Another
   interleaving of the same code would deadlock. That is a POTENTIAL DEADLOCK. LIVE DEADLOCK is
   reported only when threads are actually stuck.
4. **What is a gate lock?** A third lock G that both threads hold while taking A and B in opposite
   orders. Only one thread can hold G, so the two critical sections never overlap and the A/B cycle
   cannot deadlock. LockSpy stores the other held locks for every edge and suppresses such cycles.
5. **Why does a trylock add no edge?** A trylock never blocks. Hold-and-wait needs a thread to
   *wait*; with trylock it gets EBUSY and can back off. Linux lockdep uses the same rule.
6. **Why must `pthread_cond_wait` be handled specially?** It releases the mutex while sleeping and
   re-acquires it afterwards, inside libc where we cannot see it. If we did not mirror that, the
   sleeping thread would look like the owner of the mutex: wrong hold times and wrong wait-for edges.
7. **How do you stop LockSpy from intercepting itself?** A thread-local flag set for the whole
   wrapper. LockSpy's own locking, `malloc`, file I/O… may call pthread functions; while the flag is
   set those calls go straight to libc. Our private lock calls the real functions directly.
8. **What are the four Coffman conditions and how do you check them?** Mutual exclusion (mutexes
   have one owner), hold and wait (the thread held H while requesting L: that is what an edge is), no
   preemption (pthread locks are never taken away), circular wait (the cycle). Each report prints the
   evidence for each condition.
9. **How is the suggested lock order computed?** Tarjan's algorithm finds the strongly connected
   components of the cycle edges; inside each we order locks greedily (no incoming edge first, else
   most outgoing minus incoming). Edges pointing backwards are the call sites to change. It is a
   heuristic for the minimum feedback arc set, which is NP-hard.
10. **How does the watchdog detect a live deadlock?** Every 500 ms it takes threads blocked longer
    than that, follows thread → lock it waits for → owner thread, and runs a DFS with colours; a
    back edge to a thread on the current path closes a cycle.
11. **What is the overhead and where does it come from?** Measured (`results/RESULTS.md`): about
    185 ns per uncontended lock+unlock pair, 3.1× geometric-mean slowdown over 6 programs
    (ThreadSanitizer 4.3×, Helgrind 122.5×; TSan was faster on the tight uncontended loop). It comes from: one extra trylock call, one global private lock per event (all threads
    serialise on it), hash-map lookups for the lock and the edges, and two `clock_gettime` calls for
    wait/hold time. Stacks are captured only for new edges, blocked threads and misuse.
12. **Why is the watchdog not reported as a thread?** It is created with the real `pthread_create`
    and its re-entrancy flag is always set, so its calls never enter the analysis.
13. **What happens on relock of a normal mutex?** We report RELOCK first (with the stack), then call
    the real function anyway, as the synopsis says. A normal mutex then waits for itself forever; the
    watchdog sees the one-thread cycle and reports a LIVE DEADLOCK "predicted" by the relock report.
14. **What can LockSpy NOT detect?** Deadlocks through semaphores (no owner), spinlocks, lock-free
    code, locks taken inside libc, statically linked programs, and lock-order bugs on code paths the
    run did not execute. It is a dynamic tool: it sees what ran.
15. **How do you know it works?** `make test` runs 18 cases (buggy, correct, tricky, perf) and
    compares the finding types with `tests/expected.csv`; `bench/run_bench.sh` compares LockSpy with
    ThreadSanitizer and Helgrind and computes precision and recall. `make memcheck` runs LockSpy
    itself under Valgrind.

## 5. Five-minute live demo script

(On macOS the commands below run themselves inside the Ubuntu container through `bin/in-docker`;
Docker must be running, e.g. `colima start`. The `xdg-open` step is Linux; on a Mac use `open`.)

```bash
make                                                    # 1. build the library and the tests
./build/tests/buggy/ab_ba                               # 2. bare run: works, looks fine
./bin/lockspy run -- ./build/tests/buggy/ab_ba          # 3. POTENTIAL DEADLOCK + fix + Coffman
./bin/lockspy run -- ./build/tests/buggy/ab_ba --hang   # 4. LIVE DEADLOCK, "predicted at t=..." (Ctrl-C to stop)
./bin/lockspy run -- ./build/tests/tricky/gate_lock_safe          # 5. silent: suppressed (gate lock 1)
./bin/lockspy run --naive -- ./build/tests/tricky/gate_lock_safe  #    the naive detector raises a false alarm
./bin/lockspy run -- ./build/tests/perf/contention_heavy          # 6. contention ranking hot > warm > cold
make demo && xdg-open results/demo/lockspy_report.html            # 7. heat-map graph + HTML report
make test                                                         # 8. the whole suite: 18/18 PASS
```
