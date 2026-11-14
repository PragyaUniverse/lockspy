# LockSpy: a simple introduction

Read this first if the rest of the project feels complicated. No code knowledge needed for the
first half.

## 1. The problem

Programs often do several things at once using **threads**. When two threads share data (say, a
bank balance), they use a **lock** (a *mutex*) so only one of them touches the data at a time:

```
lock(account)      <- if another thread holds it, wait here
balance = balance - 10
unlock(account)
```

Now imagine a transfer that needs **two** accounts, A and B:

- Thread P locks A, then B (money A → B)
- Thread Q locks B, then A (money B → A)

If P grabs A and at the same moment Q grabs B, then P waits for B (Q has it) and Q waits for A
(P has it). Both wait forever. That is a **deadlock**: the program just freezes.

It's like two people in a narrow corridor, each waiting for the other to step back.

The nasty part: it only happens when the timing is unlucky. The program can pass 1000 tests and
then freeze once in production.

## 2. What LockSpy does

LockSpy watches a program's locks while it runs and tells you:

| It reports | Meaning |
|---|---|
| **Potential deadlock** | "These threads take locks in opposite orders. It didn't freeze this time, but it can." |
| **Live deadlock** | "The program is frozen right now. These threads are waiting for each other." |
| **Misuse** | Locking the same lock twice, unlocking a lock you don't hold, a thread ending while still holding a lock. |
| **Contention** | Which locks threads spend the most time waiting for (they slow the program down). |

And for each deadlock it tells you **how to fix it**: "always take A before B".

The best part: you don't change or recompile the program. You just run it through LockSpy.

## 3. How it works, in 5 steps

**Step 1: get in the middle.** Every program calls `pthread_mutex_lock` from the system
library (libc) to take a lock. Linux has a feature called `LD_PRELOAD`: it loads our library
*before* libc. Our library also has a function called `pthread_mutex_lock`, so the program calls
ours. We write down what happened, then pass the call on to the real one.

```
program  ->  LockSpy's pthread_mutex_lock  ->  real pthread_mutex_lock (libc)
                 (writes it down)
```

**Step 2: remember who holds what.** For every thread we keep a list of the locks it holds right
now. P: {A}. Q: {B}.

**Step 3: draw the lock-order graph.** Whenever a thread asks for a lock while already holding
another one, we draw an arrow: "P asked for B while holding A" gives **A → B**. Later Q asks for A
while holding B, which gives **B → A**. Now the arrows go in a circle: A → B → A. A circle means the
two threads disagree about the order, so a deadlock is possible. That's a **potential deadlock**,
reported even if nothing froze.

**Step 4: watch for real freezes.** A small helper thread (the *watchdog*) wakes up every half
second and checks: which threads are stuck waiting, for which lock, and who owns that lock?
If P waits for Q and Q waits for P, that's a circle again, so it's a **live deadlock**.

**Step 5: report it clearly.** We turn memory addresses into names you recognise (`Lock A`,
`worker_p() in ab_ba.cpp line 21`), check the four textbook conditions for deadlock, suggest a lock
order, and print it (or show it in the web page).

## 4. The four conditions (from the OS course)

A deadlock can only happen if all four are true. LockSpy prints all four for each report:

1. **Mutual exclusion**: a lock has only one owner at a time.
2. **Hold and wait**: a thread holds one lock while waiting for another.
3. **No preemption**: nobody can take a lock away from its owner.
4. **Circular wait**: the waiting forms a circle (P → B → Q → A → P).

The fix LockSpy suggests breaks number 4: if everyone takes locks in the same order (A before B),
a circle can't form.

## 5. Smart parts (what makes it better than a basic checker)

A basic checker reports every circle. Some circles can never actually freeze, and LockSpy skips them:

- **Gate lock**: both threads first take a third lock G. Only one can hold G, so they never run the
  A/B part at the same time.
- **Single thread**: one thread takes A→B and later B→A. A deadlock needs at least two threads.
- **trylock**: "try to lock, and if it's busy, give up" never waits, so it can't deadlock.

That's why LockSpy had **0 false alarms** on our tests, while a basic checker had 3.

## 6. What's in the project folder

| Folder | What it is |
|---|---|
| `src/` | LockSpy itself (C++). This becomes `build/libLockSpy.so`. |
| `tests/` | Small programs to test it: `buggy/` (must be caught), `correct/` and `tricky/` (must stay quiet), `perf/` (speed). |
| `bin/lockspy` | The command you run: `./bin/lockspy run -- ./program` |
| `ui/` | The web dashboard (`make ui`) |
| `bench/` | Compares LockSpy with ThreadSanitizer and Helgrind |
| `results/` | Measured numbers (`RESULTS.md`) |
| `docs/` | This intro, design notes, viva notes, resume points |

The source files, in the order a lock call passes through them:

| File | Job |
|---|---|
| `interpose.cpp` | the fake `pthread_mutex_lock` etc. (step 1) |
| `recorder.cpp` | who holds what, misuse checks (step 2) |
| `lock_graph.cpp` | the arrows, finding circles, the suggested order (step 3) |
| `waitfor.cpp` | the watchdog (step 4) |
| `profiler.cpp` | wait and hold times |
| `symbolize.cpp`, `report.cpp` | names and the printed reports (step 5) |
| `config.cpp` | settings (`LOCKSPY_*` environment variables) |

## 7. Try it

```bash
make ui
```

Open http://localhost:8080. Click **ab_ba** in the list: you'll see the A ⇄ B circle in red and the
suggested fix. Click **philosophers_naive** for the five-philosopher deadlock.

Or in the terminal:

```bash
./build/tests/buggy/ab_ba                              # runs fine, looks OK
./bin/lockspy run -- ./build/tests/buggy/ab_ba         # LockSpy finds the hidden deadlock
./bin/lockspy run -- ./build/tests/buggy/ab_ba --hang  # this time it really freezes; LockSpy catches it
```

## 8. Words you'll hear

| Word | Meaning |
|---|---|
| thread | one line of execution inside a program; programs can run several at once |
| mutex / lock | lets only one thread into a piece of code at a time |
| deadlock | threads waiting for each other forever |
| contention | threads waiting because a lock is busy |
| `LD_PRELOAD` | Linux setting that loads a library before all others |
| libc | the standard system library where `pthread_mutex_lock` really lives |
| lock-order graph | arrows "A taken before B"; a circle means a possible deadlock |
| wait-for graph | arrows "thread waits for thread"; a circle means a deadlock right now |
| watchdog | LockSpy's helper thread that checks for freezes every 500 ms |
| false alarm | a warning about something that can't actually go wrong |

Next: [EXPLAIN.md](EXPLAIN.md) for the viva, [DESIGN.md](DESIGN.md) for the details.
