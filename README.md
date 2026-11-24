# LockSpy

LockSpy finds deadlocks and lock contention in multithreaded C/C++ programs on Linux. You don't
need to change or recompile the program. LockSpy is a shared library that we load with
`LD_PRELOAD`, so it sees every `pthread_mutex_lock`/`unlock` (and rwlocks, condition variables
and semaphores) before libc does.

It reports:

- **potential deadlocks**: two threads take the same locks in opposite orders (even if the run didn't hang)
- **live deadlocks**: threads that are actually stuck waiting for each other
- **lock misuse**: locking a mutex twice, unlocking a mutex you don't hold, a thread exiting while holding a lock
- **contention**: which locks threads spend the most time waiting for

For each deadlock it also suggests a lock order that fixes it.

This is our OS Lab project (24B11CS312), B.Tech 3rd year, JIIT, 2026-27.
Team: Prakhar Agnihotri, Shreesh Goyal, Unnati Joshi, Pragya Sumbly.

New to the project? Start with [docs/INTRO.md](docs/INTRO.md), a plain-language introduction.

## Running it

You need Linux (Ubuntu) with `g++`, `make` and `binutils`. `graphviz`, `valgrind` and `python3`
are optional.

```bash
make                                              # builds build/libLockSpy.so and the test programs
./bin/lockspy run -- ./build/tests/buggy/ab_ba    # run a program under LockSpy
make test                                         # run all 18 test cases
make ui                                           # web UI at http://localhost:8080
```

Other targets: `make demo` (writes JSON/DOT/HTML reports to `results/demo/`), `make bench`
(compares with ThreadSanitizer and Helgrind), `make memcheck` (runs LockSpy under Valgrind).

**On a Mac:** LockSpy can't run on macOS directly, so we use a Docker container. If Docker is
running (`colima start`), the commands above work as-is: `make` and `bin/lockspy` run themselves
inside the container. `bin/in-docker bash` gives you a shell in it.

To use it on your own program, compile with `-g` (for file and line numbers) and run:

```bash
./bin/lockspy run -- ./your_program args
```

Options: `--format text,json,dot,html`, `--outdir DIR`, `--watchdog MS` (default 500, 0 turns it
off), `--on-deadlock abort`, `--naive` (no false-positive filtering), `--trace`, `--quiet`. Each one
is also an environment variable (`LOCKSPY_FORMAT`, `LOCKSPY_OUTDIR`, ...), see `src/config.h`.

## Example

`tests/buggy/ab_ba.cpp` has two threads: P locks A then B, Q locks B then A. The program finishes
normally, but LockSpy still catches it:

```
[LockSpy] POTENTIAL DEADLOCK: lock-order cycle detected  (#1, t=0.000s)
  Lock A -> Lock B   taken by thread P in worker_p() (ab_ba.cpp:21)
  Lock B -> Lock A   taken by thread Q in worker_q() (ab_ba.cpp:32)
  Suggested fix: acquire A before B in every thread.
    change worker_q() (ab_ba.cpp:32): it takes A while holding B
  Coffman conditions (a deadlock needs all four):
    ✔ Mutual exclusion  A, B are mutexes, only one owner at a time
    ✔ Hold and wait     P holds A while requesting B; Q holds B while requesting A
    ✔ No preemption     a pthread lock is never taken away; only its owner can unlock it
    ✔ Circular wait     P -> B -> Q -> A -> P   (possible; it did not have to happen in this run)
```

With `--hang` the threads are forced to overlap and the program really deadlocks. The watchdog
thread notices and says the hang matches the earlier warning:

```
[LockSpy] LIVE DEADLOCK: 2 threads are blocked in a wait-for cycle  (#2, t=1.002s)
  thread P holds Lock A and waits for Lock B (owner: thread Q), blocked 1.00s in worker_p() (ab_ba.cpp:21)
  thread Q holds Lock B and waits for Lock A (owner: thread P), blocked 1.00s in worker_q() (ab_ba.cpp:32)
  This hang was predicted at t=0.000s (finding #1).
```

## Web UI

`make ui` starts a small Python server (standard library only) and a page at
http://localhost:8080. Pick a test program, click run, and you get the findings, a drawing of the
lock graph (cycle in red), a contention table and the source code with the reported lines
highlighted. "Run all tests" runs the whole suite.

"Write your own code" opens an editor: type or paste a C/C++ pthread program and press Check code.
The server compiles it with the same flags as the tests (`g++ -g -O0 -pthread -rdynamic`), runs it
under LockSpy (the watchdog stops it if it freezes) and shows the results the same way, with red
marks next to the lines LockSpy points at. There are three examples to start from: the AB-BA bug, a
real freeze, and the fixed version.

## How it works

1. `interpose.cpp`: our wrappers have the same names as the pthread functions. The dynamic linker
   picks ours first because of `LD_PRELOAD`, and we call the real one with `dlsym(RTLD_NEXT, ...)`.
2. `recorder.cpp`: keeps the set of locks each thread holds and checks for misuse.
3. `lock_graph.cpp`: when a thread asks for lock L while holding H, we add an edge H → L. A cycle
   (found with DFS) means a potential deadlock. Before reporting, we drop cycles that can't really
   happen: both threads held a common "gate" lock, only one thread was involved, or it was a trylock.
   The suggested order comes from Tarjan's SCC algorithm plus a greedy ordering.
4. `waitfor.cpp`: a watchdog thread checks every 500 ms which threads are blocked on which lock,
   and who owns it. A cycle there is a live deadlock.
5. `profiler.cpp`, `report.cpp`, `symbolize.cpp`: wait/hold times, turning addresses into names
   (`forks[3]`, `worker_q() (ab_ba.cpp:32)`) and writing the reports.

More detail in [docs/DESIGN.md](docs/DESIGN.md).

## Results

We ran our 16 test programs (9 buggy, 7 correct or "tricky but safe") under LockSpy,
ThreadSanitizer and Helgrind. Full numbers are in [results/RESULTS.md](results/RESULTS.md).

| tool | bugs caught (of 9) | false alarms (of 7) | needs recompile |
|---|---|---|---|
| LockSpy | 9 | 0 | no |
| LockSpy with `--naive` | 9 | 3 | no |
| ThreadSanitizer | 6 | 2 | yes |
| Helgrind | 8 | 3 | no |

Average slowdown (geometric mean of 6 programs): LockSpy 3.1x, ThreadSanitizer 4.3x, Helgrind
122.5x. That's about 185 ns extra per lock/unlock. ThreadSanitizer was faster than us on one
program, a tight lock/unlock loop in 4 threads (2.6x vs 6.8x), because all our bookkeeping goes
through one internal lock.

These are small test programs we wrote ourselves, so take the numbers as a sanity check, not a
real-world benchmark.

## Limitations

- Only pthread locks. `std::mutex` works because it uses pthread underneath, but
  `condition_variable::wait_for`/`wait_until` and `pthread_mutex_clocklock` aren't intercepted yet.
- Semaphores have no owner, so they're only profiled, not part of deadlock detection.
- We only see what the program actually ran. A lock order that never executed won't be reported.
- Statically linked programs, spinlocks and lock-free code aren't covered.

## Course topics used

| Module | Where |
|---|---|
| I: Linux | `LD_PRELOAD`, shell scripts, `/proc/self/task/<tid>/comm` for thread names |
| II: Processes | dynamic linking, `dlsym`, `fork` handling, running `addr2line` with `posix_spawn` |
| III: Threads | per-thread lock sets, thread-local storage, the watchdog thread |
| V: Synchronization | mutex, rwlock, condition variables, semaphores; philosophers, producer-consumer, readers-writers tests |
| VI: Deadlocks | Coffman conditions, wait-for graph, cycle detection, prevention by lock ordering |

## Other docs

- [docs/TEAM_GUIDE.md](docs/TEAM_GUIDE.md): the whole project on one page, for the team
- [docs/CODE_GUIDE.md](docs/CODE_GUIDE.md): every file and function explained in basic language
- [docs/INTRO.md](docs/INTRO.md): simple introduction, start here
- [docs/EXPLAIN.md](docs/EXPLAIN.md): notes for the viva
- [docs/DESIGN.md](docs/DESIGN.md): design decisions and problems we ran into
- [docs/RESUME.md](docs/RESUME.md): resume points
