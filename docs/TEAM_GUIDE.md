# LockSpy: team guide

A short, plain guide so everyone on the team (Prakhar, Shreesh, Unnati, Pragya) understands the
whole project: what it is, how to run it, what is in each folder, and what to say about it.

For how the code works, file by file, read [CODE_GUIDE.md](CODE_GUIDE.md).

---

## 1. The project in one paragraph

Programs that run many **threads** at once use **locks** (mutexes) so two threads don't change the
same data at the same time. If two threads take two locks in opposite orders, they can end up
waiting for each other forever. That is a **deadlock**: the program freezes. It only happens
with unlucky timing, so it can hide through every test. **LockSpy** is a tool that watches every
lock a program takes while it runs, and tells you about deadlocks (even ones that *could* happen
but didn't this time), lock mistakes, and slow, crowded locks. You don't change or recompile the
program to use it.

## 2. The example everyone should know

```
Thread P:  lock(A)  then  lock(B)
Thread Q:  lock(B)  then  lock(A)
```

If P holds A and Q holds B at the same moment, P waits for B and Q waits for A, so both wait
forever. LockSpy notices that **A before B** and **B before A** both happened. That's a circle
(A → B → A), so it warns you, even if this run was lucky and didn't freeze.

**The fix:** every thread takes the locks in the same order (A first, then B).

## 3. What LockSpy reports

| Report | In simple words |
|---|---|
| POTENTIAL DEADLOCK | "Locks were taken in opposite orders. It can freeze, even if it didn't now." |
| LIVE DEADLOCK | "It's frozen right now. These threads are waiting for each other." |
| RELOCK | A thread locked a lock it already holds (it will wait for itself). |
| BAD UNLOCK | A thread unlocked a lock it doesn't hold. |
| HELD AT EXIT | A thread finished but forgot to unlock. |
| Contention table | Which locks threads waited on the most (these slow the program down). |

For each deadlock it also suggests a **lock order that fixes it**, and checks the four
**Coffman conditions** (the four things a deadlock needs).

## 4. How it works (no code)

1. **It sneaks in front of the lock functions.** We load LockSpy into the program with a Linux
   feature called `LD_PRELOAD`. So when the program calls `pthread_mutex_lock`, it calls *our*
   version first. We write down what happened and then call the real one.
2. **It remembers the order of locks.** If a thread holds lock A and asks for lock B, we draw an
   arrow A → B. Arrows that form a circle mean a potential deadlock.
3. **It skips false alarms.** Some circles can't really freeze, for example when both threads first
   take a common "gate" lock, when only one thread was involved, or when `trylock` was used (it
   never waits). LockSpy filters these out. `--naive` turns the filter off to show the difference.
4. **A watchdog checks for real freezes.** Every half second a background thread checks who is
   stuck waiting for which lock, and who owns that lock. A circle there is a live deadlock.
5. **It writes a report**: text on screen, plus JSON, a graph picture (DOT) and HTML if you ask.

## 5. How to run it

LockSpy needs Linux. On a Mac we run it inside a Docker container automatically. You only need to
start Docker first.

On a Mac, start Docker once per session:

```bash
colima start
```

Build everything:

```bash
make
```

Run the whole test suite (should say 18 passed):

```bash
make test
```

Open the website at http://localhost:8080 (press Ctrl-C in that terminal to stop it):

```bash
make ui
```

Run one program under LockSpy:

```bash
./bin/lockspy run -- ./build/tests/buggy/ab_ba
```

The same program, but forced to really freeze:

```bash
./bin/lockspy run -- ./build/tests/buggy/ab_ba --hang
```

When you're done on a Mac, stop Docker:

```bash
colima stop
```

## 6. The website (`make ui`)

- **Left side:** the list of test programs. Click one, then **Run**.
- **Right side:** tabs for the **Report** (what was found), **Graph** (the lock arrows, with the
  circle in red), **Contention** (which locks were busiest) and **Code** (the program, with the
  bad lines marked).
- **Run all tests:** runs every program and shows pass or fail.
- **Write your own code:** type or paste a C/C++ program and press **Check code**. The server
  compiles it, runs it under LockSpy and shows red dots next to the problem lines.
- **Learn page:** a 13-step animated lesson on how a deadlock happens and how LockSpy catches it.

## 7. What's in each folder

| Folder | What's in it |
|---|---|
| `src/` | The detector itself (C++). This becomes `build/libLockSpy.so`. |
| `bin/` | Two small scripts: `lockspy` (run a program under LockSpy) and `in-docker` (run a command in the Linux container on a Mac). |
| `tests/` | 16 small test programs plus the scripts that check LockSpy finds the right things. |
| `ui/` | The website: a Python server and the HTML/CSS/JS pages. |
| `bench/` | Scripts that compare LockSpy with ThreadSanitizer and Helgrind. |
| `results/` | Benchmark results (`RESULTS.md` is the readable one). |
| `docs/` | All the write-ups, including this one. |
| `docker/` | The recipe for the Linux container. |
| `build/` | Created by `make`. Not saved in git. |

## 8. The test programs

| Group | Programs | LockSpy should... |
|---|---|---|
| `buggy/` | ab_ba, philosophers_naive, producer_consumer_bad, readers_writers_bad, recursive_lock, bad_unlock, lock_held_at_exit | find the bug |
| `correct/` | philosophers_ordered, producer_consumer_ok, readers_writers_ok, consistent_order | say nothing |
| `tricky/` | gate_lock_safe, trylock_backoff, single_thread_both_orders | say nothing (these look buggy but are safe, so they catch false alarms) |
| `perf/` | contention_heavy, uncontended_loop | measure speed and the contention table |

These cover the classic OS lab problems: dining philosophers, producer-consumer and
readers-writers, each in a buggy and a fixed version.

## 9. Our results (the numbers to remember)

| Tool | Bugs caught (of 9) | False alarms (of 7) | Needs recompiling? |
|---|---|---|---|
| **LockSpy** | **9** | **0** | no |
| LockSpy `--naive` | 9 | 3 | no |
| ThreadSanitizer | 6 | 2 | yes |
| Helgrind | 8 | 3 | no |

- **Speed:** LockSpy makes programs about **3.1× slower** on average. ThreadSanitizer is 4.3× and
  Helgrind 122.5×.
- **Honest note:** these are our own small test programs, so the numbers are a sanity check, not
  a big real-world study.

## 10. What it can't do

- Only pthread locks (C++ `std::mutex` works because it uses pthread underneath).
- Semaphores have no owner, so they're only timed, not checked for deadlocks.
- It only sees code that actually ran. A lock order that never ran won't be reported.
- No statically linked programs, spinlocks or lock-free code.

## 11. OS syllabus topics it uses

| Module | Where it shows up |
|---|---|
| Linux | `LD_PRELOAD`, shell scripts, `/proc` for thread names |
| Processes | dynamic linking, `fork` handling, starting `addr2line` as a child process |
| Threads | per-thread lock lists, thread-local storage, the watchdog thread |
| Synchronization | mutex, rwlock, condition variables, semaphores, and the classic problems |
| Deadlocks | Coffman conditions, wait-for graph, cycle detection, prevention by lock ordering |

## 12. Other docs

- [INTRO.md](INTRO.md): a longer simple introduction
- [CODE_GUIDE.md](CODE_GUIDE.md): every file explained, in basic language
- [EXPLAIN.md](EXPLAIN.md): viva notes and likely questions
- [DESIGN.md](DESIGN.md): why we built it this way
- [../results/RESULTS.md](../results/RESULTS.md): full benchmark numbers
