// console.h — the serial console, also reachable over WiFi.
//
// Everything the console can do — read the live RSSI stream, edit thresholds,
// dump the event log — was until now reachable only down a USB cable. That is
// fine on a bench and useless once the door is screwed to a wall, which is
// precisely where the numbers that matter have to be measured: the ESP32's
// antenna is a PCB trace and it is directional, so a threshold calibrated at a
// desk describes the desk.
//
// This routes console I/O through one Stream that writes to BOTH the UART and,
// when one is connected, a network client. Nothing about the console's own code
// changes; it prints to `Con` instead of `Serial` and is otherwise unaware.
//
// WHY THIS IS NOT LISTENING ALL THE TIME
//
// This console can open your door. Telnet has no authentication, no transport
// security, and no notion of a session — so the safety here comes from two
// things stacked, neither sufficient alone:
//
//   1. IT ONLY LISTENS DURING A MAINTENANCE WINDOW. Maintenance windows expire
//      by themselves (see maintenance.h), so the port cannot be left open by
//      forgetting. When the window lapses the server stops and any connected
//      client is dropped mid-session.
//
//   2. A PASSWORD IS REQUIRED BEFORE ANY BYTE REACHES THE CONSOLE. Until it
//      arrives, input is swallowed and no output is sent — a client that cannot
//      authenticate learns nothing about the door, not even its status banner.
//
// It is still plaintext on your LAN. That is an acceptable trade for a bounded
// window on a home network and NOT acceptable as a permanent service, which is
// the whole reason it is bound to the window. Do not "improve" this by starting
// the server at boot.

#pragma once

#include <Arduino.h>

#include "config.h"

#if PETDOOR_ENABLE_WIFI

// A Stream that fans output out to the UART and to at most one network client,
// and takes input from whichever has something to say. The console reads and
// writes this and never knows which it is talking to.
class ConsoleStream : public Stream {
 public:
  // Forwarded to the real UART. The network side is started separately, by the
  // maintenance window.
  void begin(unsigned long baud);

  size_t write(uint8_t c) override;
  size_t write(const uint8_t *buf, size_t size) override;
  int available() override;
  int read() override;
  int peek() override;
  void flush() override;

  // True while an authenticated client is attached, for the status display.
  bool networkAttached() const;
  IPAddress clientIp() const;
};

extern ConsoleStream Con;

namespace NetConsole {

// Starts listening. Idempotent. Only the maintenance window should call this.
void start();

// Stops listening and drops any client. Idempotent.
void stop();

bool listening();

// Accepts connections, runs the password exchange, and reaps dead clients.
// Called from the control task; never blocks.
void tick(uint32_t nowMs);

}  // namespace NetConsole

#else   // !PETDOOR_ENABLE_WIFI

// Without WiFi the console is the UART and nothing else, so `Con` is just an
// alias and the whole fan-out disappears at compile time.
#define Con Serial

namespace NetConsole {
inline void start() {}
inline void stop() {}
inline bool listening() { return false; }
inline void tick(uint32_t) {}
}  // namespace NetConsole

#endif  // PETDOOR_ENABLE_WIFI
