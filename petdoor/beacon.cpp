#include "beacon.h"

#include <math.h>
#include <string.h>

namespace {

const char kHexDigits[] = "0123456789abcdef";

int hexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

String bytesToHex(const uint8_t *data, size_t len) {
  String out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; i++) {
    out += kHexDigits[data[i] >> 4];
    out += kHexDigits[data[i] & 0x0F];
  }
  return out;
}

bool parseIBeacon(const String &mfgData, IBeaconData &out) {
  // 2 company + 1 type + 1 length + 16 uuid + 2 major + 2 minor + 1 power
  if (mfgData.length() < 25) return false;

  const uint8_t *p = reinterpret_cast<const uint8_t *>(mfgData.c_str());
  if (p[0] != 0x4C || p[1] != 0x00) return false;  // Apple company ID, LE
  if (p[2] != 0x02 || p[3] != 0x15) return false;  // iBeacon type + length

  memcpy(out.uuid, p + 4, 16);
  out.major = static_cast<uint16_t>(p[20]) << 8 | p[21];
  out.minor = static_cast<uint16_t>(p[22]) << 8 | p[23];
  out.measuredPower = static_cast<int8_t>(p[24]);
  return true;
}

String uuidToString(const uint8_t uuid[16]) {
  String out;
  out.reserve(36);
  for (int i = 0; i < 16; i++) {
    if (i == 4 || i == 6 || i == 8 || i == 10) out += '-';
    out += kHexDigits[uuid[i] >> 4];
    out += kHexDigits[uuid[i] & 0x0F];
  }
  return out;
}

bool uuidFromString(const char *text, uint8_t out[16]) {
  if (text == nullptr) return false;

  int nibbles = 0;
  uint8_t acc = 0;
  for (const char *c = text; *c != '\0'; c++) {
    if (*c == '-' || *c == ' ') continue;
    const int v = hexValue(*c);
    if (v < 0) return false;
    if (nibbles >= 32) return false;  // too long
    acc = static_cast<uint8_t>(acc << 4 | v);
    if (nibbles % 2 == 1) out[nibbles / 2] = acc;
    nibbles++;
  }
  return nibbles == 32;
}

bool parseEddystoneTlm(const String &svcData, EddystoneTlm &out) {
  if (svcData.length() < 14) return false;
  const uint8_t *p = reinterpret_cast<const uint8_t *>(svcData.c_str());
  if (p[0] != 0x20) return false;  // not a TLM frame

  out.batteryMv = static_cast<uint16_t>(p[2]) << 8 | p[3];

  // Temperature is 8.8 fixed point, signed.
  const int16_t rawTemp = static_cast<int16_t>(static_cast<uint16_t>(p[4]) << 8 | p[5]);
  out.temperatureC = static_cast<float>(rawTemp) / 256.0f;

  out.advCount = static_cast<uint32_t>(p[6]) << 24 | static_cast<uint32_t>(p[7]) << 16 |
                 static_cast<uint32_t>(p[8]) << 8 | p[9];
  const uint32_t deciSec = static_cast<uint32_t>(p[10]) << 24 |
                           static_cast<uint32_t>(p[11]) << 16 |
                           static_cast<uint32_t>(p[12]) << 8 | p[13];
  out.uptimeSec = deciSec / 10;
  return true;
}

DeviceClass classifyDevice(const String &name, const String &mfgHex, const String &svcUuid) {
  String lowerName = name;
  lowerName.toLowerCase();

  // Checked first: if it says Minew on the tin, that is the most useful label
  // to show while the user is hunting for their beacon.
  if (lowerName.indexOf("minew") >= 0 || lowerName.indexOf("mst01") >= 0) {
    return DEV_MINEW;
  }

  if (mfgHex.startsWith("4c00")) {
    if (mfgHex.startsWith("4c000215")) return DEV_IBEACON;
    if (mfgHex.indexOf("1219") >= 0) return DEV_AIRTAG;
    return DEV_APPLE;
  }

  if (svcUuid.indexOf("feaa") >= 0) return DEV_EDDYSTONE;
  if (lowerName.startsWith("chipolo") || svcUuid.indexOf("fd6f") >= 0) return DEV_CHIPOLO;
  if (svcUuid.indexOf("180d") >= 0) return DEV_FITNESS;
  if (svcUuid.length() > 0) return DEV_GENERIC;
  return DEV_UNKNOWN;
}

const char *deviceClassName(DeviceClass c) {
  switch (c) {
    case DEV_MINEW: return "Minew";
    case DEV_IBEACON: return "iBeacon";
    case DEV_EDDYSTONE: return "Eddystone";
    case DEV_AIRTAG: return "AirTag";
    case DEV_CHIPOLO: return "Chipolo";
    case DEV_APPLE: return "Apple";
    case DEV_FITNESS: return "Fitness";
    case DEV_GENERIC: return "Generic";
    default: return "Unknown";
  }
}

int rssiAtDistanceM(float metres, int8_t measuredPower, float pathLossExponent) {
  if (metres <= 0.0f || pathLossExponent <= 0.0f) return 0;
  const float rssi = static_cast<float>(measuredPower) -
                     10.0f * pathLossExponent * log10f(metres);
  return static_cast<int>(lroundf(rssi));
}

float estimateDistanceM(int rssi, int8_t measuredPower, float pathLossExponent) {
  if (rssi == 0 || measuredPower == 0) return -1.0f;
  if (pathLossExponent <= 0.0f) return -1.0f;
  const float ratio = (static_cast<float>(measuredPower) - static_cast<float>(rssi)) /
                      (10.0f * pathLossExponent);
  return powf(10.0f, ratio);
}
