# Security policy

## Reporting a vulnerability

Please report security issues **privately** — do not open a public issue.

- GitHub: **Security → Report a vulnerability** (private vulnerability reporting), or
- Email: jacobjc@gmail.com

Include what you found, how to reproduce it, and the impact you expect. Reports
are acknowledged within **3 business days** with a first assessment within
**7 days**.

## What this project is, and what that means for severity

This is **firmware that moves a physical door an animal walks through**, plus a
small log server. So the severity scale is not purely about data:

**Anything that can make the door refuse to open, or close while an animal is in
the doorway, is treated as Critical** — above any confidentiality issue. A bug
that locks a hen out overnight, or drives a motor onto something in its way, is
the worst outcome this project has. [docs/SAFETY.md](docs/SAFETY.md) lists the
failure modes already known and designed around; a new one belongs here.

If you find a way to injure an animal that `docs/SAFETY.md` does not already
cover, report it as a security issue even though nothing is "exploited".

| Severity | Fixed within |
|---|---|
| Critical (or actively exploited) | 7 days |
| High | 30 days |
| Medium | 90 days |
| Low | next planned release |

## Supported versions

Only the latest commit on `main` receives fixes. There are no release branches:
this is a hobbyist project that people copy onto their own hardware, so the
expectation is that you update from source.

## Scope

**In scope:**

- **The BLE advertisement parsers** (`petdoor/beacon.*`). This is the only
  attacker-controlled input the firmware has — every byte arrives from an
  unauthenticated radio broadcast from anyone within range. It is fuzzed under
  ASan/UBSan in CI for that reason. Memory-safety findings here are the most
  valuable reports this project can receive.
- **Anything that moves the door without authorisation**, including replay of a
  recorded command, or a way to make the beacon-matching logic accept a device
  that is not the configured beacon.
- **The network console** (`petdoor/console.*`). It is the *full* console, so it
  can open the door. It only listens inside a maintenance window and never
  without `CONSOLE_PASSWORD`; a way around either is in scope.
- **The log server** (`tools/logserver/`): upload authentication, command
  injection into the command queue, the dashboard, and the public/private route
  split.
- **The over-the-air update path**: a way to flash a door you do not own.

**Known and accepted, so not findings:**

- **Uploads are signed, not encrypted.** Each upload carries an HMAC-SHA256
  signature and the key never crosses the wire, so a log line cannot be forged or
  replayed — but the contents (RSSI values, door states, timestamps) travel in
  cleartext over HTTP if you point it at an HTTP endpoint. TLS is a compile-time
  option that is off by default; see `LOG_ALLOW_TLS` in `petdoor/config.h` for why.
  Confidentiality of a door-open log was judged not worth the heap. Disagree with
  reasoning, not with the fact.
- **Anyone with physical access owns the door.** The relays are screw terminals
  and the board has a reset button. There is no secure boot and no flash
  encryption. Physical access is out of scope.
- **The serial console is unauthenticated.** It requires a cable. See above.
- **A jammed radio stops the door responding to the beacon.** Any 2.4 GHz
  jammer does this to any BLE device. The door fails toward whatever it last
  believed, and `BOOT_GRACE_MS` and the "never actuate before the beacon has been
  heard once" rule are what stop that being dangerous rather than merely
  inconvenient.

## Secrets

`petdoor/secrets.h` is git-ignored because this is a public repo. It holds your
beacon's address, your WiFi credentials, `OTA_PASSWORD`, `CONSOLE_PASSWORD` and
the log-server key. **If you have ever committed it, rotate those values** —
git history is public and permanent.

`secrets.example.h` is the file to copy and the file to add new examples to.

## Fixed issues are disclosed

Once a fix is on `main`, the issue is described in the commit message in enough
detail to understand what was wrong and whether you were affected — this project
documents causes rather than just changes. You will be credited if you wish.
