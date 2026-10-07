# OWASP ASVS 5.0 — PetDoor's position

The baseline is **ASVS 5.0 Level 2**. The full 345-requirement matrix mapping ASVS
to rule IDs lives in the engineering standard itself and is generated; this file is
the **per-repo** half the checklist asks for: which chapters apply here, the
evidence, and what is accepted rather than met.

Reviewed: **2026-10-06**. Verified by reading the code, not from memory — one
suspected finding was withdrawn in the process (see the note at the end).

---

## What is in scope, and what an "application" even means here

Two very different things live in this repo, and ASVS applies to them unequally:

**`tools/logserver/`** — a real website with an authenticated dashboard that can
open a door. **Every web chapter applies.**

**`petdoor/`** — ESP32 firmware. No sessions, no OAuth, no browser, no file uploads.
What it does have is **one unauthenticated input**: the BLE advertisement parsers in
`beacon.*`, which read bytes broadcast by anyone in radio range. That is the whole
attack surface and it gets treated accordingly — fuzzed under ASan/UBSan in CI
(TEST-10) plus unit tests with real payloads.

The third "application" is the **door itself**, which ASVS has no chapter for. This
project treats a door that refuses to open as a higher severity than any
confidentiality issue; see [SECURITY.md](../SECURITY.md).

---

## By chapter

| Chapter | Applies | Position |
|---|---|---|
| **V1 Encoding & Sanitization** | server | ✅ All rendered values pass `html.escape()` or the local `esc()` helper. SQL is parameterised (`?` placeholders) with two exceptions, both identifier interpolation that SQLite cannot parameterise — `PRAGMA table_info({table})` and `ALTER TABLE {table} ADD COLUMN`, both called with literals from `_ensure_column()`. Triaged not-affected, recorded in [STANDARD_EXCEPTIONS.md](../docs/STANDARD_EXCEPTIONS.md) |
| **V2 Validation & Business Logic** | both | ✅ Server: both POST endpoints cap `Content-Length` before reading (2 KB on `/api/command`, 1 MB on `/ingest`), and commands are allow-listed verb-by-verb with per-argument range checks. Firmware: every config setter validates and **refuses** rather than clamping where a silent substitution would surprise — and `static_assert` enforces the relationships that could misbehave quietly |
| **V3 Web Frontend Security** | server | ✅ Enforcing `Content-Security-Policy` with no `'unsafe-inline'` — the inline script and style were moved out to make that possible. Plus `X-Frame-Options: DENY`, `nosniff`, `Referrer-Policy: no-referrer`, HSTS, `Permissions-Policy`, and `-Server` to stop announcing the Python version |
| **V4 API & Web Service** | server | ✅ `/api/events` and `/export.csv` are behind the identity provider. `/api/command` additionally requires a CSRF check. `/ingest` is machine-authenticated — see V11 |
| **V5 File Handling** | server | ✅ No uploads. The only file-serving route is `/images/`, and `basename()` strips traversal outright. The CSS/JS routes are an exact-match allow-list of four paths, not a prefix |
| **V6 Authentication** | server | ✅ Humans: Entra ID via oauth2-proxy — no password handling in this codebase at all, which is the right amount. Machines: HMAC. The firmware's network console requires `CONSOLE_PASSWORD` and **only listens inside a maintenance window** |
| **V7 Session Management** | — | n/a — delegated entirely to oauth2-proxy. This application issues no sessions |
| **V8 Authorization** | server | ✅ One role. Public routes are an explicit allow-list and everything unnamed falls through to the gate, so a route added later is private by default |
| **V9 Self-contained Tokens** | — | n/a — no JWTs issued or validated here |
| **V10 OAuth & OIDC** | — | n/a — delegated to oauth2-proxy |
| **V11 Cryptography** | both | ✅ Uploads are signed HMAC-SHA256 over body + timestamp, compared with `hmac.compare_digest()` (constant-time), with a 15-minute skew window (`CLOCK_SKEW_S`) so a captured batch cannot be replayed indefinitely. The key never crosses the wire |
| **V12 Secure Communication** | server | 🟡 **Accepted deviation.** The dashboard is HTTPS-only with HSTS. `/ingest` also answers over plain HTTP, deliberately: an ESP32 cannot afford a TLS handshake — measured, it drove free heap to 18 KB and then failed to connect. Integrity and replay are covered by V11; what is given up is confidentiality of the fact that a door opened. Reasoned at `LOG_ALLOW_TLS` in `config.h`, which now also records that the heap argument no longer holds on NimBLE, so this is revisitable on its merits |
| **V13 Configuration** | both | ✅ `secrets.h` git-ignored with a `Read` deny and an `.example` counterpart; CI creates its own. Debug endpoints absent. Web control is **off by default**. `-Server` set |
| **V14 Data Protection** | server | ✅ The sensitive asset is not a credential, it is the **event log**: trips per day and first-out/last-out describe a household's routine precisely enough to say when a house is reliably empty. That is why the gate exists and why `/demo` carries synthetic data baked into the page so a dashboard can be shown to anyone |
| **V15 Secure Coding & Architecture** | both | ✅ The firmware's one unauthenticated parser is fuzzed. 180 host checks plus 53 server checks. A lint rule fails the build if the NUL-truncating idiom returns to the BLE adapter — the one P0 this project has had |
| **V16 Logging & Error Handling** | both | ✅ Durable event ring in NVS, uploaded and stored server-side; rejected uploads are logged with the reason and the source address. No secrets are logged. [OBSERVABILITY.md](../docs/OBSERVABILITY.md) is the producer × sink table |
| **V17 WebRTC** | — | n/a |

---

## Accepted, not met

| | Why |
|---|---|
| **No TLS on `/ingest`** | V12 above. A hardware budget, not a preference |
| **No rate limiting** | The door is one client posting every five minutes. `/ingest` requires a valid HMAC and caps the body at 1 MB; `/api/command` caps at 2 KB and requires both the identity provider and a CSRF check. Adding a limiter would protect against an attacker who already has the signing key, which is not the threat it would be defending |
| **No secure boot or flash encryption** | Anyone who can reach the hardware can open the door with a paperclip across the relay terminals. Firmware hardening would protect nothing. Physical access is explicitly out of scope |

---

## A withdrawn finding, recorded on purpose

While writing this I believed I had found an uncapped `self.rfile.read(length)` —
a client declaring a huge `Content-Length` and the server reading it into memory.
**It was wrong.** Both POST handlers check the length *before* reading; a narrow
grep had hidden the adjacent guard.

It is recorded because the near-miss is the lesson: I was one step from "fixing"
working code and from writing a vulnerability into a security document, where it
would have been believed. A mapping is only worth having if each row was checked
rather than recalled — so each row above names the mechanism, which is what makes
it falsifiable by the next reader.
