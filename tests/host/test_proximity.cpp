// Host tests for petdoor/proximity.cpp — the filters and the presence state
// machine that decide whether the door opens.
//
// WHY THIS FILE EXISTS
//
// proximity.* is the other half of the firmware that is pure logic, and CLAUDE.md
// names it as the obvious next thing to test after beacon.*. It is also where the
// safety invariants with the worst failure modes live, and every one of them
// fails SILENTLY — a door that opens a little too eagerly, or closes on a
// returning animal, looks exactly like a door that is working.
//
// The four this file is really about:
//
//   3. A stale signal can close the door but never open it.
//   4. The open path reads the FAST filter and the close path the SLOW one, the
//      fast pair may never be slower than the slow pair, and !near cancels a
//      pending close so a returning animal is noticed immediately.
//   5. Never actuate before the beacon has been heard once since boot — a dead
//      beacon battery must not read as "absent" and shut the door.
//   1. ENTER > EXIT. The gap IS the hysteresis band.
//
// Plus the millis() wrap in sampleAgeMs(), which CONTRIBUTING.md calls out
// specifically: the obvious-looking `(now > then) ? now - then : 0` reports a
// sample from before the wrap as brand new, which would let a long-dead beacon
// read as present and open the door.
//
// EVERYTHING IS CONFIGURED EXPLICITLY. config.h pulls in secrets.h when one
// exists, so relying on compile-time defaults would make these tests depend on
// whoever is running them — and fail differently in CI, which has no secrets.h.

#include <cstdio>
#include <cstdint>

#include "../../petdoor/proximity.h"

static int g_fails = 0;
static int g_checks = 0;

static void check(bool cond, const char *what) {
  ++g_checks;
  if (!cond) {
    ++g_fails;
    std::printf("  FAIL  %s\n", what);
  }
}

// Thresholds and dwells used throughout, so no test depends on a default.
static const int kEnter = -60;
static const int kExit = -72;
static const uint32_t kEnterDwell = 1500;
static const uint32_t kExitDwell = 15000;

static void configure(ProximityTracker &t) {
  t.begin();
  t.reset();
  check(t.setThresholds(kEnter, kExit), "setup: thresholds accepted");
  t.setDwell(kEnterDwell, kExitDwell);
}

// Feed `n` samples at `rssi`, one per `stepMs`, starting at `atMs`. Returns the
// time of the last sample.
static uint32_t feed(ProximityTracker &t, int rssi, int n, uint32_t atMs,
                     uint32_t stepMs = 100) {
  uint32_t when = atMs;
  for (int i = 0; i < n; ++i) {
    t.addSample(rssi, 0, when);
    t.update(when);
    if (i + 1 < n) when += stepMs;
  }
  return when;
}

// ---------------------------------------------------------------------------
// Invariant 1 — the hysteresis band
// ---------------------------------------------------------------------------
static void test_thresholds_must_keep_a_hysteresis_band() {
  ProximityTracker t;
  configure(t);

  check(!t.setThresholds(-80, -60),
        "inverted thresholds are refused — the gap IS the hysteresis band");
  check(!t.setThresholds(-60, -60),
        "equal thresholds are refused: no band means the door flaps");
  check(t.enterDbm() == kEnter && t.exitDbm() == kExit,
        "a refused pair leaves the previous one in place");
  check(t.setThresholds(-55, -70), "a valid pair is accepted");
}

// ---------------------------------------------------------------------------
// Invariant 5 — nothing may happen before the beacon has been heard
// ---------------------------------------------------------------------------
static void test_silence_at_boot_is_not_absence() {
  ProximityTracker t;
  configure(t);

  check(!t.everSeen(), "a door that has heard nothing says so");
  check(!t.hasFix(0), "and has no fix");
  check(!t.isPresent(), "and is not present");

  // The danger is the OTHER reading: that "never heard" is treated as "gone",
  // because a flat beacon battery would then shut the door on the animal it was
  // meant to let in. The firmware gates on everSeen(); this records that the
  // tracker gives it a truthful answer to gate on, however long it waits.
  for (uint32_t when = 0; when < 600000; when += 60000) t.update(when);
  check(!t.everSeen(), "ten minutes of silence still reports never-heard");
  check(!t.isPresent(), "and never invents presence out of nothing");
}

static void test_a_fix_needs_several_samples() {
  ProximityTracker t;
  configure(t);
  // One strong sample is not a fix: MIN_SAMPLES_FOR_FIX exists so a single
  // reflected packet cannot open a door.
  t.addSample(-40, 0, 1000);
  check(!t.hasFix(1000), "one sample is not a fix");
  t.addSample(-40, 0, 1100);
  t.addSample(-40, 0, 1200);
  check(t.hasFix(1200), "three samples are");
}

// ---------------------------------------------------------------------------
// Invariant 3 — a stale signal can close, never open
// ---------------------------------------------------------------------------
static void test_a_stale_signal_can_close_but_never_open() {
  ProximityTracker t;
  configure(t);

  // Strong and fresh: the door opens.
  uint32_t when = feed(t, -40, 20, 1000);
  check(t.isPresent(), "a strong fresh signal becomes PRESENT");

  // Now the beacon goes silent while still strong. No new samples at all.
  // `far` must become true from the lost fix alone, and the close must happen.
  const uint32_t silentFrom = when;
  for (uint32_t k = 1; k <= 40; ++k) t.update(silentFrom + k * 1000);
  check(!t.isPresent(),
        "a lost signal closes the door even though the last reading was strong");
  check(!t.hasFix(silentFrom + 40000), "and the fix is gone");

  // The mirror image, which is the half that protects an animal: from ABSENT,
  // staleness must NOT be able to produce an open. Drive a long way past the
  // enter dwell with no samples.
  for (uint32_t k = 1; k <= 60; ++k) t.update(silentFrom + 40000 + k * 1000);
  check(!t.isPresent(),
        "no quantity of elapsed time with a stale signal can open the door");
}

// ---------------------------------------------------------------------------
// The millis() wrap — CONTRIBUTING.md invariant 10
// ---------------------------------------------------------------------------
static void test_the_millis_wrap_cannot_resurrect_a_dead_beacon() {
  ProximityTracker t;
  configure(t);

  // A sample stamped just before the 32-bit wrap, read just after it. Unsigned
  // subtraction gives the true small age; the tempting `(now > then)` form would
  // give 0 and call a pre-wrap sample brand new.
  const uint32_t beforeWrap = 0xFFFFFF00u;
  t.addSample(-40, 0, beforeWrap);
  t.addSample(-40, 0, beforeWrap + 50);
  t.addSample(-40, 0, beforeWrap + 100);
  const uint32_t afterWrap = 200;   // wrapped: ~356 ms later in real time
  check(t.sampleAgeMs(afterWrap) < 1000,
        "a sample from just before the wrap reads as a few hundred ms old");
  check(t.hasFix(afterWrap), "so the fix survives the wrap");

  // And the case that matters for safety: a genuinely ancient sample must read
  // as ancient across the wrap, not as fresh.
  ProximityTracker u;
  configure(u);
  const uint32_t longAgo = 0xFFFF0000u;          // well before the wrap
  u.addSample(-40, 0, longAgo);
  u.addSample(-40, 0, longAgo + 10);
  u.addSample(-40, 0, longAgo + 20);
  const uint32_t muchLater = 600000;              // wrapped, ten minutes on
  check(u.sampleAgeMs(muchLater) > 60000,
        "a long-dead beacon still reads as long dead after the wrap");
  check(!u.hasFix(muchLater),
        "so it cannot read as present — this is the bug the signed delta exists "
        "to prevent");
}

static void test_a_sample_stamped_slightly_ahead_reads_as_fresh() {
  ProximityTracker t;
  configure(t);
  // The BLE callback stamps millis() a few ms ahead of the control task's nowMs.
  // A bare subtraction underflows to ~2^32 and the freshest reading there is
  // reads as 49 days old, which drops the fix and cancels a pending open.
  t.addSample(-40, 0, 10000);
  t.addSample(-40, 0, 10001);
  t.addSample(-40, 0, 10005);
  check(t.sampleAgeMs(10000) == 0, "a sample stamped ahead of now clamps to 0");
  check(t.hasFix(10000), "and still counts as a fix");
}

// ---------------------------------------------------------------------------
// Invariant 4 — the fast pair may never be slower than the slow pair
// ---------------------------------------------------------------------------
static void test_the_open_path_can_never_be_made_slower_than_the_close_path() {
  ProximityTracker t;
  configure(t);

  check(t.setFilter(5, 0.3f), "setup: slow pair 5 / 0.30");
  check(t.setFastFilter(3, 0.5f), "a faster fast pair is accepted");

  // Reversing the speeds would mean noticing a departure before an arrival.
  check(!t.setFastFilter(7, 0.5f),
        "a fast window WIDER than the slow one is refused");
  check(!t.setFastFilter(3, 0.1f),
        "a fast alpha SMALLER than the slow one is refused");
  check(t.fastWindowSize() == 3 && t.fastAlpha() == 0.5f,
        "a refused pair leaves the working one in place");

  // Equal is allowed — the rule is "never slower", not "always faster".
  check(t.setFastFilter(5, 0.3f), "a fast pair equal to the slow one is allowed");

  // An even window has no single middle element.
  check(!t.setFastFilter(4, 0.5f), "an even fast window is refused");
  check(!t.setFilter(4, 0.3f), "an even slow window is refused");

  // Nonsense alphas.
  check(!t.setFastFilter(3, 0.0f), "alpha 0 is refused");
  check(!t.setFastFilter(3, 1.5f), "alpha above 1 is refused");
}

static void test_the_fast_filter_rises_first() {
  // The substance behind invariant 4: given the same rising signal, the fast
  // filter must lead. If it ever lagged, the door would notice a departure
  // before an arrival.
  ProximityTracker t;
  configure(t);
  check(t.setFilter(5, 0.2f), "setup: slow 5 / 0.20");
  check(t.setFastFilter(1, 0.9f), "setup: fast 1 / 0.90");

  feed(t, -90, 6, 1000);          // settle both low
  feed(t, -40, 3, 2000);          // then a strong arrival
  check(t.fastRssi() > t.filteredRssi(),
        "on a rising signal the fast filter is ahead of the slow one");
}

// ---------------------------------------------------------------------------
// Invariant 4, second half — a returning animal cancels a pending close
// ---------------------------------------------------------------------------
static void test_a_returning_animal_cancels_a_pending_close() {
  ProximityTracker t;
  configure(t);
  check(t.setFilter(5, 0.2f), "setup: slow 5 / 0.20");
  check(t.setFastFilter(1, 0.9f), "setup: fast 1 / 0.90");

  uint32_t when = feed(t, -40, 20, 1000);
  check(t.isPresent(), "present to begin with");

  // Walk away: weak samples start the exit dwell.
  when = feed(t, -90, 10, when + 100);
  check(t.isPresent(), "still present part-way through the exit dwell");
  check(t.pendingExitMs(when) > 0, "a close is pending");

  // Come straight back. ONE strong advertisement must cancel it, via the fast
  // filter — this is the case the comment in update() is about: waiting for the
  // slow filter to climb back would leave the door travelling shut on an animal
  // walking into it.
  when += 100;
  t.addSample(-35, 0, when);
  t.update(when);
  check(t.pendingExitMs(when) == 0,
        "one strong advertisement cancels the pending close immediately");
  check(t.isPresent(), "and the door never left PRESENT");
}

static void test_the_dwells_are_actually_waited_out() {
  ProximityTracker t;
  configure(t);
  check(t.setFastFilter(1, 1.0f), "setup: fast filter passes samples straight through");

  // Enter dwell: strong signal, but not yet long enough.
  //
  // Note pendingEnterMs()/pendingExitMs() return the dwell time ELAPSED, not the
  // time remaining — so at the exact tick the dwell starts they return 0, which
  // is indistinguishable from "nothing pending". Harmless, because both are only
  // ever displayed, but it is why this advances a sample before asking.
  uint32_t when = feed(t, -40, 4, 1000, 100);   // fix at t=1200, near since then
  check(!t.isPresent(), "a strong signal alone is not presence — the dwell runs");
  check(t.pendingEnterMs(when) > 0, "an open is pending and has been for a while");

  when = feed(t, -40, 20, when + 100, 100);
  check(t.isPresent(), "once the enter dwell elapses, PRESENT");

  // Exit dwell: weak, but the dwell is long, so the door must hold.
  when = feed(t, -95, 20, when + 100, 100);     // ~2 s of weak samples
  check(t.isPresent(),
        "2 s of weak signal does not close a door with a 15 s exit dwell");

  when = feed(t, -95, 150, when + 100, 100);    // ~15 s more
  check(!t.isPresent(), "once the exit dwell elapses, ABSENT");
}

// ---------------------------------------------------------------------------
// The band between the thresholds
// ---------------------------------------------------------------------------
static void test_inside_the_band_nothing_changes() {
  ProximityTracker t;
  configure(t);
  check(t.setFastFilter(1, 1.0f), "setup: fast filter straight through");

  uint32_t when = feed(t, -40, 20, 1000);
  check(t.isPresent(), "present to begin with");

  // -66 is between exit (-72) and enter (-60): neither near nor far. A door
  // that acted here would flap across the band, which is what the band exists
  // to prevent.
  when = feed(t, -66, 200, when + 100);
  check(t.isPresent(), "a signal inside the band keeps the door open");
  check(t.pendingExitMs(when) == 0, "and starts no close");

  // From ABSENT, the same reading must not open it either.
  ProximityTracker u;
  configure(u);
  u.setFastFilter(1, 1.0f);
  uint32_t w = feed(u, -66, 200, 1000);
  check(!u.isPresent(), "and from absent, a signal inside the band opens nothing");
  check(u.pendingEnterMs(w) == 0, "with no open pending");
}

int main() {
  std::printf("proximity.cpp — host unit tests\n");
  test_thresholds_must_keep_a_hysteresis_band();
  test_silence_at_boot_is_not_absence();
  test_a_fix_needs_several_samples();
  test_a_stale_signal_can_close_but_never_open();
  test_the_millis_wrap_cannot_resurrect_a_dead_beacon();
  test_a_sample_stamped_slightly_ahead_reads_as_fresh();
  test_the_open_path_can_never_be_made_slower_than_the_close_path();
  test_the_fast_filter_rises_first();
  test_a_returning_animal_cancels_a_pending_close();
  test_the_dwells_are_actually_waited_out();
  test_inside_the_band_nothing_changes();
  std::printf("%s  %d checks, %d failed\n", g_fails ? "FAILED" : "ok    ",
              g_checks, g_fails);
  return g_fails ? 1 : 0;
}
