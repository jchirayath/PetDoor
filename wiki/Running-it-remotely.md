# Running it remotely

The serial console stops being the answer the moment the door is screwed to a
wall. This page is what replaces it.

All of it is **optional and off by default**, and all of it rides the log
server. Nothing ever reaches *in* to the door: it calls out, and the reply to
its own upload is where anything waiting for it travels.

---

## Why it works this way

A door behind a home router has no address anybody can reach, and opening a
port to one so it can be told to unlock is a bad trade. So the door polls.

It uploads its log when the animal is away and the door is shut, and reads the
reply. Anything queued for it comes back in that reply, signed with the same
shared key and bound to a nonce the door chose for that request — so a reply
recorded off the wire cannot be played back at it later.

The cost is that **nothing is instant**. A command waits for the door's next
check-in, usually within five minutes. Everything below is designed around that
rather than pretending otherwise.

---

## What you can do

| | |
|---|---|
| **Change any setting** | Thresholds, dwell times, the filters, the relay pulse, travel time, the beacon list |
| **Open, close, lock, unlock** | From a browser or the command line |
| **Push new firmware** | Over WiFi. No cable, no ladder |
| **Get an email** | When somebody opens, locks, unlocks or reboots it |
| **Ask what it can see** | Upload its discovery table to find a new beacon's address |

From a terminal on the server:

```bash
petdoor-logserver.py --queue pulse 500
petdoor-logserver.py --queue lock
petdoor-logserver.py --doors          # what it sees, and its address
petdoor-logserver.py --firmware       # every build it has ever run
```

Or from the dashboard's **Controls** and **Settings** tabs, which do the same
things through the same validation.

---

## The dashboard can drive the door

Start the server with `--allow-web-control` and the private dashboard grows two
things: buttons for the door, and a form for every setting with the door's
**current values already filled in**.

It is off by default on purpose. An install whose private pages are protected by
nothing but an unguessable address must not quietly acquire a button that opens
the door.

**Read the proxy rules before turning it on.** An unprotected analytics page
leaks a household routine; an unprotected control endpoint lets a stranger open
the door. Same file, very different consequence:
**[WEB-DASHBOARD.md](https://github.com/jchirayath/PetDoor/blob/main/docs/WEB-DASHBOARD.md)**

Anything that loses state or takes the door off the air — reverting every
setting, rewriting the beacon list, rebooting, opening a firmware window — asks
before it does it.

---

## Firmware over the air

```bash
petdoor-logserver.py --queue ota     # opens a five-minute window
```

The door collects that on its next check-in, stays on WiFi, and you push to it.
A 1.3 MB image takes about twenty seconds.

**A bad push rolls itself back.** The new image is held *provisional* until it
completes an upload to the log server. If it boots but cannot reach the network,
the next reboot restores the previous one by itself. That is the difference
between a mistake and a ladder, and it is why the door confirms images itself
rather than letting the bootloader do it at startup.

---

## It can tell you when somebody uses it

Set a recipient and the server emails you when a command with a consequence is
queued: **open, lock, unlock, revert-all, beacon list, reboot, firmware
window**. It says which signed-in account asked.

Settings changes are deliberately excluded. A tuning session is a dozen commands
in a minute, and a mailbox that fills with `pulse 500` is one whose PetDoor mail
gets filtered into a folder nobody opens — at which point the message that
mattered is lost with the rest.

---

## The honest limits

**It is still not a lock.** Everything here is about managing a door you cannot
walk to. None of it changes the fact that the door opens for anything
broadcasting your beacon's address — see [[Home]].

**A command can wait five minutes.** If you need something to happen in the next
ten seconds, walk to the door. The delay is also the safety net: while a command
is still waiting, you can cancel it.

**The credentials cannot be changed remotely, ever.** Not the WiFi, the server
address, the shared key or the firmware password. Change one of those over the
air and a mistake takes the door off the network permanently, with no way back
but a ladder. The door refuses them and so does the server.
