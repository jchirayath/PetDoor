#!/usr/bin/env bash
# Host tests for the pure-logic parts of the firmware. No hardware, no toolchain
# beyond a C++17 compiler — so this can run on every commit.
#
#   tests/host/run.sh              build, test, short fuzz, adapter guard
#   tests/host/run.sh --fuzz 5000000   longer fuzz budget (nightly)
set -euo pipefail
cd "$(dirname "$0")"
REPO="$(cd ../.. && pwd)"
CXX="${CXX:-c++}"
FLAGS="-std=c++17 -g -O1 -Wall -Wextra -I."
# ASan+UBSan are the point of running these at all: the parsers index straight
# into attacker-supplied bytes, and a sanitiser is what turns an off-by-one into
# a failed build instead of a field crash.
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer"
status=0
OUT="$(mktemp -d)"; trap 'rm -rf "$OUT"' EXIT

echo "▸ unit: beacon.cpp"
$CXX $FLAGS $SAN -o "$OUT/test_beacon" test_beacon.cpp "$REPO/petdoor/beacon.cpp"
"$OUT/test_beacon" || status=1

# Header-only and dependency-free, so this needs neither the Arduino shim nor
# config.h. Both decisions fail silently in both directions — see the file.
echo "▸ unit: sensor_verdict.h (noise and reed-at-rest decisions)"
$CXX $FLAGS $SAN -o "$OUT/test_sensor_verdict" test_sensor_verdict.cpp
"$OUT/test_sensor_verdict" || status=1

echo "▸ fuzz: parsers against arbitrary advertisement bytes (TEST-10)"
ITERS=200000
if [ "${1:-}" = "--fuzz" ]; then ITERS="${2:-5000000}"; fi
if $CXX $FLAGS -fsanitize=fuzzer,address,undefined \
        -o "$OUT/fuzz_beacon" fuzz_beacon.cpp "$REPO/petdoor/beacon.cpp" 2>/dev/null; then
  echo "  (libFuzzer available — coverage-guided)"
  "$OUT/fuzz_beacon" -max_total_time="${FUZZ_SECONDS:-20}" -print_final_stats=1 || status=1
else
  echo "  (no libFuzzer runtime — deterministic driver under ASan/UBSan)"
  $CXX $FLAGS $SAN -o "$OUT/fuzz_beacon" fuzz_beacon.cpp "$REPO/petdoor/beacon.cpp"
  "$OUT/fuzz_beacon" "$ITERS" || status=1
fi

# SHR-2: the standard says a "banned elsewhere" rule is enforced by a lint rule,
# not by review. This is that rule.
#
# The NimBLE adapter must not convert a binary payload through c_str(): Arduino
# String stops at the first NUL, manufacturer data begins 4C 00 and Eddystone
# TLM begins 20 00, so that silently truncated every frame to one byte and left
# the low-battery warning inert and UUID matching permanently unmatchable.
echo "▸ guard: the BLE adapter must not truncate binary payloads"
ADAPTER="$REPO/petdoor/ble_scanner.cpp"
# Comments are stripped first: the fix's own explanation names the banned idiom,
# and a guard that cannot tell code from prose is a guard nobody keeps.
if grep -vE '^[[:space:]]*(//|\*)' "$ADAPTER" | grep -nE 'String\([a-zA-Z_]+\.c_str\(\)\s*\)'; then
  echo "  ✗ ble_scanner.cpp converts a payload with String(x.c_str()) — that"
  echo "    truncates at the first NUL. Use String(bytes, len). See tests/host/README.md."
  status=1
elif grep -q 'bleToString(const std::string' "$ADAPTER" &&
     ! grep -A3 'bleToString(const std::string' "$ADAPTER" | grep -qE '\.size\(\)|length'; then
  echo "  ✗ the std::string overload of bleToString() does not carry a length."
  status=1
else
  echo "  ok    binary payloads carry their length"
fi

[ $status -eq 0 ] && echo "✓ host tests passed" || echo "✗ host tests failed"
exit $status
