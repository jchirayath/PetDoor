#include "proximity.h"

#include <math.h>

#include "beacon.h"

static_assert(RSSI_ENTER_DBM > RSSI_EXIT_DBM,
              "RSSI_ENTER_DBM must be greater (less negative) than RSSI_EXIT_DBM, "
              "otherwise there is no hysteresis band and the door will flap.");
static_assert(RSSI_MEDIAN_WINDOW % 2 == 1 && RSSI_MEDIAN_WINDOW >= 3 && RSSI_MEDIAN_WINDOW <= 15,
              "RSSI_MEDIAN_WINDOW must be an odd number between 3 and 15.");
// The fast window may be 1 (no median at all), which the slow one may not: the
// open path is confirmed by ENTER_CONFIRM_MS rather than by smoothing.
static_assert(RSSI_FAST_WINDOW % 2 == 1 && RSSI_FAST_WINDOW >= 1 && RSSI_FAST_WINDOW <= 15,
              "RSSI_FAST_WINDOW must be an odd number between 1 and 15.");
static_assert(RSSI_FAST_ALPHA > 0.0f && RSSI_FAST_ALPHA <= 1.0f,
              "RSSI_FAST_ALPHA must be in (0, 1].");
static_assert(RSSI_FAST_WINDOW <= RSSI_MEDIAN_WINDOW && RSSI_FAST_ALPHA >= RSSI_EWMA_ALPHA,
              "The fast filter must not be slower than the slow one, or the open "
              "decision would lag the close decision and the asymmetry inverts.");
static_assert(MIN_ACTUATION_INTERVAL_MS < EXIT_CONFIRM_MS,
              "MIN_ACTUATION_INTERVAL_MS must be shorter than EXIT_CONFIRM_MS, "
              "or the actuation lockout will delay the door closing.");

void ProximityTracker::begin() { reset(); }

bool ProximityTracker::setFilter(uint8_t windowSize, float alpha) {
  if (windowSize < 1 || windowSize > kMaxMedianWindow) return false;
  if (windowSize % 2 == 0) return false;  // needs a single middle element
  // NaN compares false against everything, so both bounds checks would pass it
  // and the EWMA would be poisoned permanently. Reject non-finite explicitly.
  if (!isfinite(alpha) || alpha <= 0.0f || alpha > 1.0f) return false;
  windowSize_ = windowSize;
  alpha_ = alpha;

  // Keep the open path from ever becoming the slower of the two. Shrinking the
  // close window below the open window, or raising the close alpha above the
  // open alpha, would otherwise invert the asymmetry silently — and it is the
  // close filter being edited, so refusing the edit would be the wrong answer.
  // Drag the fast pair along instead; callers report it (see applyFilter()).
  if (fastWindow_ > windowSize_) fastWindow_ = windowSize_;
  if (fastAlpha_ < alpha_) fastAlpha_ = alpha_;

  // Both filters share one fixed-size ring, so the buffer itself survives a
  // shape change. The reset is kept for a different reason: it drops the fix,
  // forcing MIN_SAMPLES_FOR_FIX fresh samples before the door can act on a
  // filter nobody has observed yet.
  reset();
  return true;
}

bool ProximityTracker::setFastFilter(uint8_t windowSize, float alpha) {
  // Same validation as setFilter(), except a window of 1 is allowed here.
  if (windowSize < 1 || windowSize > kMaxMedianWindow) return false;
  if (windowSize % 2 == 0) return false;
  if (!isfinite(alpha) || alpha <= 0.0f || alpha > 1.0f) return false;

  // The ordering invariant is enforced here rather than by the caller, so that
  // it cannot be sidestepped — including by a stale pair restored from NVS at
  // boot, which no console code path ever inspects.
  //
  // This direction REFUSES rather than clamping, the opposite of setFilter():
  // here it is the open pair being edited, so silently substituting a different
  // open latency than the one asked for would be the surprising answer.
  if (windowSize > windowSize_ || alpha < alpha_) return false;

  fastWindow_ = windowSize;
  fastAlpha_ = alpha;

  // Deliberately no reset(): the ring is shared and still valid, and dropping
  // the fix here would stall the open path for MIN_SAMPLES_FOR_FIX samples —
  // the exact delay this filter exists to remove. Re-seed from what we have.
  if (ewmaInit_) {
    fastEwma_ = static_cast<float>(median(fastWindow_));
    fastRssi_ = static_cast<int>(lroundf(fastEwma_));
  }
  return true;
}

bool ProximityTracker::setThresholds(int enterDbm, int exitDbm) {
  // Same invariant the static_assert enforces for the compile-time defaults:
  // the gap between the two IS the hysteresis band.
  if (enterDbm <= exitDbm) return false;
  enterDbm_ = enterDbm;
  exitDbm_ = exitDbm;
  return true;
}

void ProximityTracker::reset() {
  windowCount_ = 0;
  windowHead_ = 0;
  ewmaInit_ = false;
  ewma_ = 0.0f;
  fastEwma_ = 0.0f;
  filteredRssi_ = -127;
  fastRssi_ = -127;
  rawRssi_ = -127;
  measuredPower_ = 0;
  lastSeenMs_ = 0;
  totalSamples_ = 0;
  maxGapMs_ = 0;
  minRssi_ = 0;
  weakSamples_ = 0;
  state_ = PRESENCE_ABSENT;
  nearSinceValid_ = false;
  farSinceValid_ = false;
}

void ProximityTracker::addSample(int rssi, int8_t measuredPower, uint32_t atMs) {
  // Record the gap before lastSeenMs_ moves. Skipped for the first sample,
  // which has nothing to measure from.
  if (totalSamples_ > 0 && atMs > lastSeenMs_) {
    const uint32_t gap = atMs - lastSeenMs_;
    if (gap > maxGapMs_) maxGapMs_ = gap;
  }

  rawRssi_ = rssi;
  if (minRssi_ == 0 || rssi < minRssi_) minRssi_ = rssi;
  if (rssi <= -85) weakSamples_++;
  if (measuredPower != 0) measuredPower_ = measuredPower;
  lastSeenMs_ = atMs;
  if (totalSamples_ < UINT32_MAX) totalSamples_++;

  // The ring always advances by the buffer size, never by the configured
  // window: both filters read from it and they ask for different amounts.
  window_[windowHead_] = rssi;
  windowHead_ = static_cast<uint8_t>((windowHead_ + 1) % kMaxMedianWindow);
  if (windowCount_ < kMaxMedianWindow) windowCount_++;

  const int slowMed = median(windowSize_);
  const int fastMed = median(fastWindow_);
  if (!ewmaInit_) {
    ewma_ = static_cast<float>(slowMed);
    fastEwma_ = static_cast<float>(fastMed);
    ewmaInit_ = true;
  } else {
    ewma_ = alpha_ * static_cast<float>(slowMed) + (1.0f - alpha_) * ewma_;
    fastEwma_ = fastAlpha_ * static_cast<float>(fastMed) + (1.0f - fastAlpha_) * fastEwma_;
  }
  filteredRssi_ = static_cast<int>(lroundf(ewma_));
  fastRssi_ = static_cast<int>(lroundf(fastEwma_));
}

int ProximityTracker::median(uint8_t count) const {
  // Early in a session fewer samples have arrived than the window asks for.
  const uint8_t n = (count < windowCount_) ? count : windowCount_;
  if (n == 0) return -127;

  int sorted[kMaxMedianWindow];
  for (uint8_t i = 0; i < n; i++) {
    // window_ is a ring and windowHead_ points at the slot the NEXT sample will
    // take, so the most recent sample is one slot behind it. Walking backwards
    // is what makes "the last n samples" mean the same thing to both filters
    // regardless of the window each one uses.
    const uint8_t idx =
        static_cast<uint8_t>((windowHead_ + kMaxMedianWindow - 1 - i) % kMaxMedianWindow);
    sorted[i] = window_[idx];
  }

  // Insertion sort: n is at most 15, so this is cheaper than
  // anything cleverer and has no allocation.
  for (uint8_t i = 1; i < n; i++) {
    const int key = sorted[i];
    int j = static_cast<int>(i) - 1;
    while (j >= 0 && sorted[j] > key) {
      sorted[j + 1] = sorted[j];
      j--;
    }
    sorted[j + 1] = key;
  }
  return sorted[n / 2];
}

bool ProximityTracker::hasFix(uint32_t nowMs) const {
  if (totalSamples_ < MIN_SAMPLES_FOR_FIX) return false;

  // Clamped: a sample stamped a millisecond into the future is the freshest
  // reading there is, not a 49-day-old one. Getting this wrong made hasFix()
  // briefly false, which cancels a pending open and delays the door.
  //
  // This does not weaken the "stale can close but never open" rule — a
  // genuinely stale sample still has lastSeenMs_ < nowMs and still reads
  // stale.
  return sampleAgeMs(nowMs) <= SAMPLE_MAX_AGE_MS;
}

void ProximityTracker::update(uint32_t nowMs) {
  const bool fix = hasFix(nowMs);

  // A stale signal counts as far but never as near, so losing the beacon can
  // only ever close the door, never open it.
  //
  // The two tests read different filters on purpose. `near` takes the fast one
  // so an arriving animal is noticed as soon as the signal is genuinely strong;
  // `far` takes the slow one so a fade or a dropped advertisement cannot talk
  // the door into closing. Both directions still have to survive their dwell
  // timer, which is what turns a fast filter into a safe one.
  const bool near = fix && fastRssi_ >= enterDbm_;
  const bool far = !fix || filteredRssi_ <= exitDbm_;

  if (state_ == PRESENCE_ABSENT) {
    farSinceValid_ = false;
    if (near) {
      if (!nearSinceValid_) {
        nearSinceMs_ = nowMs;
        nearSinceValid_ = true;
      }
      if ((nowMs - nearSinceMs_) >= enterConfirmMs_) {
        state_ = PRESENCE_PRESENT;
        nearSinceValid_ = false;
      }
    } else {
      nearSinceValid_ = false;
    }
    return;
  }

  // PRESENCE_PRESENT
  nearSinceValid_ = false;

  // `!near` is what makes a return instant. Without it a beacon coming back
  // mid-close would have to wait for the SLOW filter to climb back over the
  // exit threshold before the pending close was cancelled — seconds during
  // which the door is already travelling shut on an animal walking into it.
  // Reading the fast filter here cancels on the first strong advertisement.
  if (far && !near) {
    if (!farSinceValid_) {
      farSinceMs_ = nowMs;
      farSinceValid_ = true;
    }
    if ((nowMs - farSinceMs_) >= exitConfirmMs_) {
      state_ = PRESENCE_ABSENT;
      farSinceValid_ = false;
    }
  } else {
    // Back inside the band, above it, or strong on the fast filter: cancel the
    // pending close.
    farSinceValid_ = false;
  }
}

float ProximityTracker::distanceM() const {
  if (!ewmaInit_) return -1.0f;

  // Prefer the beacon's own calibrated power when it advertises one (iBeacon).
  // Eddystone and sensor tags do not, so fall back to the configured value —
  // otherwise those beacons could never show a distance at all.
  const int8_t refPower =
      (measuredPower_ != 0) ? measuredPower_
                            : static_cast<int8_t>(BEACON_MEASURED_POWER_DBM);

  return estimateDistanceM(filteredRssi_, refPower, PATH_LOSS_EXPONENT);
}

uint32_t ProximityTracker::pendingEnterMs(uint32_t nowMs) const {
  return nearSinceValid_ ? (nowMs - nearSinceMs_) : 0;
}

uint32_t ProximityTracker::pendingExitMs(uint32_t nowMs) const {
  return farSinceValid_ ? (nowMs - farSinceMs_) : 0;
}
