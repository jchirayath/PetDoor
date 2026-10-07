// fuzz_beacon.cpp — TEST-10: fuzz the only attacker-controlled input the
// firmware has.
//
// Every byte these functions see arrived over the air from an unauthenticated
// radio advertisement. Anyone within range can send anything: truncated frames,
// absurd lengths, NULs anywhere, non-hex in a UUID. The parsers index into the
// payload directly (`p[24]`, `p[13]`), so a length check that is off by one is
// an out-of-bounds read in the BLE host task.
//
// Builds two ways:
//   * with libFuzzer      — coverage-guided, for CI and long nightly runs
//     c++ -std=c++17 -g -fsanitize=fuzzer,address,undefined -o fuzz_beacon \
//         fuzz_beacon.cpp ../../petdoor/beacon.cpp -I.
//   * without it          — a deterministic driver with a fixed seed, so the
//     check still runs on a machine with no fuzzer runtime (Apple clang ships
//     none) and so a crash is reproducible from the seed alone.
//
// Run it under ASan/UBSan either way; the point is the sanitiser, not the
// search. A crash becomes a regression test in test_beacon.cpp (TEST-8).

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "../../petdoor/beacon.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  // Binary-safe, exactly as the fixed BLE adapter hands it over: the parsers
  // must cope with any length, including 0.
  const String payload(data, static_cast<unsigned int>(size));

  IBeaconData ib{};
  parseIBeacon(payload, ib);

  EddystoneTlm tlm{};
  parseEddystoneTlm(payload, tlm);

  // uuidFromString takes a C string, so give it one — including the case where
  // the fuzzer's bytes contain an embedded NUL.
  {
    std::string text(reinterpret_cast<const char *>(data), size);
    uint8_t uuid[16] = {0};
    uuidFromString(text.c_str(), uuid);
  }

  // classifyDevice walks three strings looking for substrings.
  classifyDevice(payload, payload, payload);

  // And the distance model, which does logs and powers on attacker-influenced
  // RSSI and measuredPower.
  if (size >= 2) {
    const int rssi = static_cast<int8_t>(data[0]);
    const int8_t power = static_cast<int8_t>(data[1]);
    estimateDistanceM(rssi, power, 2.0f);
    rssiAtDistanceM(static_cast<float>(rssi), power, 2.0f);
  }
  return 0;
}

#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
// Deterministic driver. Not a substitute for coverage-guided fuzzing — it is
// what makes the target runnable in the default local loop, so it cannot rot.
namespace {

uint32_t g_state = 0x5EED5EED;   // fixed: every run explores the same inputs
uint32_t nextRand() {            // xorshift32
  g_state ^= g_state << 13;
  g_state ^= g_state >> 17;
  g_state ^= g_state << 5;
  return g_state;
}

}  // namespace

int main(int argc, char **argv) {
  const long iterations = (argc > 1) ? std::strtol(argv[1], nullptr, 10) : 200000;

  // Lengths around every boundary the parsers care about: the iBeacon minimum
  // (25), the TLM minimum (14), 0, and one either side of each.
  static const size_t kInteresting[] = {0, 1, 2, 13, 14, 15, 24, 25, 26, 31, 64, 255};

  uint8_t buf[512];
  for (long i = 0; i < iterations; i++) {
    size_t n;
    if (i % 4 == 0) {
      n = kInteresting[nextRand() % (sizeof(kInteresting) / sizeof(kInteresting[0]))];
    } else {
      n = nextRand() % sizeof(buf);
    }
    for (size_t b = 0; b < n; b++) buf[b] = static_cast<uint8_t>(nextRand());

    // Half the time, make it look like a real frame header so the fuzzer gets
    // past the type checks and into the field decoding.
    if (n >= 4 && (i % 2)) {
      buf[0] = 0x4C; buf[1] = 0x00; buf[2] = 0x02; buf[3] = 0x15;
    } else if (n >= 2 && (i % 3) == 0) {
      buf[0] = 0x20; buf[1] = 0x00;
    }
    LLVMFuzzerTestOneInput(buf, n);
  }
  std::printf("ok      fuzz: %ld iterations, seed 0x5EED5EED, no crash\n", iterations);
  return 0;
}
#endif
