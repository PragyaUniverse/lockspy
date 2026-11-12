#!/usr/bin/env bash
# run_tests.sh - runs every program in tests/expected.csv under LockSpy and compares the
# finding types it reports with the expected ones. Programs marked "hang" really deadlock:
# they run with LOCKSPY_ON_DEADLOCK=abort (the watchdog reports, then aborts) and a timeout.
# Prints a PASS/FAIL table; exits non-zero if any program does not match.
# Extra: LOCKSPY_NAIVE=1 ./tests/run_tests.sh shows what a naive detector would report.
cd "$(dirname "$0")/.."
out_root=results/tests
mkdir -p "$out_root"
pass=0; fail=0
printf "%-38s %-34s %-34s %s\n" "PROGRAM" "EXPECTED" "REPORTED" "RESULT"
# Map the report headlines to finding types.
kinds() {
  grep -E '^\[LockSpy\] ' "$1" | sed -E \
    -e 's/^\[LockSpy\] POTENTIAL DEADLOCK.*/POTENTIAL_DEADLOCK/' \
    -e 's/^\[LockSpy\] LIVE DEADLOCK.*/LIVE_DEADLOCK/' \
    -e 's/^\[LockSpy\] MISUSE: relock.*/RELOCK/' \
    -e 's/^\[LockSpy\] MISUSE: unlock.*/BAD_UNLOCK/' \
    -e 's/^\[LockSpy\] MISUSE: thread exited.*/HELD_AT_EXIT/' |
    grep -E '^[A-Z_]+$' | sort -u | paste -sd ';' -
}
sorted() { echo "$1" | tr ';' '\n' | sort -u | paste -sd ';' -; }
while IFS=, read -r prog args expected mode; do
  [ "$prog" = "program" ] && continue
  name="$(echo "$prog $args" | sed 's/ *$//; s/[ \/-]\+/_/g')"
  dir="$out_root/$name"; mkdir -p "$dir"
  export LOCKSPY_OUTDIR="$dir" LOCKSPY_FORMAT=text,json,dot
  if [ "$mode" = hang ]; then export LOCKSPY_ON_DEADLOCK=abort; else export LOCKSPY_ON_DEADLOCK=report; fi
  ( timeout -s KILL 20 ./bin/lockspy run -- ./build/tests/$prog $args > "$dir/stdout.txt" 2> "$dir/stderr.txt"; exit $? ) 2>/dev/null
  rc=$?
  got="$(kinds "$dir/stderr.txt")"; got="${got:-NONE}"
  want="$(sorted "$expected")"
  result=PASS; note=""
  [ "$(sorted "$got")" = "$want" ] || result=FAIL
  [ "$rc" = 137 ] && { result=FAIL; note=" (timeout)"; }
  if [ -f "$dir/lockspy_report.json" ] && ! python3 -m json.tool "$dir/lockspy_report.json" > /dev/null 2>&1; then
    result=FAIL; note="$note (bad JSON)"
  fi
  [ $result = PASS ] && pass=$((pass+1)) || fail=$((fail+1))
  printf "%-38s %-34s %-34s %s%s\n" "$prog $args" "$want" "$got" "$result" "$note"
done < tests/expected.csv
echo "passed $pass, failed $fail  (logs and reports in $out_root/)"
[ $fail -eq 0 ]
