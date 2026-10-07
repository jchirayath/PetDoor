// test_beacon.cpp — host unit tests for petdoor/beacon.cpp.
//
// These exist because of a bug that shipped and ran for weeks: Eddystone
// battery telemetry never parsed, and iBeacon UUID matching could never match,
// on the DEFAULT BLE stack. Nothing noticed, because the only verification this
// project had was "it compiles for the target".
//
// beacon.* parses bytes that arrive from any radio in range — the only
// attacker-controlled input the firmware has. It is also pure functions with no
// stack dependency, so it costs nothing to test properly. See tests/host/README.md.
//
// No framework on purpose: this repo's rule is that a clean checkout builds
// with nothing extra, and a test runner should not be the thing that breaks it.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "../../petdoor/beacon.h"

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, what)                                                        \
  do {                                                                           \
    g_checks++;                                                                  \
    if (!(cond)) {                                                               \
      g_failures++;                                                              \
      std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, (what));             \
    }                                                                            \
  } while (0)

// ---------------------------------------------------------------------------
// Payload builders. Real frame layouts, from docs/BEACON-MINEW.md and the
// Eddystone spec.
// ---------------------------------------------------------------------------

// Apple iBeacon manufacturer-specific data, 25 bytes.
//   4C 00 | 02 | 15 | <16-byte UUID> | <major BE> | <minor BE> | <power>
static std::string iBeaconPayload(const uint8_t uuid[16], uint16_t major,
                                  uint16_t minor, int8_t power) {
  std::string p;
  p += '\x4C'; p += '\x00';            // Apple company ID, little-endian
  p += '\x02'; p += '\x15';            // iBeacon type, length 21
  p.append(reinterpret_cast<const char *>(uuid), 16);
  p += static_cast<char>(major >> 8); p += static_cast<char>(major & 0xFF);
  p += static_cast<char>(minor >> 8); p += static_cast<char>(minor & 0xFF);
  p += static_cast<char>(power);
  return p;
}

// Eddystone-TLM service data for UUID 0xFEAA, 14 bytes.
//   20 | version | VBATT(2 BE mV) | TEMP(2, 8.8 signed) | ADV_CNT(4 BE) | SEC_CNT(4 BE)
static std::string tlmPayload(uint16_t mv, int16_t temp8_8, uint32_t adv, uint32_t deci) {
  std::string p;
  p += '\x20'; p += '\x00';
  p += static_cast<char>(mv >> 8); p += static_cast<char>(mv & 0xFF);
  p += static_cast<char>(static_cast<uint16_t>(temp8_8) >> 8);
  p += static_cast<char>(static_cast<uint16_t>(temp8_8) & 0xFF);
  for (int i = 3; i >= 0; i--) p += static_cast<char>((adv >> (8 * i)) & 0xFF);
  for (int i = 3; i >= 0; i--) p += static_cast<char>((deci >> (8 * i)) & 0xFF);
  return p;
}

// The two ways the BLE adapter could hand a payload to the parser. The first is
// the bug; the second is the contract beacon.h documents.
static String asTruncated(const std::string &raw) { return String(raw.c_str()); }
static String asBinarySafe(const std::string &raw) {
  return String(reinterpret_cast<const uint8_t *>(raw.data()),
                static_cast<unsigned int>(raw.size()));
}

// ---------------------------------------------------------------------------

static void test_ibeacon_happy() {
  // A UUID with NULs inside it, which is entirely ordinary and is why the
  // length contract matters past byte 1.
  const uint8_t uuid[16] = {0xE2, 0xC5, 0x6D, 0xB5, 0x00, 0x00, 0x48, 0xD2,
                            0xB0, 0x60, 0xD0, 0xF5, 0xA7, 0x10, 0x96, 0xE0};
  const std::string raw = iBeaconPayload(uuid, 1, 2, -59);
  CHECK(raw.size() == 25, "iBeacon payload is 25 bytes");

  IBeaconData ib{};
  CHECK(parseIBeacon(asBinarySafe(raw), ib), "a valid iBeacon frame parses");
  CHECK(std::memcmp(ib.uuid, uuid, 16) == 0, "uuid round-trips, NULs included");
  CHECK(ib.major == 1, "major is big-endian");
  CHECK(ib.minor == 2, "minor is big-endian");
  CHECK(ib.measuredPower == -59, "measuredPower is signed");
}

// THE REGRESSION TEST. This is the bug, pinned.
//
// Manufacturer data begins 4C 00 — the second byte is a NUL — so a String built
// from c_str() has length 1 and no iBeacon frame can ever parse. Eddystone TLM
// begins 20 00 and fails identically.
static void test_nul_truncation_is_the_bug() {
  const uint8_t uuid[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
  const std::string raw = iBeaconPayload(uuid, 7, 9, -50);

  CHECK(asTruncated(raw).length() == 1,
        "String(c_str()) truncates manufacturer data at the company ID's NUL");
  CHECK(asBinarySafe(raw).length() == 25,
        "String(bytes, len) preserves the whole frame");

  IBeaconData ib{};
  CHECK(!parseIBeacon(asTruncated(raw), ib),
        "truncated: the parser correctly refuses a 1-byte frame");
  CHECK(parseIBeacon(asBinarySafe(raw), ib),
        "binary-safe: the SAME bytes parse — so the defect is the conversion, "
        "not the parser");

  const std::string tlm = tlmPayload(3000, 0x1900, 42, 100);
  EddystoneTlm t{};
  CHECK(asTruncated(tlm).length() == 1, "TLM truncates at its version NUL too");
  CHECK(!parseEddystoneTlm(asTruncated(tlm), t), "truncated TLM cannot parse");
  CHECK(parseEddystoneTlm(asBinarySafe(tlm), t), "binary-safe TLM parses");
}

static void test_ibeacon_rejects() {
  const uint8_t uuid[16] = {0};
  const std::string good = iBeaconPayload(uuid, 1, 1, -59);

  // Every truncation, 0..24 bytes: must refuse, must not read off the end.
  for (size_t n = 0; n < 25; n++) {
    IBeaconData ib{};
    const std::string shortened = good.substr(0, n);
    CHECK(!parseIBeacon(asBinarySafe(shortened), ib),
          "a frame shorter than 25 bytes is refused");
  }

  IBeaconData ib{};
  std::string wrongCompany = good; wrongCompany[0] = '\x4D';
  CHECK(!parseIBeacon(asBinarySafe(wrongCompany), ib), "wrong company ID refused");
  std::string wrongType = good; wrongType[2] = '\x03';
  CHECK(!parseIBeacon(asBinarySafe(wrongType), ib), "wrong beacon type refused");
}

static void test_tlm_values_and_rejects() {
  EddystoneTlm t{};
  CHECK(parseEddystoneTlm(asBinarySafe(tlmPayload(3000, 0x1900, 42, 1234)), t), "TLM parses");
  CHECK(t.batteryMv == 3000, "battery in mV, big-endian");
  CHECK(std::fabs(t.temperatureC - 25.0f) < 0.01f, "temperature is 8.8 fixed point");
  CHECK(t.advCount == 42, "advertisement count is 32-bit big-endian");
  CHECK(t.uptimeSec == 123, "uptime converts from 0.1 s units");

  // Below freezing: 8.8 fixed point is SIGNED, and a coop sees this every winter.
  CHECK(parseEddystoneTlm(asBinarySafe(tlmPayload(2400, static_cast<int16_t>(0xF600), 1, 10)), t),
        "sub-zero TLM parses");
  CHECK(t.temperatureC < 0.0f, "a negative temperature stays negative");

  // 0 mV is mains power, not an error (beacon.h says so).
  CHECK(parseEddystoneTlm(asBinarySafe(tlmPayload(0, 0, 0, 0)), t), "0 mV parses");
  CHECK(t.batteryMv == 0, "0 mV is preserved, not treated as absent");

  const std::string good = tlmPayload(3000, 0, 0, 0);
  for (size_t n = 0; n < 14; n++) {
    EddystoneTlm s{};
    CHECK(!parseEddystoneTlm(asBinarySafe(good.substr(0, n)), s),
          "a TLM frame shorter than 14 bytes is refused");
  }
  std::string notTlm = good; notTlm[0] = '\x10';
  CHECK(!parseEddystoneTlm(asBinarySafe(notTlm), t), "a non-TLM frame type is refused");
}

static void test_uuid_parsing() {
  uint8_t out[16] = {0};
  CHECK(uuidFromString("E2C56DB5-DFFB-48D2-B060-D0F5A71096E0", out), "dashed UUID parses");
  CHECK(out[0] == 0xE2 && out[15] == 0xE0, "dashed UUID bytes are right");
  CHECK(uuidFromString("e2c56db5dffb48d2b060d0f5a71096e0", out), "undashed UUID parses");
  CHECK(!uuidFromString(nullptr, out), "nullptr is refused, not dereferenced");
  CHECK(!uuidFromString("", out), "empty is refused");
  CHECK(!uuidFromString("E2C56DB5", out), "too short is refused");
  CHECK(!uuidFromString("e2c56db5dffb48d2b060d0f5a71096e000", out), "too long is refused");
  CHECK(!uuidFromString("E2C56DB5-DFFB-48D2-B060-D0F5A71096EZ", out), "non-hex is refused");

  // Round-trip against the formatter.
  const uint8_t uuid[16] = {0xE2,0xC5,0x6D,0xB5,0xDF,0xFB,0x48,0xD2,
                            0xB0,0x60,0xD0,0xF5,0xA7,0x10,0x96,0xE0};
  uint8_t back[16] = {0};
  CHECK(uuidFromString(uuidToString(uuid).c_str(), back), "uuidToString output re-parses");
  CHECK(std::memcmp(uuid, back, 16) == 0, "uuid survives a format/parse round trip");
}

static void test_bytes_to_hex() {
  const uint8_t b[4] = {0x00, 0x0F, 0xA5, 0xFF};
  CHECK(bytesToHex(b, 4) == "000fa5ff", "bytesToHex is lower-case and zero-padded");
  CHECK(bytesToHex(b, 0).length() == 0, "zero length yields an empty string");
}

static void test_classify() {
  CHECK(classifyDevice("MINEW-S1", "", "") == DEV_MINEW, "a Minew name wins");
  CHECK(classifyDevice("mst01", "", "") == DEV_MINEW, "the MST01 model name wins");
  CHECK(classifyDevice("", "4c000215aabb", "") == DEV_IBEACON, "4c000215 is an iBeacon");
  CHECK(classifyDevice("", "4c00121900", "") == DEV_AIRTAG, "4c00 + 1219 is an AirTag");
  CHECK(classifyDevice("", "4c0099", "") == DEV_APPLE, "other 4c00 is generic Apple");
  CHECK(classifyDevice("", "", "feaa") == DEV_EDDYSTONE, "feaa is Eddystone");
  CHECK(classifyDevice("Chipolo", "", "") == DEV_CHIPOLO, "a Chipolo name");
  CHECK(classifyDevice("", "", "180d") == DEV_FITNESS, "180d is a heart-rate strap");
  CHECK(classifyDevice("", "", "1234") == DEV_GENERIC, "any other service UUID is generic");
  CHECK(classifyDevice("", "", "") == DEV_UNKNOWN, "nothing to go on is unknown");
}

static void test_distance_model() {
  // The model's own identity: the RSSI at 1 m IS measuredPower.
  CHECK(std::fabs(estimateDistanceM(-59, -59, 2.0f) - 1.0f) < 0.01f,
        "rssi == measuredPower means 1 metre");
  CHECK(estimateDistanceM(-80, -59, 2.0f) > 1.0f, "a weaker signal reads further");
  CHECK(estimateDistanceM(-40, -59, 2.0f) < 1.0f, "a stronger signal reads nearer");

  // Round-trip against the inverse.
  const int rssi = rssiAtDistanceM(4.0f, -59, 2.0f);
  CHECK(std::fabs(estimateDistanceM(rssi, -59, 2.0f) - 4.0f) < 0.2f,
        "rssiAtDistanceM inverts estimateDistanceM");
}

int main() {
  std::printf("beacon.cpp — host unit tests\n");
  test_ibeacon_happy();
  test_nul_truncation_is_the_bug();
  test_ibeacon_rejects();
  test_tlm_values_and_rejects();
  test_uuid_parsing();
  test_bytes_to_hex();
  test_classify();
  test_distance_model();
  std::printf("%s  %d checks, %d failed\n", g_failures ? "FAILED" : "ok    ",
              g_checks, g_failures);
  return g_failures ? 1 : 0;
}
