#!/usr/bin/env bash
# memcheck.sh - runs the non-hanging test programs with LockSpy preloaded under Valgrind
# memcheck, to find invalid reads/writes and real ("definitely lost") leaks in LockSpy itself.
# LockSpy's global tables are intentionally never freed (they must outlive every destructor);
# Valgrind lists them as "still reachable", which is not counted as an error.
cd "$(dirname "$0")/.."
command -v valgrind >/dev/null || { echo "valgrind not installed"; exit 0; }
mkdir -p results/memcheck
bad=0
while IFS=, read -r prog args expected mode; do
  [ "$prog" = program ] || [ "$mode" = hang ] && continue
  case "$prog" in perf/*) args=1000 ;; esac
  log="results/memcheck/$(echo "$prog $args" | sed 's/ *$//; s/[ \/-]\+/_/g').log"
  LD_PRELOAD=./build/libLockSpy.so LOCKSPY_QUIET=1 \
    valgrind --tool=memcheck --error-exitcode=99 --leak-check=full --errors-for-leak-kinds=definite \
    --log-file="$log" ./build/tests/$prog $args > /dev/null 2>&1
  rc=$?
  errs=$(grep -oE 'ERROR SUMMARY: [0-9]+' "$log" | tail -1)
  printf "%-36s %s\n" "$prog $args" "${errs:-?}"
  [ $rc = 99 ] && bad=$((bad+1))
done < tests/expected.csv
echo "programs with memcheck errors: $bad  (logs in results/memcheck/)"
[ $bad -eq 0 ]
