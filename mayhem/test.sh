#!/usr/bin/env bash
#
# mayhem/test.sh — RUN the known-answer suite that mayhem/build.sh built (mayhem/build/selftest).
# It checks fixed inputs against fixed expected values in the bundled cJSON and http_parser, and
# prints "SUMMARY: N tests run; F failed". A neutered/missing binary prints nothing, which fails here.
set -uo pipefail
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH
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

runner="$SRC/mayhem/build/selftest"
MIN_TESTS=50
if [ ! -x "$runner" ]; then
  echo "test.sh: $runner missing — mayhem/build.sh should have built it" >&2
  emit_ctrf "selftest" 0 1 0
  exit 1
fi

out="$("$runner" 2>&1)"
printf '%s\n' "$out" | grep -v '^PASS ' || true

summary="$(printf '%s\n' "$out" | sed -n 's/^SUMMARY: \([0-9][0-9]*\) tests run; \([0-9][0-9]*\) failed$/\1 \2/p' | tail -n1)"
if [ -z "$summary" ]; then
  echo "test.sh: no SUMMARY line from selftest" >&2
  emit_ctrf "selftest" 0 1 0
  exit 1
fi
run="${summary% *}"; bad="${summary#* }"
npass="$(printf '%s\n' "$out" | grep -c '^PASS ')"
if [ "$run" -lt "$MIN_TESTS" ] || [ "$npass" -ne $(( run - bad )) ]; then
  echo "test.sh: implausible results (run=$run failed=$bad passlines=$npass, expected >= $MIN_TESTS)" >&2
  emit_ctrf "selftest" "$npass" $(( bad > 0 ? bad : 1 )) 0
  exit 1
fi
emit_ctrf "selftest" "$npass" "$bad" 0
