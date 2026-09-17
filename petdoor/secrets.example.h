// secrets.example.h — copy to secrets.h and edit.
//
//   cp petdoor/secrets.example.h petdoor/secrets.h
//
// secrets.h is git-ignored. Anything you define here overrides the default in
// config.h, so `git pull` never clobbers your settings and your beacon's
// address never lands in the public repo.

#pragma once

// ---------------------------------------------------------------------------
// Your beacon's MAC address.
// Flash the firmware with nothing set here, open the serial monitor at 115200,
// and read it out of the discovery table. See docs/BEACON-SETUP.md.
// ---------------------------------------------------------------------------
// #define BEACON_MAC "aa:bb:cc:dd:ee:ff"

// ---------------------------------------------------------------------------
// Optional: match on the iBeacon identity instead of / as well as the MAC.
// Handy if you want a spare beacon you can swap in without reflashing.
// All configured matchers must match.
// ---------------------------------------------------------------------------
// #define BEACON_UUID  "E2C56DB5-DFFB-48D2-B060-D0F5A71096E0"
// #define BEACON_MAJOR 1      // -1 for "any"
// #define BEACON_MINOR 1      // -1 for "any"

// ---------------------------------------------------------------------------
// Optional: upload the event log over WiFi.
//
// Leave these unset and the radio is NEVER brought up — no WiFi, no cloud,
// which is the default behaviour.
//
// The ESP32 shares one antenna between WiFi and BLE, so uploads are deferred
// until the beacon is absent and the door is closed. Expect the log to reach
// your endpoint a minute or two after your pet leaves, not instantly.
// ---------------------------------------------------------------------------
// #define WIFI_SSID        "your-network"
// #define WIFI_PASSWORD    "your-password"
// #define LOG_ENDPOINT_URL "http://192.168.1.50:8080/petdoor"

// ---------------------------------------------------------------------------
// Anything else from config.h can be overridden here too, e.g.:
// ---------------------------------------------------------------------------
// #define RSSI_ENTER_DBM   -60
// #define RSSI_EXIT_DBM    -72
// #define RELAY_ACTIVE_LOW 1
