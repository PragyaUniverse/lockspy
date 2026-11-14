# Design notes

Why LockSpy is built the way it is, and the problems we hit along the way.

## Overview

```
program --> interpose.cpp (our wrappers) --> real pthread functions in libc
                 |
             recorder.cpp (threads, held locks, misuse checks)
           /       |        \
  lock_graph    waitfor    profiler
           \       |        /
        report.cpp + symbolize.cpp
```

All of LockSpy's data is in one `State` struct (`recorder.h`) protected by one mutex, `State::mu`.
One lock is simple and easy to get right. The downside is that every thread of the program goes
through that lock on every lock call, which is where most of our overhead comes from.

## Decisions

### LD_PRELOAD instead of ptrace or recompiling

The dynamic linker uses the first library that defines a symbol, and `LD_PRELOAD` libraries are
searched first. So if our `.so` defines `pthread_mutex_lock`, the program calls ours, and
`dlsym(RTLD_NEXT, "pthread_mutex_lock")` gives us libc's. `ptrace` would stop the process on every
call (two context switches each time), and recompiling needs the source code.

What we can't see: libc's internal locks (they don't go through the PLT) and statically linked
programs.

### Try the lock first

Every blocking wrapper first calls the real `trylock`. If it works, the thread didn't wait, so we
just record the acquisition. Only when the lock is busy do we take a stack trace, mark "this thread
is waiting for L" for the watchdog, and call the real blocking function. This keeps the common
case cheap.

### Do slow work outside our lock

Updating our maps is fast. Taking a stack trace, running `addr2line` and printing are slow. So
the recorder remembers what still needs doing (a `Todo`), releases `State::mu`, does the slow part,
and locks again only to save the result. See `finish()` in `recorder.cpp`.

### Stack traces

We store raw return addresses and only turn them into `function (file:line)` when printing a
report, using `addr2line`, and `nm` for global variable names. We take a stack only when we need
one: a new lock-order edge, a thread that is about to block, a misuse report, and once per call
site (used for "lock held at thread exit").

### trylock adds no edge

A trylock can't block, so it can't be the "wait" in hold-and-wait. Linux's lockdep uses the same
rule. The lock still goes into the held set, so locks taken after it get edges from it.
`tests/tricky/trylock_backoff.cpp` is the test for this. Timed locks are treated the same way,
since they give up after a timeout.

### Condition variables

`pthread_cond_wait(cv, m)` releases `m` while sleeping and takes it back before returning. We
remove `m` from the thread's held set before calling the real function and add it back after.
Without this the sleeping thread would look like it still owns `m`. The sleep time is counted per
condition variable, separately from lock wait time.

### Semaphores

A semaphore has no owner, so there's no "lock → owner" edge for the wait-for graph. We only count
waits and wait time. Deadlocks through semaphores are not detected.

### Read-write locks

They are nodes in the lock graph like mutexes, but we remember the mode. Taking a read lock twice
in one thread is fine. In the wait-for graph, a reader is only blocked by writers. A cycle that
would need a reader to wait for another reader is dropped.

### Recursive mutexes

`pthread_mutex_init` tells us the type through the attribute. For mutexes created with a static
initializer we read the type glibc stores inside the mutex (`__data.__kind`). Relocking a
recursive mutex isn't an error, so we don't report it.

### Destroyed locks

Locks are identified by address. When a mutex is destroyed we mark its node dead and forget the
address, so a new lock at the same address gets a new node. Dead nodes stay in the final report but
the cycle search skips them.

### Filtering false positives

For every edge H → L we keep up to 8 "witnesses": which thread took it, in which modes, which other
locks it held at that moment, and the stack. When the DFS finds a cycle, we look for one witness per
edge such that:

1. at least two different threads are involved,
2. no two of them were holding the same other lock (a "gate" lock would stop them overlapping),
3. no reader would have to wait for a reader.

If no such choice exists, the cycle can't actually deadlock, so we count it as "suppressed" instead
of reporting it. The DFS only runs when an edge or witness is new, so a loop doing the same thing
over and over just costs a hash lookup.

### Suggesting a lock order

We split the edges of reported cycles into strongly connected components (Tarjan). Inside each one
we order the locks greedily: first a lock with no incoming edges, otherwise the one with the most
outgoing minus incoming edges, ties going to the lock created first. Edges pointing backwards in that
order are the lines to change. Finding the best order is NP-hard (minimum feedback arc set), but for
AB-BA and the dining philosophers this gives the textbook answer.

### Threads

Each thread gets a number (main is 0) and we record its kernel thread id. Its name comes from
`/proc/self/task/<tid>/comm` if the program set one with `pthread_setname_np`, otherwise we call it
`T1`, `T2`, ... We wrap `pthread_create` so we can wrap the start function: that's how we know
which function a thread started in, and we can check its held locks when it returns.

### Watchdog

One extra thread, created with the real `pthread_create` and always ignored by our wrappers. Every
500 ms (configurable) it looks at threads that have been blocked for longer than that, follows
"waits for lock → owned by thread", and runs a DFS. A cycle is a live deadlock. If it uses the same
locks as an earlier potential deadlock (or the same lock as a relock report), the report says the
hang was predicted. With `LOCKSPY_ON_DEADLOCK=abort` it writes the reports and calls `abort()`.

### fork

`pthread_atfork` handlers hold our locks across `fork()`. In the child we keep the lock table and
the forking thread's held locks, clear everything else, and start a new watchdog. The child writes
its report files with its pid in the name.

## Problems we ran into

1. **Initialisation order.** Our constructor can run before the C++ global constructors of our
   other source files, and their destructors can run before we print the final report. So nothing
   in LockSpy is a global C++ object: `State`, `Config` and the caches are created on first use and
   never freed.
2. **Calling ourselves.** Our own code uses `std::vector`, `malloc`, `fopen`, which can call
   pthread functions, which would land back in our wrappers. A thread-local flag (`t_in_spy`) is
   set while we're inside a wrapper; if it's set, the wrapper just calls the real function.
3. **Locks before we're ready.** Another library can lock a mutex before our constructor runs. The
   wrappers look up the real function on the spot in that case. We don't need a fallback allocator
   (tools that replace `malloc` do) because we don't replace `malloc`.
4. **Two versions of `pthread_cond_wait`.** On x86-64, glibc has an old (GLIBC_2.2.5) and a current
   (GLIBC_2.3.2) version, and plain `dlsym` returns the old one, which hangs with condition
   variables made by current code. We ask for the right version with `dlvsym`. Our benchmark ran on
   aarch64, which only has one version, so we also ran the full test suite on x86-64 Ubuntu
   (emulated): 18/18 passed.
5. **`backtrace()` loads a library the first time.** We call it once at startup so it doesn't
   happen inside a lock call.
6. **Helper programs being traced.** Running `addr2line` through `popen` would start a shell that
   also has our `LD_PRELOAD` and prints its own report. We start helpers with `posix_spawnp` and
   remove `LD_PRELOAD` from their environment.
7. **Hold times include printing.** The thread that finds a potential deadlock prints it before
   continuing, so that time shows up in the hold time of the locks it holds. It only happens once
   per cycle.

## Valgrind

`make memcheck` runs every non-hanging test with LockSpy loaded: 0 errors, nothing "definitely
lost". Valgrind does list LockSpy's tables as "still reachable" (we never free them on purpose) and
320 bytes "possibly lost", which is the watchdog thread's thread-local storage (it's still running
when the program exits).

## Limitations

- Not intercepted: `pthread_mutex_clocklock` and `pthread_cond_clockwait` (used by
  `std::condition_variable::wait_for`/`wait_until`), spinlocks, lock-free code, static binaries.
- No deadlock detection through semaphores.
- Only lock orders that actually ran are seen.
- A cycle made by one thread alone is suppressed, even though the same code in two threads could
  deadlock. `--naive` shows it.
- Without `-g` there are no line numbers, and in a stripped binary locks are named `mutex@0x...`.
- The benchmark runs ThreadSanitizer with `setarch -R` because GCC 13's TSan crashes on this
  kernel otherwise; in Docker that needs `--security-opt seccomp=unconfined`.
