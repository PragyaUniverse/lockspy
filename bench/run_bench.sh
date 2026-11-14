#!/usr/bin/env bash
# run_bench.sh - honest head-to-head evaluation of LockSpy.
#  1. Detection: every program in tests/expected.csv under LockSpy, LockSpy --naive (filters off),
#     ThreadSanitizer (needs a -fsanitize=thread rebuild) and Valgrind Helgrind (if installed).
#     Each run is classified: caught / missed (buggy programs), clean / false alarm (others).
#  2. Overhead: wall-clock time of the perf and correct programs, bare vs each tool (median of N).
# Output: results/detection.csv, results/overhead.csv, results/RESULTS.md. Nothing is invented:
# every number in RESULTS.md comes from the runs below.
#
# ThreadSanitizer in GCC 13 aborts with "unexpected memory mapping" on kernels with high ASLR
# entropy; we run it under "setarch -R" (ASLR off for that process). In Docker this needs
# --security-opt seccomp=unconfined.
cd "$(dirname "$0")/.."
if [ "$(uname -s)" != Linux ]; then
  echo "$(basename "$0"): LockSpy needs Linux; on macOS run this inside the lockspy-dev container (see README)." >&2
  exit 2
fi
RUNS="${RUNS:-5}"
mkdir -p results/bench build/tsan
LIB=./build/libLockSpy.so

# ---------- ThreadSanitizer rebuild ----------
tsan_ok=0
if echo 'int main(){}' | g++ -x c++ -fsanitize=thread - -o build/tsan/probe 2>/dev/null; then
  TSAN_PREFIX=""
  setarch -R true 2>/dev/null && TSAN_PREFIX="setarch -R"
  for src in tests/*/*.cpp; do
    out="build/tsan/${src#tests/}"; out="${out%.cpp}"; mkdir -p "$(dirname "$out")"
    g++ -std=c++17 -fsanitize=thread -g -O0 -pthread "$src" -o "$out"
  done
  $TSAN_PREFIX ./build/tsan/correct/consistent_order > /dev/null 2>&1 && tsan_ok=1
fi
helgrind_ok=0; command -v valgrind > /dev/null && helgrind_ok=1

# Finding types from each tool's output (same vocabulary for all tools).
lockspy_kinds() {
  grep -E '^\[LockSpy\] (POTENTIAL|LIVE|MISUSE)' "$1" | sed -E \
    -e 's/.*POTENTIAL DEADLOCK.*/POTENTIAL_DEADLOCK/' -e 's/.*LIVE DEADLOCK.*/LIVE_DEADLOCK/' \
    -e 's/.*MISUSE: relock.*/RELOCK/' -e 's/.*MISUSE: unlock.*/BAD_UNLOCK/' \
    -e 's/.*MISUSE: thread exited.*/HELD_AT_EXIT/' | sort -u | paste -sd ';' -
}
tsan_kinds() {
  grep -oE 'WARNING: ThreadSanitizer: [a-z -]+' "$1" | sed -E \
    -e 's/.*lock-order-inversion.*/POTENTIAL_DEADLOCK/' -e 's/.*double lock.*/RELOCK/' \
    -e 's/.*unlock of an unlocked.*/BAD_UNLOCK/' -e 's/.*data race.*/DATA_RACE/' \
    -e 's/.*thread leak.*/THREAD_LEAK/' -e 's/^WARNING.*/OTHER/' | sort -u | paste -sd ';' -
}
helgrind_kinds() {
  { grep -q 'lock order .* violated' "$1" && echo POTENTIAL_DEADLOCK
    grep -q 'Attempt to re-lock' "$1" && echo RELOCK
    grep -qE 'unlocked a not-locked lock|unlocked lock .* currently held by' "$1" && echo BAD_UNLOCK
    grep -q 'Exiting thread still holds' "$1" && echo HELD_AT_EXIT
    grep -q 'Possible data race' "$1" && echo DATA_RACE
  } | sort -u | paste -sd ';' -
}

# run_tool TOOL PROG ARGS MODE LOG -> runs one program under one tool, output in LOG
run_tool() {
  local tool=$1 prog=$2 args=$3 mode=$4 log=$5
  case $tool in
    bare)     ( timeout -s KILL 20 ./build/tests/$prog $args > /dev/null 2> "$log"; exit $? ) 2>/dev/null ;;
    lockspy)  ( LOCKSPY_ON_DEADLOCK=$([ "$mode" = hang ] && echo abort || echo report) LOCKSPY_OUTDIR=results/bench \
                timeout -s KILL 20 env LD_PRELOAD=$LIB ./build/tests/$prog $args > /dev/null 2> "$log"; exit $? ) 2>/dev/null ;;
    naive)    ( LOCKSPY_NAIVE=1 LOCKSPY_ON_DEADLOCK=$([ "$mode" = hang ] && echo abort || echo report) LOCKSPY_OUTDIR=results/bench \
                timeout -s KILL 20 env LD_PRELOAD=$LIB ./build/tests/$prog $args > /dev/null 2> "$log"; exit $? ) 2>/dev/null ;;
    tsan)     ( timeout -s KILL 20 $TSAN_PREFIX ./build/tsan/$prog $args > /dev/null 2> "$log"; exit $? ) 2>/dev/null ;;
    helgrind) ( timeout -s KILL 60 valgrind --tool=helgrind ./build/tests/$prog $args > /dev/null 2> "$log"; exit $? ) 2>/dev/null ;;
  esac
}

# ---------- 1. detection ----------
tools="lockspy naive"
[ $tsan_ok = 1 ] && tools="$tools tsan"
[ $helgrind_ok = 1 ] && tools="$tools helgrind"
echo "program,args,group,expected,tool,reported,verdict" > results/detection.csv
echo "== detection ($tools)"
while IFS=, read -r prog args expected mode; do
  [ "$prog" = program ] && continue
  group=${prog%%/*}
  [ "$group" = perf ] && continue
  for tool in $tools; do
    log="results/bench/$(echo "$tool $prog $args" | sed 's/ *$//; s/[ \/-]\+/_/g').log"
    run_tool $tool "$prog" "$args" "$mode" "$log"
    case $tool in lockspy|naive) got=$(lockspy_kinds "$log") ;; tsan) got=$(tsan_kinds "$log") ;; helgrind) got=$(helgrind_kinds "$log") ;; esac
    got=${got:-NONE}
    if [ "$group" = buggy ]; then
      verdict=missed
      for k in ${expected//;/ }; do [[ ";$got;" == *";$k;"* ]] && verdict=caught; done
    else
      [ "$got" = NONE ] && verdict=clean || verdict="false alarm"
    fi
    echo "$prog,$args,$group,$expected,$tool,$got,$verdict" >> results/detection.csv
    printf "  %-34s %-9s %-40s %s\n" "$prog $args" "$tool" "$got" "$verdict"
  done
done < tests/expected.csv

# ---------- 2. overhead ----------
echo "program,tool,run,seconds" > results/overhead.csv
echo "== overhead (median of $RUNS runs; Helgrind 1 run)"
perf_progs=("perf/uncontended_loop 1000000" "perf/contention_heavy 4000" "correct/philosophers_ordered" \
            "correct/producer_consumer_ok" "correct/readers_writers_ok" "correct/consistent_order")
for entry in "${perf_progs[@]}"; do
  set -- $entry; prog=$1; args=${2:-}
  for tool in bare lockspy $([ $tsan_ok = 1 ] && echo tsan) $([ $helgrind_ok = 1 ] && echo helgrind); do
    n=$RUNS; [ $tool = helgrind ] && n=1
    for r in $(seq 1 $n); do
      t0=$(date +%s.%N)
      LOCKSPY_QUIET=1 run_tool $tool "$prog" "$args" run /dev/null
      t1=$(date +%s.%N)
      echo "$prog $args,$tool,$r,$(echo "$t1 - $t0" | bc -l 2>/dev/null || python3 -c "print($t1-$t0)")" >> results/overhead.csv
    done
  done
  echo "  $prog $args done"
done

# ---------- 3. RESULTS.md ----------
python3 bench/make_results.py "$tsan_ok" "$helgrind_ok" "$RUNS"
