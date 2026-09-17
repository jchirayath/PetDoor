// proximity.h — turns a noisy stream of RSSI samples into a stable
// present/absent decision.
//
// Three stages, each fixing a different failure mode:
//
//   1. Median filter   kills isolated spikes and deep multipath fades, which
//                      are the dominant noise in BLE RSSI.
//   2. EWMA            smooths what is left without adding much lag.
//   3. Hysteresis +    two thresholds and two dwell timers, so the door never
//      dwell timers     flaps at the boundary and rides out short dropouts.
//
// Not thread safe. Only the control task touches it; the BLE callback hands
// samples over through a queue. See ble_scanner.h.

#pragma once

#include <Arduino.h>

#include "config.h"

// The median window is adjustable at runtime, so the buffer is sized for the
// largest value setFilter() will accept — not for RSSI_MEDIAN_WINDOW, which is
// merely the default. Sizing it to the default made every preset above it
// silently unreachable.
static constexpr uint8_t kMaxMedianWindow = 15;

enum PresenceState {
  PRESENCE_ABSENT,
  PRESENCE_PRESENT,
};

class ProximityTracker {
 public:
  void begin();
  void reset();

  // Thresholds are runtime values, seeded from RSSI_ENTER_DBM / RSSI_EXIT_DBM
  // but overridable from the console and persisted in NVS. The compile-time
  // static_assert still guards the defaults; setThresholds() enforces the same
  // rule for anything entered later.
  int enterDbm() const { return enterDbm_; }
  int exitDbm() const { return exitDbm_; }

  // Returns false (changing nothing) unless enter > exit, i.e. there is a real
  // hysteresis band. Without one the door flaps at the boundary.
  bool setThresholds(int enterDbm, int exitDbm);

  // Dwell times, seeded from ENTER_CONFIRM_MS / EXIT_CONFIRM_MS.
  //
  // Shortening the exit dwell makes closing more responsive at the cost of the
  // protection it exists to provide: it is what rides out a BLE dropout, and a
  // dropout shorter than this can no longer close the door on an animal. Check
  // `worst gap` in the status output before lowering it — anything at or below
  // that figure will close on a routine gap.
  uint32_t enterConfirmMs() const { return enterConfirmMs_; }
  uint32_t exitConfirmMs() const { return exitConfirmMs_; }
  // Filter shape. The median window is capped by RSSI_MEDIAN_WINDOW (the array
  // is sized at compile time); only the first `windowSize_` slots are used.
  //
  // A smaller window and a higher alpha track reality faster, at the cost of
  // the smoothing that stops the door reacting to a single spike. window=1,
  // alpha=1.0 disables filtering entirely and switches on raw RSSI — which is
  // exactly the behaviour this project exists to fix. Do that only knowingly.
  uint8_t windowSize() const { return windowSize_; }
  float alpha() const { return alpha_; }
  bool setFilter(uint8_t windowSize, float alpha);

  void setDwell(uint32_t enterMs, uint32_t exitMs) {
    enterConfirmMs_ = enterMs;
    exitConfirmMs_ = exitMs;
  }

  // Feed one advertisement. `atMs` is when it was received.
  void addSample(int rssi, int8_t measuredPower, uint32_t atMs);

  // Re-evaluate the state machine. Call every control tick, even when no new
  // samples arrived — that is how a beacon going silent is noticed.
  void update(uint32_t nowMs);

  PresenceState state() const { return state_; }
  bool isPresent() const { return state_ == PRESENCE_PRESENT; }

  // True when there are enough recent samples to trust the reading.
  bool hasFix(uint32_t nowMs) const;

  int filteredRssi() const { return filteredRssi_; }
  int rawRssi() const { return rawRssi_; }
  int8_t measuredPower() const { return measuredPower_; }
  float distanceM() const;
  uint32_t lastSeenMs() const { return lastSeenMs_; }

  // Milliseconds since the last sample, clamped at 0.
  //
  // Always prefer this over `nowMs - lastSeenMs()`. lastSeenMs_ carries the
  // millis() the BLE callback stamped on the sample, which can be a few ms
  // AHEAD of the `nowMs` the control task sampled at the top of its tick. The
  // bare subtraction then underflows to ~2^32 and a brand-new sample reads as
  // ancient.
  uint32_t sampleAgeMs(uint32_t nowMs) const {
    // Unsigned subtraction is already correct across the millis() wrap; the
    // ONLY thing needing special handling is the few milliseconds of task skew
    // where a sample is stamped just ahead of nowMs.
    //
    // Do NOT write `(nowMs > lastSeenMs_) ? ... : 0` — that looks equivalent
    // but silently reports a sample from before a wrap as brand new, which
    // would let a long-dead beacon read as present and open the door.
    const int32_t delta = static_cast<int32_t>(nowMs - lastSeenMs_);
    return (delta > 0) ? static_cast<uint32_t>(delta) : 0;
  }
  uint32_t totalSamples() const { return totalSamples_; }

  // Longest observed gap between consecutive samples. This is the number that
  // tells you whether a beacon advertises fast enough: compare it against
  // SAMPLE_MAX_AGE_MS (a longer gap means "no fix", which counts as FAR) and
  // EXIT_CONFIRM_MS (a gap this long lets the close path complete).
  uint32_t maxGapMs() const { return maxGapMs_; }

  // Weakest raw RSSI ever heard since the last reset, and how many samples
  // arrived at or below -85 dBm (the band where packet loss starts to bite).
  //
  // Together with maxGapMs() this is a self-service range test: press `r` to
  // reset, carry the beacon as far as it will go, come back, and read `s`.
  // No second person and no shouting across the yard required.
  int minRssi() const { return minRssi_; }
  uint32_t weakSamples() const { return weakSamples_; }
  bool everSeen() const { return totalSamples_ > 0; }

  // Milliseconds the pending transition has been building, 0 when idle.
  // Only useful for diagnostics.
  uint32_t pendingEnterMs(uint32_t nowMs) const;
  uint32_t pendingExitMs(uint32_t nowMs) const;

 private:
  int median() const;

  int window_[kMaxMedianWindow] = {0};
  uint8_t windowCount_ = 0;
  uint8_t windowHead_ = 0;

  float ewma_ = 0.0f;
  bool ewmaInit_ = false;

  int filteredRssi_ = -127;
  int rawRssi_ = -127;
  int8_t measuredPower_ = 0;

  uint32_t lastSeenMs_ = 0;
  uint32_t totalSamples_ = 0;
  uint32_t maxGapMs_ = 0;
  int minRssi_ = 0;          // 0 == nothing heard yet
  uint32_t weakSamples_ = 0;

  PresenceState state_ = PRESENCE_ABSENT;

  int enterDbm_ = RSSI_ENTER_DBM;
  int exitDbm_ = RSSI_EXIT_DBM;
  uint8_t windowSize_ = RSSI_MEDIAN_WINDOW;
  float alpha_ = RSSI_EWMA_ALPHA;
  uint32_t enterConfirmMs_ = ENTER_CONFIRM_MS;
  uint32_t exitConfirmMs_ = EXIT_CONFIRM_MS;

  uint32_t nearSinceMs_ = 0;
  bool nearSinceValid_ = false;
  uint32_t farSinceMs_ = 0;
  bool farSinceValid_ = false;
};
