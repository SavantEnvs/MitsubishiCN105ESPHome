#!/usr/bin/env bash
# mayhem/test.sh — run the upstream GoogleTest suite (tests/unit) that mayhem/build.sh built with
# normal flags. The tests assert decoded values of real captured CN105 frames, checksums, lookup
# tables and temperature conversions (known-answer tests). Emits a CTRF report.
set -uo pipefail
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH
: "${SRC:=/mayhem}"
cd "$SRC"

emit_ctrf() {
  local tool="$1" passed="$2" failed="$3" skipped="${4:-0}" pending="${5:-0}" other="${6:-0}"
  local tests=$(( passed + failed + skipped + pending + other ))
  cat > "${CTRF_REPORT:-$SRC/ctrf-report.json}" <<JSON
{
  "results": {
    "tool": { "name": "$tool" },
    "summary": {
      "tests": $tests,
      "passed": $passed,
      "failed": $failed,
      "pending": $pending,
      "skipped": $skipped,
      "other": $other
    }
  }
}
JSON
  printf 'CTRF {"results":{"tool":{"name":"%s"},"summary":{"tests":%d,"passed":%d,"failed":%d,"pending":%d,"skipped":%d,"other":%d}}}\n' \
    "$tool" "$tests" "$passed" "$failed" "$pending" "$skipped" "$other"
  [ "$failed" -eq 0 ]
}

passed=0; failed=0; skipped=0
tmp=$(mktemp -d "${TMPDIR:-/tmp}/cn105-test.XXXXXX")
for t in cn105_tests cn105_legacy_action_tests; do
  bin="tests/unit/build/$t"
  if [ ! -x "$bin" ]; then
    echo "missing $bin (run mayhem/build.sh first)" >&2
    failed=$((failed + 1)); continue
  fi
  "$bin" --gtest_output="xml:$tmp/$t.xml" > "$tmp/$t.log" 2>&1
  rc=$?
  tail -n 3 "$tmp/$t.log"
  if [ ! -s "$tmp/$t.xml" ]; then
    echo "$t produced no report (rc=$rc)" >&2
    failed=$((failed + 1)); continue
  fi
  # count <testcase> results from the gtest XML report
  n=$(grep -c '<testcase ' "$tmp/$t.xml")
  f=$(grep -c '<failure' "$tmp/$t.xml")
  s=$(grep -c 'result="skipped"' "$tmp/$t.xml")
  passed=$((passed + n - f - s)); failed=$((failed + f)); skipped=$((skipped + s))
  [ "$rc" -eq 0 ] || [ "$f" -gt 0 ] || failed=$((failed + 1))
done
rm -rf "$tmp"
emit_ctrf "googletest" "$passed" "$failed" "$skipped"
