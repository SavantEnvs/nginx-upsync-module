#!/usr/bin/env bash
#
# mayhem/build.sh — build the fuzz harnesses and the project's known-answer test binary.
#
# nginx-upsync-module is an nginx add-on. The two self-contained parsers it bundles — the cJSON copy
# (src/ngx_http_json.c) and joyent's http_parser (src/ngx_http_parser.c) — have no nginx
# dependency, so they are compiled directly (sanitized for fuzzing, plain for the test binary).
set -euo pipefail

# clang rejects SOURCE_DATE_EPOCH='' (empty) — it must be unset or a valid integer.
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH

: "${SANITIZER_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer}"
: "${DEBUG_FLAGS:=-g -gdwarf-3}"
: "${CC:=clang}" ; : "${CXX:=clang++}" ; : "${LIB_FUZZING_ENGINE:=-fsanitize=fuzzer}"
: "${MAYHEM_JOBS:=$(nproc)}"
: "${COVERAGE_FLAGS=}"
export SANITIZER_FLAGS DEBUG_FLAGS CC CXX LIB_FUZZING_ENGINE MAYHEM_JOBS COVERAGE_FLAGS

cd "$SRC"
B="$SRC/mayhem/build"
mkdir -p "$B"

# 1) The project's parsers, instrumented: sanitizers + DWARF<=3 + SanCov (fuzzer-no-link) so the
#    fuzzed code itself reports coverage, not just the harness TU.
for f in ngx_http_json ngx_http_parser; do
    $CC $SANITIZER_FLAGS $DEBUG_FLAGS -fsanitize=fuzzer-no-link -O1 \
        -c "src/$f.c" -o "$B/$f.o"
done
$CXX $SANITIZER_FLAGS $DEBUG_FLAGS -c mayhem/lsan_off.cc -o "$B/lsan_off.o"

# 2) Each harness twice: libFuzzer binary + a run-once standalone reproducer.
build_harness() {
    local name="$1"; shift
    $CC $SANITIZER_FLAGS $DEBUG_FLAGS -O1 $LIB_FUZZING_ENGINE \
        "mayhem/$name.c" "$@" "$B/lsan_off.o" -lm -o "$SRC/$name"
    $CC $SANITIZER_FLAGS $DEBUG_FLAGS -O1 "$STANDALONE_FUZZ_MAIN" \
        "mayhem/$name.c" "$@" "$B/lsan_off.o" -lm -o "$SRC/$name-standalone"
}
build_harness fuzz_json "$B/ngx_http_json.o"
build_harness fuzz_http_parser "$B/ngx_http_parser.o"

# 3) Known-answer test binary, NORMAL flags (no sanitizers, no -gdwarf-3): an independent build of
#    the same sources, so test.sh stays an honest functional oracle.
$CC -O2 $COVERAGE_FLAGS mayhem/selftest/selftest.c src/ngx_http_json.c src/ngx_http_parser.c \
    -lm -o "$B/selftest"
