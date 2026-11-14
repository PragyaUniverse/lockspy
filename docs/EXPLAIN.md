# Viva notes

## Explaining the project in 2 minutes

When threads lock the same mutexes in different orders, they can deadlock, but only if the timing
is bad. So the bug can pass every test and show up later.

LockSpy is a shared library. We load it into a normal, unmodified program using `LD_PRELOAD`.
Because of that, when the program calls `pthread_mutex_lock`, it actually calls our function
first. We note down which thread took which lock and from where, then call the real
`pthread_mutex_lock` from libc (we get it with `dlsym(RTLD_NEXT)`).

For every thread we keep the set of locks it currently holds. If a thread asks for lock L while
it holds H, we add an edge H → L to a graph. If that graph gets a cycle, two parts of the code
disagree about the order, so we report a POTENTIAL DEADLOCK, even if nothing hung this time.

There's also a watchdog thread. Every half second it checks which threads are stuck waiting for a
lock and who owns that lock. If that forms a cycle, the program is deadlocked right now and we
report a LIVE DEADLOCK.

On top of that: we skip cycles that can't really happen (both threads held a common lock first,
only one thread involved, or trylock), we print the four Coffman conditions for each report, we
suggest a lock order that fixes it, and we show which locks threads wait on the most.

## What happens on one pthread_mutex_lock call

Example: in `tests/buggy/ab_ba.cpp`, thread Q holds `B` and calls `pthread_mutex_lock(&A)`.

1. Because of `LD_PRELOAD`, the call goes to our `pthread_mutex_lock` in `src/interpose.cpp:64`.
2. `Guard g` (`src/recorder.h:50`) sets a thread-local flag. If the flag was already set, it means
   LockSpy itself is locking something, so we just call the real function and return.
3. We call the real `pthread_mutex_trylock` first (`interpose.cpp:69`). A is free, so it succeeds
   and we call `rec_acquired` (`src/recorder.cpp:137`).
4. `rec_acquired` locks our own `State::mu` and calls `lg_note_edges` (`src/lock_graph.cpp:14`).
   Q holds B, so it adds the edge B → A, remembering that Q took it. The edge is new, so it gets
   checked for cycles.
5. A goes into Q's held set, and `prof_acquired` (`src/profiler.cpp:8`) counts it.
6. `finish` (`recorder.cpp:118`) unlocks `State::mu`, takes a stack trace, locks again and runs the
   cycle check.
7. `check_edge` (`lock_graph.cpp:96`) looks for a path from A back to B. `find_paths`
   (`lock_graph.cpp:51`) finds A → B, which thread P added earlier. So A → B → A is a cycle.
8. `choose` (`lock_graph.cpp:73`) checks it's real: two different threads (P and Q), no common
   lock held by both, not a reader waiting for a reader.
9. `lg_suggest` (`lock_graph.cpp:168`) works out the order A < B, so B → A (Q's code) is the one
   to change.
10. `report_emit` (`src/report.cpp:147`) turns addresses into names and prints the report,
    including the Coffman conditions (`report.cpp:71`).
11. Our `pthread_mutex_lock` returns 0 to the program.

If A had been locked by someone else, the trylock would fail and we'd go the slow way:
`rec_before_block` (`recorder.cpp:152`) adds the edges, takes a stack and marks "Q is waiting for
A", then we call the real (blocking) lock, then `rec_after_block` (`recorder.cpp:191`) clears the
waiting mark and records how long Q waited. If Q never gets A, the watchdog (`src/waitfor.cpp:75`)
finds the cycle Q → A → P → B → Q.

For `pthread_mutex_unlock` (`interpose.cpp:98`), `rec_release` (`recorder.cpp:205`) checks that the
thread actually holds the lock (otherwise it's a bad unlock), records the hold time and removes it
from the held set, and then we call the real unlock.

## OS concepts and where they are

| Concept | Where |
|---|---|
| Shell, `LD_PRELOAD` | `bin/lockspy` sets `LD_PRELOAD` and the `LOCKSPY_*` options |
| Dynamic linking | `interpose.cpp`: wrappers with the same names, `resolve_all()` uses `dlsym(RTLD_NEXT)` |
| Library constructor/destructor | `lockspy_init` / `lockspy_fini` in `interpose.cpp` |
| Processes, `fork` | `pthread_atfork` handlers in `recorder.cpp`; `posix_spawnp` in `symbolize.cpp` |
| Threads | `pthread_create` wrapper, `ThreadState`, `gettid` |
| Thread-local storage | `t_in_spy`, `t_self` |
| `/proc` | thread names from `/proc/self/task/<tid>/comm`, program path from `/proc/self/exe` |
| Mutex, monitor, semaphore | the wrappers in `interpose.cpp`; `rec_cond_*` for condition variables |
| Classical problems | `tests/*/philosophers_*`, `producer_consumer_*`, `readers_writers_*` |
| Coffman conditions | `coffman()` in `report.cpp` |
| Wait-for graph | `waitfor.cpp` (live), `lock_graph.cpp` (lock order) |
| Cycle detection | `find_paths` (DFS), colour DFS in `wf_check` |
| Prevention by ordering | `lg_suggest` and `lg_components` (Tarjan) in `lock_graph.cpp` |
| Recovery | `LOCKSPY_ON_DEADLOCK=abort` |

## Questions they might ask

**Why LD_PRELOAD and not ptrace or recompiling?**
It works on any dynamically linked program and only adds a function call per lock. ptrace stops
the process on every event, which is way too slow for locks. Recompiling (what ThreadSanitizer
does) needs the source.

**How do you call the real function?**
`dlsym(RTLD_NEXT, "pthread_mutex_lock")` gives the next definition after our library, which is
libc's.

**How can you report a deadlock if the program didn't hang?**
The graph remembers that one thread took A then B and another took B then A, even at different
times. With different timing they would deadlock. That's a potential deadlock. A live deadlock is
only reported when threads are really stuck.

**What's a gate lock?**
A third lock G that both threads take before A and B. Only one of them can hold G, so they can never
be in the A/B part at the same time, and the cycle can't deadlock. We store which other locks were
held for every edge, so we can skip these.

**Why doesn't trylock add an edge?**
It never waits. If the lock is busy it just returns EBUSY. No waiting means no hold-and-wait.
Linux lockdep does the same.

**Why is pthread_cond_wait special?**
It unlocks the mutex while sleeping and locks it again inside libc, where we can't see it. If we
didn't handle it, the sleeping thread would look like it still owns the mutex.

**How do you avoid LockSpy intercepting its own locks?**
A thread-local flag is set while we're inside a wrapper. Any pthread call made while it's set goes
straight to libc. Our own internal lock also calls libc directly.

**The four Coffman conditions?**
Mutual exclusion (a mutex has one owner), hold and wait (the thread held H while asking for L, that's
exactly what an edge is), no preemption (nobody can take a mutex away), circular wait (the cycle).

**How do you pick the suggested order?**
Tarjan's algorithm finds the strongly connected components. Inside each, we pick locks greedily
(no incoming edges first, otherwise most outgoing minus incoming). Edges that go backwards are the
lines to change. The perfect answer is NP-hard (minimum feedback arc set), so this is a heuristic.

**How does the watchdog find a live deadlock?**
Every 500 ms it takes the threads blocked longer than that, follows thread → lock it waits for →
owner, and does a DFS. Reaching a thread that's already on the current path means a cycle.

**What's the overhead?**
About 185 ns per lock/unlock when there's no contention, and 3.1x slowdown on average over our 6
programs (ThreadSanitizer 4.3x, Helgrind 122.5x, see `results/RESULTS.md`). It comes from the extra
trylock, our one internal lock that every thread goes through, a couple of hash lookups and two
clock reads. ThreadSanitizer was faster than us on the tight uncontended loop.

**Why isn't the watchdog shown as a thread?**
It's created with the real `pthread_create` and its flag is always set, so we never record it.

**What happens on a relock of a normal mutex?**
We print RELOCK first, then call the real lock anyway (like the synopsis says). The thread then
waits for itself forever, and the watchdog reports a live deadlock that the relock "predicted".

**What can't LockSpy detect?**
Deadlocks through semaphores (no owner), spinlocks, lock-free code, libc's internal locks,
static binaries, and lock orders that never ran.

**How do you know it works?**
`make test` runs 18 cases and compares with `tests/expected.csv`. `make bench` compares with
ThreadSanitizer and Helgrind. `make memcheck` runs LockSpy under Valgrind.

## Demo plan (about 5 minutes)

On a Mac these run inside Docker automatically (start it with `colima start` first). Use `open`
instead of `xdg-open` on a Mac.

```bash
make                                                     # build
./build/tests/buggy/ab_ba                                # normal run, looks fine
./bin/lockspy run -- ./build/tests/buggy/ab_ba           # potential deadlock + fix
./bin/lockspy run -- ./build/tests/buggy/ab_ba --hang    # live deadlock, "predicted at..."
./bin/lockspy run -- ./build/tests/tricky/gate_lock_safe          # nothing reported
./bin/lockspy run --naive -- ./build/tests/tricky/gate_lock_safe  # without filters: false alarm
./bin/lockspy run -- ./build/tests/perf/contention_heavy          # contention table
make test                                                         # 18/18
make ui                                                           # web UI, http://localhost:8080
```
