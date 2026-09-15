// beacon.h — beacon payload parsing and device classification.
//
// Pure functions over advertisement bytes. No BLE stack dependency, so this
// file is the easy one to unit test or reuse.

#pragma once

#include <Arduino.h>

// Apple iBeacon advertisement body, as emitted by a Minew beacon in iBeacon
// mode and by most other beacon vendors.
struct IBeaconData {
  uint8_t uuid[16];
  uint16_t major;
  uint16_t minor;
  int8_t measuredPower;  // calibrated RSSI at 1 m, as programmed into the beacon
};

// Parses manufacturer-specific data. `mfgData` is raw bytes (it may contain
// NULs; Arduino String carries an explicit length, so that is safe) and starts
// with the 16-bit little-endian company ID.
//
// Layout: 4C 00 | 02 | 15 | <16-byte UUID> | <major BE> | <minor BE> | <power>
// Returns false if this is not an iBeacon frame.
bool parseIBeacon(const String &mfgData, IBeaconData &out);

String uuidToString(const uint8_t uuid[16]);

// Accepts "E2C56DB5-DFFB-48D2-B060-D0F5A71096E0" with or without dashes.
// Returns false unless exactly 32 hex digits were found.
bool uuidFromString(const char *text, uint8_t out[16]);

String bytesToHex(const uint8_t *data, size_t len);

// Eddystone-TLM telemetry, broadcast by most Eddystone beacons alongside their
// UID/URL frames. This is how a beacon reports its own battery.
//
// Service data for UUID 0xFEAA, frame type 0x20:
//   0x20 | version | VBATT(2, BE, mV) | TEMP(2, 8.8 signed) |
//   ADV_CNT(4, BE) | SEC_CNT(4, BE, 0.1s units)
//
// VBATT is 0 on mains-powered beacons, which is not an error.
struct EddystoneTlm {
  uint16_t batteryMv;
  float temperatureC;
  uint32_t advCount;
  uint32_t uptimeSec;
};

// `svcData` is the raw service-data payload for 0xFEAA. Returns false if this
// is not a TLM frame.
bool parseEddystoneTlm(const String &svcData, EddystoneTlm &out);

enum DeviceClass {
  DEV_UNKNOWN,
  DEV_MINEW,
  DEV_IBEACON,
  DEV_EDDYSTONE,
  DEV_AIRTAG,
  DEV_CHIPOLO,
  DEV_APPLE,
  DEV_FITNESS,
  DEV_GENERIC,
};

// Best-effort labelling to help you pick your beacon out of the discovery
// table. `mfgHex` and `svcUuid` are expected lower-case.
DeviceClass classifyDevice(const String &name, const String &mfgHex, const String &svcUuid);

const char *deviceClassName(DeviceClass c);

// Log-distance path loss model: d = 10 ^ ((measuredPower - rssi) / (10 * n)).
// Returns -1 when there is not enough information. Indicative only — BLE RSSI
// is far too noisy for this to be a real rangefinder.
float estimateDistanceM(int rssi, int8_t measuredPower, float pathLossExponent);

// Inverse of the above: the RSSI you would expect at `metres`.
//   rssi = measuredPower - 10 * n * log10(d)
// Lets a threshold be entered in metres and stored as the dBm the rest of the
// firmware actually decides on. Only as accurate as measuredPower is
// calibrated — see docs/TUNING.md.
int rssiAtDistanceM(float metres, int8_t measuredPower, float pathLossExponent);
