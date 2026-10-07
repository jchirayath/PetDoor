#include "actuator.h"

#include "ble_scanner.h"
#include "position.h"
#include "vibration.h"

namespace Actuator {
namespace {

DoorController *g_door = nullptr;

Phase g_phase = PH_IDLE;
DoorState g_target = DOOR_UNKNOWN;
DoorState g_origin = DOOR_UNKNOWN;
ActuationSource g_source = SRC_BEACON;

// Whether a switch agreed the door was at g_origin when the attempt began.
// Without that agreement, "the origin switch has released" is not evidence of
// anything — the door was never there to leave.
bool g_originMade = false;

// Whether the switch at the DESTINATION was already made when the attempt
// began. It can be: a forced press on a door whose believed position is stale,
// or simply someone pressing `o` on a door that is already open. The press is
// still sent — they asked — but the instant "arrival" that follows is not a
// measurement of a travel, and recording it as one would put a nonsense figure
// into the number travel-time tuning is read off.
bool g_targetMadeAtStart = false;

// The free-running edge counter when the attempt began, so the resolution can
// report how many arrived during it — blanking and all.
uint32_t g_vibRawAtStart = 0;

uint16_t g_flags = 0;
uint8_t g_presses = 0;
uint8_t g_swallowRetries = 0;

uint32_t g_deadlineMs = 0;      // when the current phase gives up waiting
uint32_t g_travelStartMs = 0;   // the press the travel is timed from
uint32_t g_lastPressAtMs = 0;   // when the most recent press finished
bool g_arrivalKnowable = false; // is a switch fitted at the destination?

uint32_t g_travelOpenMs = DOOR_TRAVEL_OPEN_MS;
uint32_t g_travelCloseMs = DOOR_TRAVEL_CLOSE_MS;
uint32_t g_measuredOpenMs = 0;
uint32_t g_measuredCloseMs = 0;

uint32_t g_wakeIdleMs = WAKE_IDLE_MS;
uint32_t g_retryDelayMs = CLOSE_RETRY_DELAY_MS;
uint8_t g_retryLimit = CLOSE_RETRY_LIMIT;

uint8_t g_closeAttempts = 0;
uint32_t g_retryAtMs = 0;
bool g_retryPending = false;
bool g_gaveUp = false;

// "An attempt in this direction just achieved nothing; leave it alone for a
// while." Without it the door loops: a failed attempt commits nothing, so the
// condition that asked for it is still true, so the control task asks again on
// its next tick and presses the relay ten times a second.
uint32_t g_coolUntilMs = 0;
DoorState g_coolTarget = DOOR_UNKNOWN;
bool g_coolPending = false;

// A stalled close has to be reversed, but not from inside resolve(): the
// caller has not yet been handed the stall to announce and log, and starting
// the reversal first would report the two in the wrong order. So it is armed
// here and acted on by the next tick.
bool g_failsafePending = false;

Result g_result = {};
bool g_resultPending = false;

// Notes queue. Four is plenty: an attempt can generate at most one wake note
// and SWALLOW_RETRY_LIMIT retry notes.
constexpr uint8_t kNoteCap = 6;
Note g_notes[kNoteCap];
uint8_t g_noteCount = 0;

void note(LogEventType type, uint8_t detail, bool record = true) {
  if (g_noteCount >= kNoteCap) return;  // drop rather than grow: these are advisory
  g_notes[g_noteCount++] = {type, detail, record};
}

// Wrap-correct "has this deadline passed". Signed, for the same reason every
// other deadline in this firmware is: a door left running through a season
// reaches the millis() rollover at 49 days.
inline bool reached(uint32_t deadlineMs, uint32_t nowMs) {
  return static_cast<int32_t>(nowMs - deadlineMs) >= 0;
}

DoorState opposite(DoorState s) { return s == DOOR_OPEN ? DOOR_CLOSED : DOOR_OPEN; }

// ---------------------------------------------------------------------------
// Evidence. Two sensors answering two different questions, and the rules about
// what each may be used for are the rules in docs/REQUIREMENTS.md.
// ---------------------------------------------------------------------------

// Can anything observe whether the door started moving? With neither a
// vibration sensor nor a switch at the end it was leaving, no — and then the
// start is skipped rather than guessed at.
bool startKnowable() {
  return Vibration::enabled() || g_originMade;
}

// Did the door demonstrably start moving?
//
// Vibration answers this and ONLY this: never direction, never position. Two
// confident position reconstructions were built on it during bench work and
// both were wrong, which is why it is confined to one boolean here.
//
// `strong` raises the vibration bar from "something happened" to "this is
// travel". The wake probe asks for it and the start check does not, because a
// false answer costs opposite things in the two cases — see
// VIBRATION_MOVING_PULSES in config.h.
//
// The limit-switch half needs no such distinction. A switch that was made and
// has let go is unambiguous: the door has physically left that end.
bool movedEvidence(bool strong) {
  if (Vibration::enabled()) {
    const uint32_t bar = strong ? VIBRATION_MOVING_PULSES : VIBRATION_MIN_PULSES;
    if (Vibration::pulsesThisTravel() >= bar) return true;
  }
  // The switch it was sitting on has let go. Only meaningful if it was made to
  // begin with, which g_originMade records at press time.
  if (g_originMade && !Position::madeAt(g_origin)) return true;
  return false;
}

// ---------------------------------------------------------------------------
// Calibration state. Kept with the actuator because it is entirely about
// travel timing, and because it has to see each attempt resolve.
// ---------------------------------------------------------------------------
CalState g_cal = CAL_OFF;
uint32_t g_calUntilMs = 0;
char g_calMsg[96] = "";
bool g_calOpenSeen = false;
bool g_calClosedSeen = false;
uint32_t g_calOpenMs = 0;
uint32_t g_calCloseMs = 0;

void calSay(const char *text) {
  strncpy(g_calMsg, text, sizeof(g_calMsg) - 1);
  g_calMsg[sizeof(g_calMsg) - 1] = '\0';
}

void calFail(const char *why) {
  g_cal = CAL_FAILED;
  calSay(why);
}

void resolve(Outcome outcome);

// ---------------------------------------------------------------------------
// The attempt itself.
// ---------------------------------------------------------------------------

// One press of the button, and the bookkeeping that has to happen around it.
void sendPress() {
  g_door->press(g_target);
  g_presses++;
  g_lastPressAtMs = millis();
  // Baseline the vibration counter AFTER the press rather than before it. The
  // relay's own click is the loudest thing the sensor will hear all day, and
  // it happens at both ends of the press — so blanking from before it closes
  // the window while the contacts are still ringing. See vibration.h.
  Vibration::travelStarted(millis());
}

void enterAwaitArrival() {
  g_phase = PH_AWAIT_ARRIVAL;
  // Timed from the PRESS, not from the moment motion was noticed. Motion is
  // noticed up to MOTION_ONSET_MS later, and a deadline measured from there
  // would quietly grant every travel a couple of extra seconds — which is the
  // sort of slack that makes a travel-time measurement useless for spotting a
  // door that is getting slower. It also makes the measured figure comparable
  // to the stopwatch numbers in docs/REQUIREMENTS.md, which were taken from
  // the press.
  g_travelStartMs = g_lastPressAtMs;
  g_arrivalKnowable = Position::fittedAt(g_target);
  const uint32_t travel = travelMs(g_target);

  if (g_arrivalKnowable) {
    // A switch is going to tell us. The travel time only bounds the wait.
    g_deadlineMs = g_travelStartMs +
                   (travel != 0 ? travel + TRAVEL_GRACE_MS : ARRIVAL_WAIT_MAX_MS);
    return;
  }
  if (travel == 0) {
    // Nothing can confirm arrival and nothing was measured, so there is
    // nothing to wait for and nothing honest to announce.
    resolve(OUT_UNTIMED);
    return;
  }
  // The old open-loop behaviour, preserved exactly: the deadline IS the
  // announcement, with no grace added, so the "arrived" chime lands when it
  // always did on a door with no switches.
  g_deadlineMs = g_travelStartMs + travel;
}

void enterAwaitStart() {
  if (!startKnowable()) {
    // Nothing can see a start. Do not invent a wait for it.
    enterAwaitArrival();
    return;
  }
  g_phase = PH_AWAIT_START;
  g_deadlineMs = millis() + MOTION_ONSET_MS;
}

void beginAttempt(DoorState target, ActuationSource src, uint32_t nowMs) {
  g_target = target;
  g_source = src;
  g_origin = g_door->state();
  g_originMade = Position::madeAt(g_origin);
  g_targetMadeAtStart = Position::madeAt(target);
  g_vibRawAtStart = Vibration::pulses();
  g_flags = (src == SRC_FAILSAFE) ? LOGF_FAILSAFE : 0;
  g_presses = 0;
  g_swallowRetries = 0;

  if (wakeNeeded(nowMs)) {
    // The controller is believed asleep, so this press is expected to be
    // swallowed. It is sent in the TARGET direction deliberately: if the
    // controller turns out to have been awake after all, the press that wakes
    // it moves the door the way we wanted it to go anyway, and the probe below
    // will notice and not press again.
    g_flags |= LOGF_WOKE;
    sendPress();
    g_phase = PH_WAKE_WAIT;
    g_deadlineMs = millis() + WAKE_PROBE_MS;
    return;
  }
  sendPress();
  enterAwaitStart();
}

void resolve(Outcome outcome) {
  const uint32_t now = millis();
  Result r = {};
  r.outcome = outcome;
  r.target = g_target;
  r.source = g_source;
  r.presses = g_presses;
  r.vibrationRaw = Vibration::pulses() - g_vibRawAtStart;
  r.measuredMs = 0;
  r.attempt = 0;
  r.gaveUp = false;

  switch (outcome) {
    case OUT_ARRIVED:
      if (!g_targetMadeAtStart) {
        r.measuredMs = now - g_travelStartMs;
        if (g_target == DOOR_OPEN) {
          g_measuredOpenMs = r.measuredMs;
        } else {
          g_measuredCloseMs = r.measuredMs;
        }
      }
      g_flags |= LOGF_VERIFIED;
      g_door->commit(g_target, g_source, now);
      g_coolPending = false;
      // A travel that completed clears the whole failed-close history. Not
      // just the counter: if the door can close again, the reason it was being
      // held open is gone.
      if (g_target == DOOR_CLOSED) {
        g_closeAttempts = 0;
        g_gaveUp = false;
        g_retryPending = false;
      }
      break;

    case OUT_ASSUMED:
    case OUT_UNTIMED:
      // No switch could confirm it. The belief still moves — this is what the
      // firmware has always done, and with no sensors it is the only thing it
      // can do — but the entry in the log will not carry LOGF_VERIFIED, and
      // that absence is the honest part.
      g_door->commit(g_target, g_source, now);
      g_coolPending = false;
      break;

    case OUT_NO_MOVE:
      // The door demonstrably never moved, so the belief must NOT move either.
      // This is the whole value of being able to tell: the old firmware
      // recorded a close here, and then refused the next close because it
      // thought the door was already shut.
      //
      // Which is also why this direction has to be put on ice: the belief is
      // unchanged, so whatever asked for this travel will ask again on the
      // very next tick.
      g_coolTarget = g_target;
      g_coolUntilMs = now + FAILED_ATTEMPT_COOLDOWN_MS;
      g_coolPending = true;
      break;

    case OUT_STALLED:
      // Somewhere between the two ends. Believing either would be a lie, and
      // the lie that matters is "closed" — so the door admits it does not know
      // and the next request actuates instead of being told it is already
      // there.
      g_door->setBeliefUnknown();
      // Same reasoning as OUT_NO_MOVE, and it matters more here: the belief is
      // now UNKNOWN, which makes "already there" stop refusing anything, so
      // without a cooldown a stalled OPEN would be re-attempted every tick.
      // A stalled close has the much longer retry delay below as well.
      g_coolTarget = g_target;
      g_coolUntilMs = now + FAILED_ATTEMPT_COOLDOWN_MS;
      g_coolPending = true;
      if (g_target == DOOR_CLOSED) {
        g_closeAttempts++;
        r.attempt = g_closeAttempts;
        // Fail OPEN. A door stopped partway while closing is exactly when
        // something may be under it, so the firmware reverses its own command
        // rather than pressing again. Armed here, sent by the next tick, so
        // the stall is announced before the reversal that follows it.
        g_failsafePending = true;
        if (g_closeAttempts >= g_retryLimit) {
          g_gaveUp = true;
          g_retryPending = false;
          r.gaveUp = true;
        } else {
          // Minutes, not seconds. Whatever stopped the door needs time to
          // move or to be noticed, and a door that retries every few seconds
          // is a door driving repeatedly onto an obstruction.
          g_retryAtMs = now + g_retryDelayMs;
          g_retryPending = true;
        }
      }
      break;

    default:
      break;
  }

  r.flags = g_flags;
  g_result = r;
  g_resultPending = true;
  g_phase = PH_IDLE;

  // Calibration watches every attempt resolve, because its whole output is
  // the duration of two of them.
  if (g_cal == CAL_RUN_A || g_cal == CAL_RUN_B) {
    if (outcome != OUT_ARRIVED) {
      calFail(outcome == OUT_NO_MOVE
                  ? "the door never moved — check the pulse width and the wiring"
                  : "the travel did not complete — calibration abandoned");
    } else if (r.measuredMs == 0) {
      // Arrived, but nothing was timed: the door was already at that end when
      // the press went out. Nothing to adopt.
      calFail("the door was already at that end; nothing was timed");
    } else {
      if (r.target == DOOR_OPEN) {
        g_calOpenMs = r.measuredMs;
      } else {
        g_calCloseMs = r.measuredMs;
      }
      if (g_cal == CAL_RUN_A) {
        g_cal = CAL_RUN_B;
      } else {
        g_cal = CAL_DONE;
        if (g_calOpenMs != 0 && g_calCloseMs != 0) {
          setTravelMs(g_calOpenMs, g_calCloseMs);
          BleScanner::storeTravelPair(g_travelOpenMs, g_travelCloseMs);
          char buf[96];
          snprintf(buf, sizeof(buf), "measured open %lu ms, close %lu ms — adopted and saved",
                   static_cast<unsigned long>(g_calOpenMs),
                   static_cast<unsigned long>(g_calCloseMs));
          calSay(buf);
        } else {
          calFail("only one direction was measured; nothing adopted");
        }
      }
    }
  }
}

// Drives the calibration sequence between attempts.
void calTick(uint32_t nowMs) {
  switch (g_cal) {
    case CAL_QUIET: {
      // F2: the vendor controller moves the door by itself. A switch changing
      // during the quiet period means something other than this firmware is
      // still driving the door, and every number measured after that would be
      // measuring that instead.
      if (Position::madeAt(DOOR_OPEN) != g_calOpenSeen ||
          Position::madeAt(DOOR_CLOSED) != g_calClosedSeen) {
        calFail("the door moved during the quiet period — a vendor mode is still enabled");
        return;
      }
      if (!reached(g_calUntilMs, nowMs)) return;
      // Quiet confirmed. Start from whichever end the door is actually at.
      const DoorState at = Position::state();
      if (at == DOOR_UNKNOWN) {
        calFail("the door is not at either end; move it to one and try again");
        return;
      }
      g_cal = CAL_RUN_A;
      calSay("quiet confirmed; timing the first travel");
      if (request(opposite(at), SRC_MANUAL, nowMs, /*force=*/true) != ACT_DONE) {
        // The boot grace is the one gate `force` does not open, and it is the
        // likely one here: somebody rebooted the door and ran `calibrate`.
        calFail("the door refused to move — try again in a minute");
      }
      return;
    }
    case CAL_RUN_B:
      // The first travel has resolved; send the door back. Only once, and only
      // when nothing is in flight.
      if (busy()) return;
      if (g_calOpenMs != 0 && g_calCloseMs != 0) return;  // waiting on resolve()
      if (g_resultPending) return;                        // let the caller read it first
      calSay("timing the return travel");
      if (request(opposite(g_door->state()), SRC_MANUAL, nowMs, /*force=*/true) != ACT_DONE) {
        calFail("the door refused the return travel — nothing adopted");
      }
      return;
    default:
      return;
  }
}

}  // namespace

void begin(DoorController *door) {
  g_door = door;

  uint32_t openMs = DOOR_TRAVEL_OPEN_MS;
  uint32_t closeMs = DOOR_TRAVEL_CLOSE_MS;
  // The legacy single-value key first, so a door upgrading from the old
  // firmware keeps the travel time somebody measured with a stopwatch, then
  // the per-direction pair on top of it.
  uint32_t legacy = 0;
  if (BleScanner::loadStoredTravelMs(legacy)) {
    openMs = closeMs = legacy;
  }
  BleScanner::loadStoredTravelPair(openMs, closeMs);
  setTravelMs(openMs, closeMs);

  uint32_t wakeMs = WAKE_IDLE_MS;
  uint32_t retryMs = CLOSE_RETRY_DELAY_MS;
  uint8_t limit = CLOSE_RETRY_LIMIT;
  BleScanner::loadStoredActuation(wakeMs, retryMs, limit);
  setWakeIdleMs(wakeMs);
  setRetryPolicy(retryMs, limit);

  g_phase = PH_IDLE;
}

ActuationResult request(DoorState target, ActuationSource src, uint32_t nowMs,
                        bool force) {
  if (g_door == nullptr) return ACT_ALREADY;
  if (target != DOOR_OPEN && target != DOOR_CLOSED) return ACT_ALREADY;

  if (busy()) {
    // F3: a second press in the same direction mid-travel reads as STOP and
    // parks the door halfway. Refused, not queued — by the time a travel ends
    // the request that was queued may be the wrong one.
    if (target == g_target) return ACT_BUSY;
    // The opposite direction supersedes. This is the path that matters most in
    // the whole module: it is "open it, something is under it" arriving while
    // a close is in flight, and it must never be the request that is refused.
    //
    // Which is exactly why the belief has to be dropped here. A travel in
    // flight has not committed yet, so the door still believes it is at the
    // end it set off FROM — and check() would answer "already there" and
    // refuse the reversal. The door is mid-travel; it is at neither end, and
    // saying so is both true and what lets the reversal through.
    //
    // It has to happen before check() rather than beside beginAttempt(),
    // because check() is what would answer "already there". So a reversal that
    // a later gate then refuses leaves the belief at UNKNOWN — which is safe
    // in every case: the travel still in flight restores a definite belief
    // when it resolves, and UNKNOWN meanwhile only ever means "the next
    // request actuates instead of being told it is already there".
    g_door->setBeliefUnknown();
  }

  if (g_gaveUp) {
    if (!force && target == DOOR_CLOSED) return ACT_GAVE_UP;
    if (force) {
      // A person is asking. They can see what the firmware cannot, so they get
      // the full attempt budget back.
      g_gaveUp = false;
      g_closeAttempts = 0;
      g_retryPending = false;
      g_coolPending = false;
    }
  }

  if (g_retryPending && target == DOOR_CLOSED && !force) {
    if (!reached(g_retryAtMs, nowMs)) return ACT_RETRY_WAIT;
    g_retryPending = false;
  }

  if (g_coolPending && target == g_coolTarget && !force) {
    if (!reached(g_coolUntilMs, nowMs)) return ACT_RETRY_WAIT;
    g_coolPending = false;
  }

  const ActuationResult gate = g_door->check(target, nowMs, force);
  if (gate != ACT_DONE) return gate;

  beginAttempt(target, src, nowMs);
  return ACT_DONE;
}

void tick(uint32_t nowMs) {
  if (g_door == nullptr) return;

  switch (g_phase) {
    case PH_WAKE_WAIT:
      if (movedEvidence(/*strong=*/true)) {
        // The wake press was not swallowed: it actuated. Pressing again now
        // would be the mid-travel press that stops the door — so do not.
        note(LOG_WAKE, 1);
        enterAwaitArrival();
        break;
      }
      if (reached(g_deadlineMs, nowMs)) {
        // Deliberately NOT a log entry. A wake press is sent before nearly
        // every real actuation — the door is idle for hours between uses —
        // so recording each one would double the log's volume with its least
        // interesting event. That it happened is already carried by
        // LOGF_WOKE on the actuation's own entry; only the surprising case
        // above, where the wake press turned out to BE the actuation, is
        // worth an entry of its own.
        note(LOG_WAKE, 0, /*record=*/false);
        sendPress();
        enterAwaitStart();
      }
      break;

    case PH_AWAIT_START:
      if (movedEvidence(/*strong=*/false)) {
        enterAwaitArrival();
        break;
      }
      if (reached(g_deadlineMs, nowMs)) {
        if (g_swallowRetries < SWALLOW_RETRY_LIMIT) {
          // A press that moved nothing may be retried immediately: the door is
          // still where it was, nothing is trapped, and this failure is
          // distinguishable from a stall. It is the one retry in this module
          // that does not have to wait.
          g_swallowRetries++;
          g_flags |= LOGF_RETRIED;
          note(LOG_RETRY, g_swallowRetries);
          sendPress();
          enterAwaitStart();
        } else {
          resolve(OUT_NO_MOVE);
        }
      }
      break;

    case PH_AWAIT_ARRIVAL:
      if (g_arrivalKnowable && Position::madeAt(g_target)) {
        resolve(OUT_ARRIVED);
        break;
      }
      if (reached(g_deadlineMs, nowMs)) {
        resolve(g_arrivalKnowable ? OUT_STALLED : OUT_ASSUMED);
      }
      break;

    case PH_IDLE:
      // Deferred by one tick while a result is unread. beginAttempt() can
      // resolve synchronously — a door with no switch at the far end and no
      // travel time resolves the moment it is pressed — and that would
      // overwrite the stall this reversal is reacting to before anything had
      // announced it.
      if (g_failsafePending && !g_resultPending) {
        g_failsafePending = false;
        // Forced, because the belief is DOOR_UNKNOWN and the lockout has just
        // been started by the close that failed — neither of which should
        // stand in the way of getting a door off whatever it stopped on.
        request(DOOR_OPEN, SRC_FAILSAFE, nowMs, /*force=*/true);
      }
      break;
  }

  calTick(nowMs);
}

bool consumeResult(Result &out) {
  if (!g_resultPending) return false;
  out = g_result;
  g_resultPending = false;
  return true;
}

bool popNote(Note &out) {
  if (g_noteCount == 0) return false;
  out = g_notes[0];
  for (uint8_t i = 1; i < g_noteCount; i++) g_notes[i - 1] = g_notes[i];
  g_noteCount--;
  return true;
}

bool busy() { return g_phase != PH_IDLE; }
Phase phase() { return g_phase; }
DoorState target() { return g_target; }

uint32_t elapsedMs(uint32_t nowMs) {
  if (!busy()) return 0;
  const int32_t d = static_cast<int32_t>(nowMs - g_travelStartMs);
  return d > 0 ? static_cast<uint32_t>(d) : 0;
}

uint32_t deadlineRemainingMs(uint32_t nowMs) {
  if (!busy()) return 0;
  const int32_t d = static_cast<int32_t>(g_deadlineMs - nowMs);
  return d > 0 ? static_cast<uint32_t>(d) : 0;
}

const char *phaseName(Phase p) {
  switch (p) {
    case PH_WAKE_WAIT: return "waking";
    case PH_AWAIT_START: return "waiting to move";
    case PH_AWAIT_ARRIVAL: return "travelling";
    default: return "idle";
  }
}

const char *outcomeName(Outcome o) {
  switch (o) {
    case OUT_ARRIVED: return "arrived";
    case OUT_ASSUMED: return "assumed";
    case OUT_UNTIMED: return "untimed";
    case OUT_NO_MOVE: return "never moved";
    case OUT_STALLED: return "stalled";
    default: return "-";
  }
}

uint32_t travelMs(DoorState dir) {
  return dir == DOOR_OPEN ? g_travelOpenMs : g_travelCloseMs;
}

bool setTravelMs(uint32_t openMs, uint32_t closeMs) {
  // 0 is legal and means "not measured". Anything else has to be long enough
  // to be a door rather than a typo: a travel time under a second would turn
  // every real travel into a stall, and with a close that means reversing a
  // door that was closing perfectly well.
  for (uint32_t v : {openMs, closeMs}) {
    if (v != 0 && (v < 1000 || v > 600000)) return false;
  }
  g_travelOpenMs = openMs;
  g_travelCloseMs = closeMs;
  return true;
}

uint32_t measuredMs(DoorState dir) {
  return dir == DOOR_OPEN ? g_measuredOpenMs : g_measuredCloseMs;
}

uint32_t wakeIdleMs() { return g_wakeIdleMs; }

bool setWakeIdleMs(uint32_t ms) {
  // 0 disables the wake press. Above that there is a floor: a threshold of a
  // second or two would send a wake press before nearly every actuation,
  // including ones seconds apart where the controller is definitely awake —
  // which is the case where a second press stops a moving door.
  if (ms != 0 && (ms < 5000 || ms > 3600000)) return false;
  g_wakeIdleMs = ms;
  return true;
}

bool wakeNeeded(uint32_t nowMs) {
  if (g_wakeIdleMs == 0) return false;
  if (g_door == nullptr) return false;
  // Nothing has pressed the button since boot, so the controller has been idle
  // for at least as long as this firmware has been running, and probably much
  // longer. Treat it as asleep.
  if (!g_door->hasPressed()) return true;
  const uint32_t since = nowMs - g_door->lastPressMs();
  return since >= g_wakeIdleMs;
}

uint32_t retryDelayMs() { return g_retryDelayMs; }
uint8_t retryLimit() { return g_retryLimit; }

bool setRetryPolicy(uint32_t delayMs, uint8_t limit) {
  // The floor is a minute because the requirement is "minutes, not seconds":
  // the delay exists to give whatever stopped the door time to move or be
  // noticed, and a few seconds gives it neither.
  if (delayMs < 60000 || delayMs > 86400000UL) return false;
  if (limit < 1 || limit > 10) return false;
  g_retryDelayMs = delayMs;
  g_retryLimit = limit;
  return true;
}

uint8_t closeAttempts() { return g_closeAttempts; }

uint32_t retryWaitRemainingMs(uint32_t nowMs) {
  // Whichever hold is in force, reported as one number: from outside, "the
  // door is not going to try again for N seconds" is the same fact either way.
  uint32_t left = 0;
  if (g_retryPending) {
    const int32_t d = static_cast<int32_t>(g_retryAtMs - nowMs);
    if (d > 0) left = static_cast<uint32_t>(d);
  }
  if (g_coolPending) {
    const int32_t d = static_cast<int32_t>(g_coolUntilMs - nowMs);
    if (d > 0 && static_cast<uint32_t>(d) > left) left = static_cast<uint32_t>(d);
  }
  return left;
}

bool gaveUp() { return g_gaveUp; }

void clearGaveUp() {
  g_gaveUp = false;
  g_closeAttempts = 0;
  g_retryPending = false;
  g_coolPending = false;
}

bool startCalibration(uint32_t nowMs, bool maintenanceActive, String &msg) {
  if (!Position::fittedAt(DOOR_OPEN) || !Position::fittedAt(DOOR_CLOSED)) {
    msg = "calibration needs a limit switch at BOTH ends — `sensors <open> <closed>`";
    return false;
  }
  if (Position::fault()) {
    msg = "both limit switches are made at once; fix the wiring first";
    return false;
  }
  if (!maintenanceActive) {
    msg = "calibration needs a maintenance window: the beacon must not be able "
          "to move the door while it is being timed. Try `maint 15` first";
    return false;
  }
  if (busy()) {
    msg = "the door is already travelling";
    return false;
  }
  if (Position::state() == DOOR_UNKNOWN) {
    msg = "the door is between the two ends; send it to one and try again";
    return false;
  }

  g_cal = CAL_QUIET;
  g_calUntilMs = nowMs + CAL_QUIET_MS;
  g_calOpenSeen = Position::madeAt(DOOR_OPEN);
  g_calClosedSeen = Position::madeAt(DOOR_CLOSED);
  g_calOpenMs = g_calCloseMs = 0;
  calSay("quiet period: nothing may command or move the door");

  char buf[160];
  snprintf(buf, sizeof(buf),
           "calibrating: %lu s of quiet first, then one timed travel each way. "
           "Do not touch the door. It MUST stay still — if it moves on its own "
           "the vendor controller still has a mode enabled and the run aborts",
           static_cast<unsigned long>(CAL_QUIET_MS / 1000UL));
  msg = buf;
  return true;
}

void abortCalibration(const char *why) {
  if (g_cal == CAL_OFF) return;
  calFail(why != nullptr ? why : "abandoned");
}

CalState calState() { return g_cal; }
bool calibrating() { return g_cal == CAL_QUIET || g_cal == CAL_RUN_A || g_cal == CAL_RUN_B; }
const char *calMessage() { return g_calMsg; }

uint32_t calRemainingMs(uint32_t nowMs) {
  if (g_cal != CAL_QUIET) return 0;
  const int32_t d = static_cast<int32_t>(g_calUntilMs - nowMs);
  return d > 0 ? static_cast<uint32_t>(d) : 0;
}

ChimeTune moveTune(ActuationSource src, DoorState target) {
  const bool opening = (target == DOOR_OPEN);
  switch (src) {
    case SRC_MANUAL:
      return opening ? CHIME_MOVE_CONSOLE_OPEN : CHIME_MOVE_CONSOLE_CLOSE;
    case SRC_REMOTE:
      return opening ? CHIME_MOVE_REMOTE_OPEN : CHIME_MOVE_REMOTE_CLOSE;
    case SRC_FAILSAFE:
      // No movement tune. The reversal is the second half of a stall, and the
      // stall's own alarm is still sounding — announcing it as an ordinary
      // open would say the opposite of what happened.
      return CHIME_NONE;
    default:
      return opening ? CHIME_MOVE_BEACON_OPEN : CHIME_MOVE_BEACON_CLOSE;
  }
}

}  // namespace Actuator
