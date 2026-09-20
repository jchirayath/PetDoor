# FAQ

### Can I use an AirTag?

No, and not for a fixable reason. AirTags rotate their Bluetooth address for
privacy — one produced **four different addresses in under two hours** — so
there is nothing stable to match. They also advertise far too slowly: at two
feet, the *typical* gap between readings was **5,140 ms**, against 349 ms for a
proper beacon further away. See [[The beacon]].

### Does it need WiFi or an account?

No. WiFi is optional and off unless you configure it; there is no account, no
cloud and no subscription. The door keeps its own rolling log and works
identically with no network.

### What happens when the beacon battery dies?

The door **closes and stays closed**. The signal goes stale, stale counts as
far. A beacon that is already dead when the ESP32 boots is handled more
carefully still: the door is not operated at all until the beacon has been heard
once, so a flat battery can never be mistaken for "the animal has gone away".

### What happens in a power cut?

The door stops wherever it is. On restart the firmware reports its position as
unknown and will not close for 30 seconds — but it cannot recover the door's
real position, and it has no way to know whether an animal is in the doorway.
This is one of several reasons the **door mechanism** matters more than the
code.

### Can it open for more than one animal?

Yes. The MAC list takes several beacons. Any one of them opens the door.

### Will it work for chickens?

That is where it started. The caveat is that a bird has to carry the beacon — a
leg band works but is fiddly. Many coop users run it the other way round: the
coop door stays on its dusk timer and this handles the daytime pop-door.

### Is it secure?

**No, and it is weaker than it looks.** BLE advertisements are unauthenticated
plaintext, so the door opens for anything broadcasting your beacon's address.
Reading that address takes a free phone app; rebroadcasting it takes a $10
board. There is no fix within BLE advertising because there is no shared secret
to verify against.

Fine against weather, rodents and the neighbour's cat. Not a barrier against a
person.

### Can it close on my pet?

The firmware will not close during the boot grace window, will not close without
having heard the beacon, waits out dropouts before closing, and cancels a
pending close the moment the beacon returns. But **it has no way to see into the
doorway.**

That is why the door mechanism is the most important purchase in the build. Buy
one with anti-pinch or obstruction detection. Read
[SAFETY.md](https://github.com/jchirayath/PetDoor/blob/main/docs/SAFETY.md).

### How far away does it open?

About **1 metre** by default, and fully tunable — you set the thresholds by
standing where the animal stands and reading the actual signal. Range depends on
the beacon's transmit power, the door material and where the beacon sits on the
collar.

### Why does it open instantly but close slowly?

Deliberate asymmetry. Opening late can shut an animal out; closing early can
shut a door on one. So the open path reacts immediately and the close path is
patient, rides out dropouts, and is cancelled the moment the beacon comes back.

### Do I need to solder?

For the button-tap conversion, usually yes — two wires per button onto the
controller's switch pads. If your door came with a **spare remote**, you can
solder across its button instead, which is easier. If the buttons are on a
connector, you may be able to tap that and avoid soldering entirely.

### It clicks but the door does not move

Most likely the relay contacts. Common 10 A relay boards are poorly suited to
switching a controller's *dry circuit* button input — they can click perfectly
while conducting intermittently. The tell: **a hand-short across COM/NO works
every time and the relay does not.**

Full diagnosis:
[TROUBLESHOOTING.md](https://github.com/jchirayath/PetDoor/blob/main/docs/TROUBLESHOOTING.md#the-relay-clicks-but-the-door-does-not-move)

### Can I use it without the dashboard?

Yes — that is the default. The dashboard, the log server and the upload are all
optional and all off unless configured.

### Where do I start?

[[Parts and cost]], then
[SAFETY.md](https://github.com/jchirayath/PetDoor/blob/main/docs/SAFETY.md),
then [[Converting a door]].
