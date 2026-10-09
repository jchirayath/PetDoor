# Deviations from the engineering standard

Every rule in `~/code/engineering-standard/ENGINEERING_STANDARD.md` that this
repo does not meet, with the reason, an owner, and an expiry. Rules not listed
here are either met or genuinely not applicable in the way §1 describes.

**Owner for everything below: Jacob Chirayath.** This is a one-maintainer
hobbyist project; there is no second owner to escalate to, and saying so is more
useful than writing a name twice.

Reviewed: **2026-10-06**.

Items waiting on an external event rather than on effort — the beacon's TLM slot,
an unreproduced panic — live in [PARKING_LOT.md](PARKING_LOT.md). A deviation here
is a commitment with a date; a parked item cannot be worked on yet. Keeping them
apart is what stops this file filling up with excuses.

---

## Why so much of the standard does not apply

The standard is written for web services and apps. This repo is **ESP32 firmware
plus a ~2,000-line single-file log server**. Rules about nav inventories, design
tokens, icon registries, e2e walkthroughs and store releases have no referent
here — there is no nav, no router, no icon set and no store. Those are marked
**N/A** rather than listed as debt, because recording them as debt would bury the
handful of real gaps in a list nobody reads.

The two places the standard applies with full force are:

1. **`petdoor/beacon.*`** — the only attacker-controlled input in the firmware.
   Every byte comes from an unauthenticated broadcast. This is fuzzed under
   ASan/UBSan in CI (TEST-10) and has unit tests with real payloads.
2. **`tools/logserver/`** — a public website with an authenticated dashboard that
   can open a door. The §2A website gate and the WEB-* rules apply to it
   properly.

---

## Open deviations

| Rule | Deviation | Reason | Expiry |
|---|---|---|---|
| **VULN-3** | Arduino/PlatformIO dependencies are not watched automatically | Dependabot is enabled for `github-actions`, which is the whole surface it can see. The ESP32 core and NimBLE-Arduino are pinned by hand in CI and `platformio.ini`, and Dependabot has no ecosystem for either, so those two are checked by a person when the core is bumped | 2027-06-30 |
| **TEST-0 / TEST-7** | No feature→test map; no e2e or walkthrough tests | There is no nav inventory to map to. Most of the firmware needs a chip, a radio and a relay, so it is verified on the reference door and recorded in commit messages instead. The parts that are pure logic — `beacon.*`, `proximity.*`, the sensor verdicts, the server's alerting — do have unit tests (180 host checks, 53 server checks) | No expiry — see note below |
| **REL-1 / REL-3** | No `docs/SLO.md`, no incident section in a `docs/RUNBOOK.md` | The "service" is one door and one log server with one user. An SLO would be fiction. `docs/TROUBLESHOOTING.md` and `docs/DIAGNOSTICS.md` are the runbook in practice | 2027-03-31, to revisit if anyone else deploys the server |
| **Checklist** | No `docs/THREAT_MODEL.md` | The threat model exists in prose, in the Scope section of [SECURITY.md](../SECURITY.md) and by chapter in [mappings/ASVS-5.0.md](../mappings/ASVS-5.0.md), which is where a contributor will actually look. A separate document would restate both. `docs/OBSERVABILITY.md` now exists and earned its keep — it caught three gaps where the door knew something and no sink a person reads carried it | 2027-03-31 |

### On manual control being able to shut an animal out

**A deliberate deviation from invariant 10, added at the owner's explicit request
on 2026-10-08.**

Invariant 10 says a maintenance window expires by itself, is bounded, and is
never written to NVS, because "a door left inert by a forgotten flag, a lost
network or a brownout is a door that cannot let an animal in". Manual control
breaks all three on purpose: it does not expire, it has no bound, and it
survives a power cut.

What was wanted is sound — pin the door shut while the coop is cleaned, or open
while a broody hen is moved, and have it still be pinned an hour later. Why it is
a deviation is equally sound: a forgotten override is indistinguishable from
outside from a door that has stopped working, and the cost lands on an animal
that cannot report it.

It is paid for with noise rather than a timer, on the argument that a bounded
hold would not do the job asked of it:

| | |
|---|---|
| status line | `ovr=1`, so the dashboard shows it on every refresh |
| dashboard | a pill that deliberately shows **no countdown**, because it has none |
| event log | a `MANUAL` event, so the moment it began is timestamped |
| email | on arrival with **no cooldown**, and **repeated** by the watchdog sweep for as long as it stays set |
| LED | a triple blip, distinct from the double blip that means merely locked |
| boot banner | stated on every restart, since that is when somebody is asking why the door does nothing |

**Owner:** Jacob Chirayath. **Expiry: 2027-04-30** — to be revisited with a
question rather than a timer: has a hold ever been left on by accident? If yes,
the honest fix is a long bound (a day, not four hours) rather than removing the
feature. If no, close this entry as working as intended.

### On TEST-0 having no expiry

Writing an expiry would be pretending. The rule asks for four kinds of test per
feature, and for a door controller the meaningful ones need hardware: you cannot
unit-test "the relay pulse was long enough for this particular vendor controller
to notice". What this project does instead is stated in CLAUDE.md — verification
is host tests *plus* "it compiles for every target", and anything hardware-proven
says so in its commit message along with the numbers measured. Where logic can be
extracted and tested on a host, it has been, and `petdoor/sensor_verdict.h` exists
specifically because that extraction is worth doing.

---

## Accepted permanently, with reasoning

These are not debt. They are decisions, recorded so they are not re-litigated as
oversights.

| Rule | Decision | Reasoning |
|---|---|---|
| **VULN-1** (supported versions) | Only `main` is supported; there are no release branches | People copy this onto their own hardware from source. Maintaining a release branch for a one-maintainer hobbyist project would mean backporting fixes nobody is running |
| Transport confidentiality | Log uploads are **signed, not encrypted**, and TLS is off by default | Each upload carries HMAC-SHA256 and the key never crosses the wire, so forgery and replay are covered. TLS cost ~170 KB of flash and, measured on Bluedroid, drove the heap low-water to 18 KB and then failed to connect. Integrity was the property that mattered; confidentiality of an RSSI log was not worth a door that panics. See `LOG_ALLOW_TLS` in `config.h` — and note that comment now records the headroom argument no longer holding on NimBLE, so this may be revisited on its merits |
| Secrets in a local file | `petdoor/secrets.h` is a git-ignored header, not a secret manager | It is compiled into the binary because it has to be — the device has no network at boot and no secure element. The guardrails are the gitignore, the `Read` deny in `.claude/settings.json`, and `secrets.example.h`. CLAUDE.md states plainly that the deny is a guardrail and not a sandbox |
| Physical access | Out of scope: no secure boot, no flash encryption | The relays are screw terminals and the board has a reset button. Anyone who can reach the hardware can open the door with a paperclip, so firmware hardening would protect nothing |
| Two semgrep SQL findings | Marked not-affected rather than fixed | Both are f-strings building SQL in `tools/logserver/`, and in both the interpolated value is a **literal in the source**, not input — they interpolate an identifier, which SQLite cannot parameterise. Rewriting them to satisfy the scanner would make the code less clear without changing what it does. VULN-4 requires the triage to be recorded, which is what this row is |

---

## How to change this file

If you close one of the open deviations, delete its row — do not mark it done.
This file is a list of what is currently untrue, and a row that says "fixed" is a
row that makes the list longer without making it more informative.

If you add a deviation, it needs all four columns. A deviation with no expiry
needs a reason it cannot have one, like TEST-0 above.
