# LockSpy: code guide

Every file in the project, explained in basic language: what it does, the important functions,
and how the pieces connect. Read [TEAM_GUIDE.md](TEAM_GUIDE.md) first if you haven't.

---

## 1. Words you'll see everywhere

| Word | Meaning |
|---|---|
| **thread** | One line of work inside a program. A program can have many running at once. |
| **mutex / lock** | Only one thread can hold it at a time. Others wait. |
| **rwlock** | Read-write lock: many readers at once, or one writer. |
| **condition variable** | Lets a thread sleep until another thread signals it. While sleeping, it gives up its mutex. |
| **semaphore** | A counter. `sem_wait` takes one (waits if zero), `sem_post` gives one back. It has no owner. |
| **LD_PRELOAD** | A Linux setting that loads our library *before* libc, so our functions win when names match. |
| **wrapper** | Our version of a pthread function. It records the event, then calls the real one. |
| **edge H → L** | "A thread asked for lock L while it was holding lock H." |
| **lock-order graph** | All those edges together. A circle in it means a **potential** deadlock. |
| **wait-for graph** | "Thread X is waiting for a lock that thread Y owns." A circle means a **live** deadlock. |
| **finding** | One problem LockSpy reports. |
| **stack / pc** | The chain of function calls, as raw addresses. We turn them into `function() (file.cpp:line)` later. |

## 2. The big picture

```
your program
    │ calls pthread_mutex_lock(&A)
    ▼
interpose.cpp ── our wrapper with the same name (found first because of LD_PRELOAD)
    │
    ├──► recorder.cpp ── who holds what, misuse checks
    │        ├──► lock_graph.cpp ── add edges, find circles  → POTENTIAL DEADLOCK
    │        └──► profiler.cpp   ── wait and hold times      → contention table
    │
    └──► the real pthread_mutex_lock in libc

waitfor.cpp ── watchdog thread, every 500 ms      → LIVE DEADLOCK
report.cpp + symbolize.cpp ── turn numbers into names, print and write reports
config.cpp ── reads the LOCKSPY_* settings
```

All of LockSpy's memory lives in **one** struct, `State` (in `recorder.h`), protected by **one**
private mutex, `State::mu`. That keeps it simple and safe. The cost is that every lock call in the
program goes through that one mutex, which is most of our slowdown.

`make` compiles all 8 files in `src/` into **one** shared library: `build/libLockSpy.so`.

---

## 3. The detector: `src/`

Each `.cpp` has a matching `.h` (header) that lists what it offers to the other files.

### `config.h` / `config.cpp`: settings

A preloaded library can't read command-line arguments, so options come in as environment
variables. `config_load()` reads them once at start-up into a `Config` struct. Everyone else
reads them with `cfg()`.

| Variable | Default | What it does |
|---|---|---|
| `LOCKSPY_FORMAT` | `text` | Which reports to write: `text,json,dot,html` |
| `LOCKSPY_OUTDIR` | `.` | Folder for report files |
| `LOCKSPY_WATCHDOG_MS` | `500` | How often the watchdog checks (0 = off) |
| `LOCKSPY_ON_DEADLOCK` | `report` | `abort` stops the program when a live deadlock is found |
| `LOCKSPY_NAIVE` | off | Turn off the false-alarm filters (for comparison) |
| `LOCKSPY_TRACE` | off | Print every lock event |
| `LOCKSPY_QUIET` | off | Print nothing, only write files |
| `LOCKSPY_STACK_DEPTH` | `8` | How many function calls to remember per event |
| `LOCKSPY_TOP` | `5` | How many locks to show in the contention table |
| `LOCKSPY_NAMES` | none | A file giving friendly names to locks |

### `interpose.cpp`: the entry point (the LD_PRELOAD part)

This is where the program "enters" LockSpy. It defines functions with **exactly the same names**
as the pthread ones: `pthread_mutex_lock`, `pthread_mutex_unlock`, `pthread_mutex_trylock`,
`pthread_mutex_timedlock`, `pthread_mutex_init`/`destroy`, `pthread_cond_wait`/`timedwait`, all
the `pthread_rwlock_*` functions, `sem_wait`/`trywait`/`timedwait`/`post`, `pthread_create` and
`pthread_exit`.

**Key pieces:**

- **`resolve_all()`** finds the real libc functions with `dlsym(RTLD_NEXT, "name")` ("give me the
  *next* definition after mine") and stores them in the `real` struct. There's one special case:
  `pthread_cond_wait` has two versions in glibc, so we ask for the right one with `dlvsym`.
- **`lockspy_init()`** has `__attribute__((constructor))`, so it runs automatically when the
  library loads, before `main()`. It finds the real functions, loads the config, sets up the
  symbolizer and the state, registers `fork` handlers and starts the watchdog.
- **`lockspy_fini()`** has `__attribute__((destructor))`, so it runs when the program ends. It
  stops the watchdog and writes the final report.
- **`Guard`** (defined in `recorder.h`) sets a per-thread flag `t_in_spy` while we're inside a
  wrapper. If LockSpy itself locks something or allocates memory, that call sees the flag and goes
  straight to libc. Without this, LockSpy would spy on itself forever.

**What a lock wrapper does (simplified):**

```
pthread_mutex_lock(m):
    if we're already inside LockSpy → just call the real lock
    try the real trylock first
        worked?  → rec_acquired(...)          fast path: no waiting happened
        busy?    → rec_before_block(...)      "this thread is now waiting for m"
                   real lock (this may wait)
                   rec_after_block(...)       "got it, waited this long"
```

Trying first means the common case (no waiting) stays cheap. Only the waiting case pays for a
stack trace and a "waiting" mark.

- **`trampoline()` + `pthread_create`:** we wrap the thread's start function so LockSpy knows
  when each thread starts and which function it runs.
- **`pthread_cond_wait`:** inside the real call, libc unlocks the mutex while sleeping and locks
  it again on wake-up, where we can't see it. So we tell the recorder "released" before
  (`rec_cond_release`) and "taken again" after (`rec_cond_reacquire`). Otherwise a sleeping thread
  would look like it still owns the mutex.

### `recorder.h` / `recorder.cpp`: the bookkeeper

**The data structures (in `recorder.h`):**

| Struct | What it holds |
|---|---|
| `LockInfo` | One lock: its id, address, kind (mutex/rwlock), who holds it now, and timing stats |
| `ThreadState` | One thread: number, kernel id, name, **list of locks it holds** (`held`), and what it's waiting for (`wait_lock`) |
| `Held` | One entry in a thread's held list: which lock, read or write, how many times, when it was taken |
| `Edge` | One arrow H → L in the lock-order graph, with who took it and when |
| `Witness` | One sighting of an edge: which thread, and which *other* locks it held then (used for the gate-lock filter) |
| `Finding` | One problem to report, with the threads and locks involved and a suggested fix |
| `State` | Everything above, plus `mu`, the one mutex that protects it all |

`SpyLock` is our own private mutex. It calls the real libc functions directly, so it never goes
through our wrappers.

**The functions `interpose.cpp` calls:**

| Function | When it runs | What it does |
|---|---|---|
| `rec_acquired` | A lock was taken without waiting | Adds edges, adds it to the thread's held list, counts it |
| `rec_before_block` | A lock is busy and we're about to wait | Adds edges, marks "thread waits for lock" for the watchdog |
| `rec_after_block` | The wait is over | Clears the waiting mark, records the wait time |
| `rec_release` | Unlock | Checks the thread really held it (else **BAD UNLOCK**), records the hold time, removes it |
| `rec_init` / `rec_destroy` | Lock created or destroyed | Registers or retires the lock |
| `rec_cond_release` / `rec_cond_reacquire` | Around a condition-variable wait | Mirrors libc's hidden unlock and relock |
| `rec_sem_wait` / `rec_sem_post` | Semaphores | Timing only (no owner, so no deadlock check) |
| `rec_thread_start` / `rec_thread_exit` | Thread starts or ends | Registers the thread; at exit, checks it holds nothing (else **HELD AT EXIT**) |
| `rec_atfork_*` | Around `fork()` | Keeps our state safe in the child process |

**Misuse checks** happen here too. Locking a lock you already hold is a **RELOCK** (unless it's a
recursive mutex, which allows that).

**`finish()`** handles slow work. Taking a stack trace and checking for circles can be slow, so
`finish()` unlocks `State::mu`, does the slow part, then locks again. Other threads aren't held up.

### `lock_graph.h` / `lock_graph.cpp`: finding potential deadlocks

**`lg_note_edges(thread, L)`**: for every lock H the thread currently holds, add the edge
H → L. Also record which other locks were held at the time, for the gate filter. A trylock or
timedlock adds no edges, because it never waits forever.

**`check_edge` + `find_paths`:** when an edge H → L is new, look for a path back from L to H
using **DFS** (depth-first search). If one exists, H → L → … → H is a circle.

**`choose`**: the false-alarm filter. A circle is only reported if:

1. The edges were taken by **at least two different threads**. One thread can't deadlock with
   itself this way.
2. The threads held **no common "gate" lock** at the time. If both held G first, only one can be
   inside at a time.
3. It's not just **readers waiting on readers**. Two read locks don't block each other.

`--naive` skips these checks. That's how we show the false alarms other tools give.

**`lg_suggest` + `lg_components`**: the fix suggestion. `lg_components` uses **Tarjan's
algorithm** to find groups of locks that are all tangled in circles. `lg_suggest` then picks an
order greedily: locks that nothing points to go first, otherwise the one with the most outgoing
minus incoming arrows. Edges that go "backwards" in that order are the lines of code to change.
(The perfect answer is an NP-hard problem, so this is a good-enough heuristic.)

### `waitfor.h` / `waitfor.cpp`: the watchdog (live deadlocks)

- **`wf_start()`** starts a background thread with the *real* `pthread_create`, so it's never
  recorded as one of the program's threads.
- **`watchdog_main()`** sleeps 500 ms, calls `wf_check()`, and repeats.
- **`wf_check()`**:
  1. For every thread that has been waiting longer than 500 ms, look at the lock it waits for and
     who owns it. That gives the arrows "thread X waits for thread Y".
  2. Run a **coloured DFS** over those arrows (white = not visited, grey = on the current path,
     black = done). Reaching a grey thread again means a circle.
  3. Each new circle becomes a **LIVE DEADLOCK** finding, made by `make_live()`. If an earlier
     potential deadlock involved the same locks, the report says "this hang was predicted".
  4. With `--on-deadlock abort`, it writes the final report and stops the program.

### `profiler.h` / `profiler.cpp`: contention

Simple counters, per lock:

- **`prof_acquired`** counts acquisitions, how many had to wait, and the total and max wait time.
- **`prof_released`** records the total and max hold time.
- **`prof_cond`** and **`prof_sem_*`** keep separate stats for condition variables and
  semaphores. Waiting for a signal isn't the same as fighting over a lock.
- **`prof_ranked`** sorts locks by total wait time. That's the "top locks" contention table.

### `symbolize.h` / `symbolize.cpp`: turning numbers into names

LockSpy stores raw memory addresses because it's fast. Before printing, this file makes them
readable:

- **`sym_pc(address)`** turns an address into a function, file and line, for example
  `worker_q() (ab_ba.cpp:32)`. It uses `dladdr` to find which file the address is in, then runs
  the `addr2line` tool. Results are cached.
- **`run_capture(command)`** starts a helper program (`addr2line` or `nm`) with `posix_spawnp`
  and reads its output. It removes `LD_PRELOAD` first, so the helper isn't spied on.
- **`site_of(stack)`** skips LockSpy's own frames and libc's frames, and returns the first line
  in *your* code.
- **`lock_name(id)`** gives a lock a readable name. It tries your `--names` file first, then the
  global variable name (found with `nm`, for example `forks[3]`), and falls back to the address.
- **`thread_name(idx)`** reads the thread's name from `/proc/self/task/<tid>/comm`.

### `report.h` / `report.cpp`: printing and saving

- **`report_emit(finding)`** formats one finding and prints it right away to stderr. It includes:
  - the locks and threads involved, with code lines
  - the suggested fix (`fix_text`)
  - the four Coffman conditions, each with a ✔ and why (`coffman`)
- **`report_final()`** runs once at the end. It prints the summary (counts plus the contention
  table) and writes the files you asked for:
  - `lockspy_report.json`: everything, for tools (the web UI reads this one)
  - `lockspy_graph.dot`: the lock graph, which Graphviz turns into a picture
  - `lockspy_report.html`: a single page with the summary and graph

---

## 4. Scripts: `bin/`

| File | What it does |
|---|---|
| `bin/lockspy` | The command you type. It turns options like `--naive` into `LOCKSPY_*` variables, sets `LD_PRELOAD=build/libLockSpy.so`, then runs your program. On a Mac it re-runs itself inside Docker. |
| `bin/in-docker` | Runs any command inside the `lockspy-dev` Ubuntu container. It mounts the project at `/lockspy` and builds the image the first time. The Makefile and `bin/lockspy` call it automatically on macOS. |

`docker/Dockerfile` is the container recipe: Ubuntu 24.04 with g++, make, gdb, valgrind,
binutils (for `addr2line` and `nm`), graphviz and python3.

## 5. `Makefile`

| Command | What it does |
|---|---|
| `make` | Compiles `src/*.cpp` into `build/libLockSpy.so` (with `-fPIC`, `-shared`, `-ldl`) and every test program into `build/tests/...` |
| `make test` | Runs `tests/run_tests.sh` |
| `make demo` | Runs ab_ba and writes all report types to `results/demo/` |
| `make bench` | Runs `bench/run_bench.sh` |
| `make memcheck` | Runs `tests/memcheck.sh` |
| `make ui` | Builds, then starts `ui/server.py` on port 8080 |
| `make clean` | Deletes `build/` and old results |

Test programs are built with `-g -O0 -rdynamic -no-pie`, so LockSpy can find file names, line
numbers and function names.

## 6. Tests: `tests/`

- **The 16 programs:** each `.cpp` starts with a comment saying what it does and what LockSpy
  should report.
- **`expected.csv`:** one row per test case (18 rows, because two programs also run with an extra
  argument). Each row has the program, its arguments, the expected findings, and whether it
  really hangs.
- **`run_tests.sh`:** runs each row under LockSpy and reads the `[LockSpy] ...` lines from stderr.
  It compares them with the expected findings and prints PASS or FAIL. It also checks the JSON is
  valid. Hanging programs run with `abort` and a 20-second timeout.
- **`memcheck.sh`:** runs the programs with LockSpy under Valgrind, to make sure *LockSpy itself*
  has no memory bugs.

## 7. Benchmarks: `bench/` and `results/`

- **`bench/run_bench.sh`:**
  - **Detection:** runs every test under LockSpy, LockSpy `--naive`, ThreadSanitizer and
    Helgrind, and marks each run caught, missed, clean or false alarm.
  - **Overhead:** times the programs with and without each tool.
  - **Output:** writes `results/detection.csv` and `results/overhead.csv`.
- **`bench/make_results.py`:** turns those CSVs into the readable `results/RESULTS.md`.
- **`results/bench/*.log`:** the raw output of every tool run, kept as proof.

## 8. The website: `ui/`

### `ui/server.py`: the Python server

It uses only the Python standard library (`http.server`), with no extra packages.

| Endpoint | What it does |
|---|---|
| `GET /` and static files | Serves `ui/static/*` |
| `GET /api/programs` | The list of test programs (from `expected.csv`) and whether they're built |
| `GET /api/source?path=` | The source code of a test program, for the Code tab |
| `GET /api/bench` | Summary numbers from `results/*.csv` |
| `POST /api/build` | Runs `make` |
| `POST /api/run` | Runs one test program under LockSpy (`run_program` then `run_exe`) |
| `POST /api/check` | Compiles code typed into the page and runs it (`check_code`) |

- **`run_exe()`** is the core. It:
  1. makes a temporary folder
  2. sets `LD_PRELOAD` and `LOCKSPY_FORMAT=json`
  3. runs the program with a timeout
  4. reads `lockspy_report.json` and sends it back to the page with the output
- **`check_code()`** saves your code as `mycode.cpp` (or `.c`), compiles it with the same flags as
  the tests, and runs it in "hang" mode so a freeze is stopped by the watchdog. It sends back
  either the compile errors or the normal result.
- **`inside_root()`** stops a request from reading or running files outside the project folder.

### `ui/static/`: the pages

| File | What it does |
|---|---|
| `index.html` | Dashboard layout: header, intro, score cards, program list, detail panel |
| `app.js` | All dashboard behaviour (see below) |
| `style.css` | All colours, fonts and layout for both pages, including dark mode |
| `learn.html` + `learn.js` | The 13-step animated lesson |

**`app.js`, the main parts:**

| Part | Functions | What it does |
|---|---|---|
| Talking to the server | `api()` | One helper for every fetch call |
| Program list | `loadPrograms`, `renderList`, `select` | Shows the programs and handles clicks |
| Running | `runOne`, `runSelected`, `runAll` | Calls `/api/run` or `/api/check` and updates the score cards (`updateKpis`) |
| Results | `renderDetail`, `renderReport`, `findingCard`, `renderGraph`, `renderContention` | Fills the four tabs. `renderGraph` draws the lock graph as SVG, with the circle in red. |
| Code tab | `loadCode`, `highlightCode` | Shows the source with the reported lines highlighted |
| Editor | `EXAMPLES`, `renderEditor`, `markEditor`, `drawGutter`, `gotoLine` | The "Write your own code" box. Your code is saved in the browser (`localStorage`), and red dots mark the lines LockSpy points at. |
| Other | `loadBench`, `rebuild`, `showIntro` | Benchmark card, Rebuild button, intro card |

**`learn.js`** holds the lesson. `STEPS` lists the 13 steps (text, code and which arrows to
show), `drawScene` and `drawEdges` draw the threads and locks, and `show(n)` moves between steps.

---

## 9. Follow one lock call from start to end

Example: `tests/buggy/ab_ba.cpp`. Thread Q holds lock **B** and calls `pthread_mutex_lock(&A)`.

1. Because of `LD_PRELOAD`, the call lands in **our** `pthread_mutex_lock` (`interpose.cpp`).
2. `Guard` checks we're not already inside LockSpy.
3. We try the real `trylock` on A. It's free, so that works. We call `rec_acquired`
   (`recorder.cpp`).
4. `rec_acquired` locks `State::mu`. Then `lg_note_edges` adds the edge **B → A** ("Q held B and
   took A").
5. A goes into Q's held list, and `prof_acquired` counts it.
6. `finish()` takes a stack trace and runs `check_edge`. DFS finds **A → B** from thread P
   earlier, so **B → A → B** is a circle.
7. `choose()` checks the circle is real: two different threads (P and Q), no shared gate lock,
   and not readers only. It's real.
8. `lg_suggest()` works out the order "A before B", so Q's line is the one to change.
9. `report_emit()` turns addresses into names (`symbolize.cpp`) and prints **POTENTIAL
   DEADLOCK** with the fix and the Coffman conditions.
10. Our function returns 0. The program carries on and never knows LockSpy was there.

If A had been **busy**, step 3 would go through `rec_before_block` → real lock (waiting) →
`rec_after_block`. If Q never gets A, the watchdog finds Q → A → P → B → Q and reports a **LIVE
DEADLOCK**.

## 10. Where to change things

| I want to... | Edit |
|---|---|
| Add a new command-line option | `bin/lockspy` + `config.h`/`config.cpp` |
| Intercept another pthread function | `interpose.cpp` (wrapper) + `RealFns` in `recorder.h` + a `rec_*` function |
| Change how false alarms are filtered | `choose()` in `lock_graph.cpp` |
| Change the report text | `format()` / `coffman()` in `report.cpp` |
| Add a test program | Put the `.cpp` in `tests/<group>/` and add a row to `tests/expected.csv` |
| Change the website look | `ui/static/style.css` |
| Change the website text or layout | `ui/static/index.html`, `learn.html` |
| Change what the website does | `ui/static/app.js`, `ui/server.py` |
