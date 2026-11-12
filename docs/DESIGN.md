# LockSpy design notes

This file records the design decisions, and the tricky bugs we hit and how we solved them.
For the viva walkthrough see [EXPLAIN.md](EXPLAIN.md).

## 1. Architecture

```
target program ──calls──▶ interpose.cpp (LD_PRELOAD wrappers) ──forwards──▶ real libc pthread
                                  │
                                  ▼
                           recorder.cpp  (threads, held-lock sets, lock table, misuse checks)
                     ┌────────────┼──────────────────┐
                     ▼            ▼                  ▼
              lock_graph.cpp   waitfor.cpp        profiler.cpp
              (order graph,    (wait-for graph,   (acquisitions,
               DFS, filter,     watchdog thread)   wait / hold time)
               SCC, lock order)
                     └────────────┼──────────────────┘
                                  ▼
                 report.cpp + symbolize.cpp  (text, JSON, DOT heat map, HTML)
```

All analysis state lives in one `State` object (`recorder.h`), protected by one private
mutex `State::mu`. One lock is easy to reason about and easy to explain; the cost is that all
threads of the target briefly serialise on it (see the overhead numbers in `results/RESULTS.md`).

## 2. Decisions

**Interception with `LD_PRELOAD`, not `ptrace` or recompiling.** The dynamic linker resolves a
symbol to the first library in its search order that defines it, and `LD_PRELOAD` libraries come
first. So defining `pthread_mutex_lock` in our `.so` is enough; `dlsym(RTLD_NEXT, ...)` returns
the next definition, which is libc's. No source changes, no recompile, and every call costs a
normal function call (`ptrace` would cost two context switches per call). Limitation: calls made
*inside* libc to its own internal locks do not go through the PLT, so we do not see them, and
statically linked programs cannot be preloaded at all.

**Fast path: trylock first.** Every blocking wrapper first calls the real `trylock`. If that
succeeds, the thread never blocked, so no wait-for edge is needed and the bookkeeping is one short
critical section. Only when the lock is busy do we capture a stack, register "thread waits for L"
and call the real blocking function. Semantics are unchanged: a successful trylock is exactly a
lock that did not have to wait.

**Bookkeeping under the lock, slow work outside it.** Updating sets and maps is fast; capturing a
stack (`backtrace`), symbolizing (`addr2line`) and printing are slow. So the recorder notes what
must be done in a `Todo`, releases `State::mu`, does the slow part, and takes the lock again only
to store the result (`finish()` in `recorder.cpp`).

**Raw stacks on the hot path, names only when reporting.** We store return addresses. They are
turned into `function (file:line)` only when a report is printed, by running `addr2line` (for the
program and its libraries) and `nm` (to name global lock variables). The helpers are started with
`posix_spawnp` and an environment *without* `LD_PRELOAD`, so they are not traced themselves.

**Stacks are captured only when they are needed:** for a new lock-order edge (or a new witness of
one), for a thread about to block (the live-deadlock report shows where it is stuck), for misuse
reports, and once per acquisition call site (the "site stack" cache, used to show where a lock
still held at thread exit was taken). An uncontended acquisition at a known site takes no stack.

**trylock adds no lock-order edge** (the same rule Linux lockdep uses). A trylock cannot block,
so it can never be the "wait" in hold-and-wait. It still enters the held set, so locks requested
*after* it get edges from it. `tests/tricky/trylock_backoff.cpp` shows the effect: a naive
detector (`--naive`, where trylock adds edges) reports a cycle there; LockSpy does not.
Timed locks (`pthread_mutex_timedlock`, `pthread_rwlock_timed*lock`) are treated the same way,
because they give up after the timeout and cannot hang forever.

**Condition variables.** `pthread_cond_wait(cv, m)` atomically releases `m`, sleeps, and
re-acquires `m`. We remove `m` from the held set before the real call and add it back afterwards.
Without this, a thread sleeping in `cond_wait` would still look like the owner of `m`: its hold
time would include the sleep, and the wait-for graph would blame the sleeping thread when another
thread blocks on `m`. The sleep is profiled separately per condition variable.

**Semaphores are profiled, not analysed.** A semaphore has no owner, so there is no "lock → owner"
edge to put into a wait-for graph, and no held set. LockSpy counts waits and wait time only. A
deadlock that goes through a semaphore is not detected (stated in the README limitations).

**Read-write locks** are nodes of the lock-order graph like mutexes, with a mode per edge end.
Read after read on the same lock by the same thread is allowed (no relock report). In the
wait-for graph a reader is only blocked by writers. In the cycle filter, a step where a reader
waits for a lock held for reading is impossible, so such cycles are suppressed (`read-read`).

**Mutex type.** `pthread_mutex_init` reads the type from the attribute; for statically
initialised mutexes (`PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP`) we read the kind field glibc keeps
inside `pthread_mutex_t` (`__data.__kind`). Relocking a recursive mutex is never reported.

**Lock identity and destroy.** Locks are keyed by address. `pthread_mutex_destroy` marks the node
dead and removes the address from the live table, so a new lock at the same address (common with
heap memory) gets a fresh node. Dead nodes stay in the history for the final report but the DFS
skips them.

**Edges store witnesses.** Each edge `H→L` keeps up to 8 distinct observations: thread, the modes,
the set of *other* locks held at that moment (the possible gate locks), and the stack. The DFS
runs only when an edge or a witness is new, so a hot loop that repeats the same pattern pays only
for a hash lookup.

**False-positive filter (gate lock / threads / read-read).** For a cycle found by the DFS we try
to choose one witness per edge so that (1) at least two different threads are involved, (2) no
two chosen witnesses hold a common third lock, and (3) no reader waits for a reader. If no choice
works, the cycle cannot deadlock in the way it was observed; it is counted as "suppressed (safe)"
with the reason. This is a backtracking search over at most 8 witnesses per edge, with a step budget.

**Suggested lock order.** The edges of reported cycles are split into strongly connected
components with Tarjan's algorithm. Inside a component we order locks greedily: take a lock with no
incoming edge if there is one, else the lock with the largest (outgoing − incoming) edges; ties go to
the lock created first. Edges that point backwards in that order are the call sites to change.
This is a heuristic for the (NP-hard) minimum feedback arc set; for the classic cases (AB–BA,
dining philosophers) it gives the textbook answer.

**Threads** get an index (`main` = 0, then 1, 2, ... in order of first appearance) and a kernel
thread id (`gettid`). Their name is read from `/proc/self/task/<tid>/comm`; if the program named the
thread (`pthread_setname_np`) we show that name, otherwise `T<n>`. The start routine is known
because `pthread_create` is wrapped: we pass our own `trampoline` that records it, calls it, and
checks the held-lock set when it returns. `pthread_exit` is wrapped for the same check.

**The watchdog** is one detached thread created with the *real* `pthread_create`, with the
re-entrancy flag permanently set, so it never appears in any analysis. Every `LOCKSPY_WATCHDOG_MS`
it builds the thread→thread graph from threads that have been blocked longer than that period
(short waits are normal contention), runs a colour DFS, and reports each new cycle once. With
`LOCKSPY_ON_DEADLOCK=abort` it writes the final reports and calls `abort()`.

**Predict-then-confirm.** When a live cycle uses exactly the same set of locks as an earlier
POTENTIAL DEADLOCK (or the same lock as an earlier RELOCK, for a thread waiting on itself), the
LIVE report says "This hang was predicted at t=…".

**fork.** `pthread_atfork` handlers take LockSpy's two private locks before `fork()` and release
them in the parent. The child (which has only the forking thread) releases them, keeps the lock
table and its own held set, clears the graphs, findings and statistics, and starts a new watchdog.
A child writes its report files with its pid in the name.

## 3. Tricky problems and how they are solved

These are the classic ways an LD_PRELOAD lock tool breaks. Each one is handled in the code; the
comments next to the fix say why. (Items 4 and 6 are hazards we designed around; item 4 only shows
up on x86-64, and our measurements were taken on aarch64, where glibc has a single version.)

1. **Initialisation order.** Our constructor can run before C++ global constructors of other
   files in the same `.so`, and global destructors can run before our destructor writes the report.
   A global `std::map` or `std::string` would then be used before construction or after
   destruction. Fix: all LockSpy state (`State`, `Config`, the symbol caches) is allocated on first
   use and never freed.
2. **Re-entrancy.** LockSpy uses `std::vector`, `malloc`, `fopen`, … which may themselves take
   pthread locks. The `thread_local` flag `t_in_spy` (initial-exec TLS: a fixed offset from the
   thread pointer, no allocation on access) is set for the whole wrapper; any pthread call made
   while it is set goes straight to libc.
3. **Bootstrap.** A library loaded before us may lock a mutex before our constructor has run.
   Every wrapper checks `g_ready`; before that it resolves the real function on demand. While
   `dlsym` itself is running we return success without locking (only one thread exists so early).
   We do *not* need the static fallback allocator that malloc-interposing tools need, because
   LockSpy does not replace `malloc`/`calloc`: when `dlsym` calls `calloc` it reaches the real one.
4. **Versioned `pthread_cond_wait`.** On x86-64, glibc exports two versions of
   `pthread_cond_wait` (GLIBC_2.2.5, the old ABI, and GLIBC_2.3.2). An unversioned `dlsym` returns
   the *old* one, which does not understand condition variables created by current code and hangs.
   We use `dlvsym(..., "GLIBC_2.3.2")` and fall back to `dlsym` on platforms (such as aarch64) that
   have only one version.
5. **`backtrace()` allocates on first use** (it loads `libgcc_s`). We call it once in the
   constructor so that the first real capture inside a wrapper does not load a library.
6. **Tools run from inside the target would be traced.** `popen("addr2line …")` would start
   `/bin/sh` with our `LD_PRELOAD`, and that shell would print its own LockSpy summary. We start helpers with
   `posix_spawnp` and an environment without `LD_PRELOAD`.
7. **Hold time includes reporting.** A POTENTIAL report is printed by the thread that found it,
   before it continues; the time spent symbolizing is counted in the hold time of the locks it holds.
   Only the first occurrence of each cycle is reported, so the effect is one-off.

## 4. Valgrind (`make memcheck`)

All non-hanging test programs run under memcheck with LockSpy preloaded with zero errors and no
"definitely lost" blocks. Two things Valgrind lists that are not bugs:

- *still reachable*: LockSpy's tables, deliberately never freed (see bug 1).
- *possibly lost* (320 bytes): the thread-local storage block of the detached watchdog thread,
  which is still running when the process exits.

## 5. Known limitations

- Only pthread mutexes, rwlocks and condition variables that the program calls through the
  dynamic linker. `std::mutex`, `std::shared_mutex` and `std::condition_variable::wait` work (they
  call pthread). Not covered: `pthread_mutex_clocklock`, `pthread_cond_clockwait` (used by
  `std::condition_variable::wait_for/until` on glibc ≥ 2.30), spinlocks, futex-based or lock-free
  code, and statically linked programs.
- Semaphore deadlocks are not detected (no owner).
- A potential deadlock is reported only if the run executed both lock orders. LockSpy analyses what
  ran; it is not a static analyser.
- The thread filter means a cycle created by one thread alone is suppressed, even though the same
  code run by two threads could deadlock. `--naive` shows those cycles.
- File:line needs debug info (`-g`). Without it, reports fall back to function names, and lock
  names fall back to `mutex@0x…` if the binary is also stripped.
- ThreadSanitizer (used only by the benchmark) needs `setarch -R` on kernels with high ASLR
  entropy; in Docker that needs `--security-opt seccomp=unconfined`.
