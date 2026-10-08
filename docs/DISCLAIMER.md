# Disclaimer, warranty and support

Short version: **this is one person's hobby project that drives a motor on a
door an animal walks through. There is no warranty and no support. You are the
engineer responsible for your installation.**

If that is not a trade you want to make, do not fit it. Nothing here is
criticism of you — a door that can trap or exclude an animal deserves a clear
head about who is accountable for it, and the answer is the person who mounts
it.

This page is about responsibility and expectations. The engineering safety
analysis — what the firmware protects against, what it does not, and the
commissioning procedure — is [SAFETY.md](SAFETY.md), and you should read it
before the door is live. Vulnerability reporting is [SECURITY.md](../SECURITY.md).

---

## No warranty

The software is MIT licensed, and the licence says this in legal terms:

> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
> IMPLIED.

In plain terms: it is not certified, not independently reviewed, not tested
against any standard, and not guaranteed to work on your door. It has been
tested on **one** reference installation by one person. Your door mechanism,
controller, wiring, radio environment and animal are all different.

The same applies to the hardware suggestions. Parts lists name specific modules
because a stranger needs somewhere to start, not because they have been
qualified for the job. Links to retailers are not endorsements and earn nothing.

## No support

There is no support, no service-level agreement, and no commitment to respond.

- **Issues and discussions** are read when there is time, and may never be
  answered. A question going unanswered is the expected outcome, not a slight.
- **Pull requests** are welcome and may sit indefinitely. See
  [CONTRIBUTING.md](../CONTRIBUTING.md).
- **No obligation to maintain.** The project may stop being updated at any time,
  without notice or announcement. Fork it; that is what the licence is for.
- **No remote assistance.** Nobody can log into your door or your log server,
  and nobody should ever ask you for your WiFi credentials, `OTA_PASSWORD`,
  `CONSOLE_PASSWORD` or log-server key. Treat any such request as hostile.

The one exception, and it is a courtesy rather than a promise: a credible report
that a door **traps, injures or excludes an animal** is the thing most worth
hearing about, and takes priority over every other kind of issue. See
[SAFETY.md](SAFETY.md#reporting-a-safety-problem).

## What this is not

**Not a security device.** It is a pet door. It can be opened by anyone with the
beacon, and the beacon can be cloned by anyone within radio range — the
advertisement it matches on is unauthenticated and broadcast in the clear. Keep
a real lock on anything that matters.

**Not a life-safety device.** Do not use it where a failure could harm a person,
and do not use it as the only thing standing between an animal and a hazard
(water, a road, livestock, weather that could kill overnight).

**Not suitable for animals you cannot check on.** The firmware fails open on a
stalled close, which protects against crushing, and that is the right trade — but
it means the door can be left open, and after
`CLOSE_RETRY_LIMIT` attempts it deliberately stays open and says so. If nobody
will see that message, nobody will know the door is open.

**Not a substitute for competence with mains wiring.** If your door controller is
mains-powered, the relay side of this project is mains-adjacent. If that sentence
does not already tell you what precautions to take, hire an electrician. See
[SAFETY.md](SAFETY.md#mains-voltage).

## The parts you are responsible for

The firmware cannot know your door. These are yours to get right, and all of them
can hurt an animal if wrong:

| | |
|---|---|
| **The mechanism** | Choose one that fails safe — stalls rather than forces, and does not guillotine. [SAFETY.md](SAFETY.md#choose-a-door-mechanism-that-fails-safe) |
| **Travel times** | They are the stall deadline. Measured in the wrong orientation they are wrong in both directions |
| **Thresholds** | Set too far out, the door opens for an animal that is not there yet; too close and it shuts behind one |
| **Schedules** | A scheduled lockout **will** shut an animal out at the boundary. [SAFETY.md](SAFETY.md#a-scheduled-lockout-will-shut-an-animal-out) |
| **Commissioning** | Watch a dozen full cycles before leaving it unattended |

## Use at your own risk

By building, flashing or running this, you accept that you are responsible for
the result: for the animal, for the mechanism, for the wiring, and for anything
the door does when you are not watching. If you are not willing to own that,
this project is not for you, and that is a perfectly reasonable place to land.
