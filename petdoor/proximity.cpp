#include "proximity.h"

#include "beacon.h"

static_assert(RSSI_ENTER_DBM > RSSI_EXIT_DBM,
              "RSSI_ENTER_DBM must be greater (less negative) than RSSI_EXIT_DBM, "
              "otherwise there is no hysteresis band and the door will flap.");
static_assert(RSSI_MEDIAN_WINDOW % 2 == 1 && RSSI_MEDIAN_WINDOW >= 3 && RSSI_MEDIAN_WINDOW <= 15,
              "RSSI_MEDIAN_WINDOW must be an odd number between 3 and 15.");
static_assert(MIN_ACTUATION_INTERVAL_MS < EXIT_CONFIRM_MS,
              "MIN_ACTUATION_INTERVAL_MS must be shorter than EXIT_CONFIRM_MS, "
              "or the actuation lockout will delay the door closing.");

void ProximityTracker::begin() { reset(); }

bool ProximityTracker::setFilter(uint8_t windowSize, float alpha) {
  if (windowSize < 1 || windowSize > kMaxMedianWindow) return false;
  if (windowSize % 2 == 0) return false;  // needs a single middle element
  if (alpha <= 0.0f || alpha > 1.0f) return false;
  windowSize_ = windowSize;
  alpha_ = alpha;
  reset();  // the old window contents were sized for the old shape
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
  filteredRssi_ = -127;
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

  window_[windowHead_] = rssi;
  windowHead_ = static_cast<uint8_t>((windowHead_ + 1) % windowSize_);
  if (windowCount_ < windowSize_) windowCount_++;

  const int med = median();
  if (!ewmaInit_) {
    ewma_ = static_cast<float>(med);
    ewmaInit_ = true;
  } else {
    ewma_ = alpha_ * static_cast<float>(med) + (1.0f - alpha_) * ewma_;
  }
  filteredRssi_ = static_cast<int>(lroundf(ewma_));
}

int ProximityTracker::median() const {
  if (windowCount_ == 0) return -127;

  int sorted[kMaxMedianWindow];
  for (uint8_t i = 0; i < windowCount_; i++) sorted[i] = window_[i];

  // Insertion sort: windowCount_ is at most 15, so this is cheaper than
  // anything cleverer and has no allocation.
  for (uint8_t i = 1; i < windowCount_; i++) {
    const int key = sorted[i];
    int j = static_cast<int>(i) - 1;
    while (j >= 0 && sorted[j] > key) {
      sorted[j + 1] = sorted[j];
      j--;
    }
    sorted[j + 1] = key;
  }
  return sorted[windowCount_ / 2];
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
  const bool near = fix && filteredRssi_ >= enterDbm_;
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
  if (far) {
    if (!farSinceValid_) {
      farSinceMs_ = nowMs;
      farSinceValid_ = true;
    }
    if ((nowMs - farSinceMs_) >= exitConfirmMs_) {
      state_ = PRESENCE_ABSENT;
      farSinceValid_ = false;
    }
  } else {
    // Back inside the band (or above it): cancel the pending close.
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
