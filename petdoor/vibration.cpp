#include "vibration.h"

namespace Vibration {
namespace {

int g_pin = -1;
bool g_activeLow = false;
bool g_attached = false;

// Written by the ISR, read by the control task. 32-bit aligned loads and stores
// are atomic on this chip, so a plain volatile is enough here — there is no
// read-modify-write outside the ISR.
volatile uint32_t g_pulses = 0;

uint32_t g_baseline = 0;
uint32_t g_blankUntilMs = 0;
bool g_travelActive = false;

void IRAM_ATTR onEdge() { g_pulses++; }

void detach() {
  if (g_attached && g_pin >= 0) detachInterrupt(digitalPinToInterrupt(g_pin));
  g_attached = false;
}

}  // namespace

const char *pinProblem(int p) {
  if (p < 0) return nullptr;
  if (!digitalPinIsValid(p)) return "not a pin on this chip";
  // 6-11 are the SPI flash. Saying so is worth more than letting somebody
  // discover it by bricking the boot.
  if (p >= 6 && p <= 11) return "wired to the SPI flash — using it stops the board booting";
  if (p == 0 || p == 2 || p == 12 || p == 15) {
    return "a strapping pin: it will work, but a pull-up or pull-down here can stop the board booting";
  }
  return nullptr;
}

bool enabled() { return g_pin >= 0; }
int pin() { return g_pin; }
bool activeLow() { return g_activeLow; }
uint32_t pulses() { return g_pulses; }

uint32_t pulsesThisTravel() {
  const uint32_t now = g_pulses;
  return now >= g_baseline ? now - g_baseline : 0;
}

void begin(int p, bool activeLow) { configure(p, activeLow); }

bool configure(int p, bool activeLow) {
  detach();
  g_pin = -1;
  if (p < 0) return true;             // disabling is always allowed
  const char *problem = pinProblem(p);
  if (problem != nullptr && !digitalPinIsValid(p)) return false;
  if (p >= 6 && p <= 11) return false;

  g_pin = p;
  g_activeLow = activeLow;
  // INPUT_PULLUP covers the common case of an open-collector comparator output
  // with no pull-up of its own. Harmless when the module supplies one.
  pinMode(g_pin, activeLow ? INPUT_PULLUP : INPUT);
  // CHANGE rather than an edge: these modules chatter in both directions as the
  // spring makes and breaks, and every transition is evidence of movement.
  attachInterrupt(digitalPinToInterrupt(g_pin), onEdge, CHANGE);
  g_attached = true;
  g_pulses = 0;
  g_baseline = 0;
  return true;
}

void travelStarted(uint32_t nowMs) {
  if (!enabled()) return;
  g_travelActive = true;
  // The relay's own click, and the structure ringing from it, arrive first and
  // are not the door moving. Discount everything until the window closes; tick()
  // re-baselines continuously until then.
  g_blankUntilMs = nowMs + VIBRATION_BLANK_MS;
  g_baseline = g_pulses;
}

void tick(uint32_t nowMs) {
  if (!enabled() || !g_travelActive) return;
  if (static_cast<int32_t>(nowMs - g_blankUntilMs) < 0) {
    // Still inside the blanking window: keep discarding.
    g_baseline = g_pulses;
  }
}

bool movedThisTravel() {
  if (!enabled()) return false;
  return pulsesThisTravel() >= VIBRATION_MIN_PULSES;
}

}  // namespace Vibration
