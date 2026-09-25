#include "maintenance.h"

namespace Maintenance {
namespace {

// The window, held as an absolute deadline rather than a remaining time so that
// nothing has to be decremented on a tick — a missed tick during a long WiFi
// operation cannot extend the window.
uint32_t g_endsAtMs = 0;
bool g_active = false;
bool g_expiredPending = false;
uint32_t g_startedAtMs = 0;

// RSSI distribution as a histogram rather than a buffer of samples. A window
// can legitimately run for an hour at ~2 samples/second, which is far more
// samples than this chip has RAM to keep — but the bins are fixed at 240 bytes
// no matter how long you leave it running, and percentiles come straight out of
// them. RSSI is an int8 in the advertisement, so the range below cannot
// overflow it in either direction.
constexpr int kMinDbm = -120;
constexpr int kBins = 120;  // covers -120 .. -1 dBm
uint16_t g_bins[kBins] = {0};
uint32_t g_count = 0;
int32_t g_sum = 0;
int16_t g_min = 0, g_max = 0;

// Nearest-rank percentile, walked out of the cumulative histogram.
int16_t percentile(uint8_t p) {
  if (g_count == 0) return 0;
  // ceil(p * n / 100) without floating point.
  uint32_t target = (static_cast<uint32_t>(p) * g_count + 99) / 100;
  if (target == 0) target = 1;
  uint32_t cum = 0;
  for (int i = 0; i < kBins; ++i) {
    cum += g_bins[i];
    if (cum >= target) return static_cast<int16_t>(kMinDbm + i);
  }
  return g_max;
}

}  // namespace

bool start(uint32_t nowMs, uint32_t durationMs, const char *&problem) {
  if (durationMs == 0) durationMs = MAINT_DEFAULT_MS;
  if (durationMs < MAINT_MIN_MS) {
    problem = "window too short to walk the boundary";
    return false;
  }
  // Refused rather than clamped. Silently shortening a window somebody asked
  // for means the door starts moving while they are still standing at it,
  // which is precisely the surprise this mode exists to prevent.
  if (durationMs > MAINT_MAX_MS) {
    problem = "window longer than the maximum; the door must not stay inert";
    return false;
  }
  g_active = true;
  g_startedAtMs = nowMs;
  g_endsAtMs = nowMs + durationMs;
  g_expiredPending = false;
  resetStats();
  problem = nullptr;
  return true;
}

void stop() {
  g_active = false;
  g_endsAtMs = 0;
  // Deliberately NOT flagged as expired: this path is somebody explicitly
  // ending the window, and they do not need to be told it ended.
  g_expiredPending = false;
}

bool active(uint32_t nowMs) {
  if (!g_active) return false;
  // Signed comparison so the millis() rollover at 49 days is handled; an
  // unsigned test would make the window appear to last another 49 days.
  if (static_cast<int32_t>(nowMs - g_endsAtMs) >= 0) {
    g_active = false;
    g_expiredPending = true;
    return false;
  }
  return true;
}

uint32_t remainingMs(uint32_t nowMs) {
  if (!g_active) return 0;
  const int32_t left = static_cast<int32_t>(g_endsAtMs - nowMs);
  return left > 0 ? static_cast<uint32_t>(left) : 0;
}

bool consumeExpired(uint32_t nowMs) {
  active(nowMs);  // may set the flag
  if (!g_expiredPending) return false;
  g_expiredPending = false;
  return true;
}

void addSample(uint32_t nowMs, int rssi) {
  if (!active(nowMs)) return;
  if (rssi >= 0 || rssi < kMinDbm) return;  // not a plausible advertisement
  const int idx = rssi - kMinDbm;
  if (idx < 0 || idx >= kBins) return;
  if (g_bins[idx] != 0xFFFF) g_bins[idx]++;
  if (g_count == 0) {
    g_min = g_max = static_cast<int16_t>(rssi);
  } else {
    if (rssi < g_min) g_min = static_cast<int16_t>(rssi);
    if (rssi > g_max) g_max = static_cast<int16_t>(rssi);
  }
  g_count++;
  g_sum += rssi;
}

void resetStats() {
  memset(g_bins, 0, sizeof(g_bins));
  g_count = 0;
  g_sum = 0;
  g_min = g_max = 0;
}

bool stats(Stats &out) {
  if (g_count == 0) return false;
  out.n = g_count;
  out.min = g_min;
  out.max = g_max;
  out.p5 = percentile(5);
  out.median = percentile(50);
  out.p95 = percentile(95);
  out.meanX10 = static_cast<int16_t>((g_sum * 10) / static_cast<int32_t>(g_count));
  return true;
}

String summary(uint32_t nowMs) {
  char buf[160];
  Stats s;
  if (!stats(s)) {
    snprintf(buf, sizeof(buf), "maint left=%lus n=0 (no beacon heard yet)",
             static_cast<unsigned long>(remainingMs(nowMs) / 1000));
    return String(buf);
  }
  snprintf(buf, sizeof(buf),
           "maint left=%lus n=%lu min=%d p5=%d med=%d p95=%d max=%d mean=%d.%d",
           static_cast<unsigned long>(remainingMs(nowMs) / 1000),
           static_cast<unsigned long>(s.n), s.min, s.p5, s.median, s.p95, s.max,
           static_cast<int>(s.meanX10 / 10), abs(s.meanX10 % 10));
  return String(buf);
}

}  // namespace Maintenance
