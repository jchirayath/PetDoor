#!/usr/bin/env python3
"""Generates docs/assets/wiring-esp32-2relay-headers.svg.

Pad positions come from the board's own pin card; this only adds which wire
lands on which pad, and which of them need a resistor. Generated rather than
hand-drawn because forty pads placed by hand drift, and a drifted pad in a
wiring diagram is somebody's 5 V into a GPIO. Every wire gets its own vertical
lane, and wires to the inner column cross between rows, so none is ever drawn
through a pin name.
"""
import os

W, H = 1180, 924
PITCH, TOP = 34, 128

LEFT = [("3V3", "GND"), ("SVP", "EN"), ("G34", "SVN"), ("G32", "G35"),
        ("G25", "G33"), ("G27", "G26"), ("G12", "G14"), ("SD2", "G13"),
        ("CMD", "SD3"), ("GND", "5V")]
RIGHT = [("GND", "G23"), ("G22", "TXD"), ("RXD", "G21"), ("G19", "G18"),
         ("G5", "G17"), ("G16", "G4"), ("G0", "SD1"), ("SD0", "CLK"),
         ("3V3", "GND")]

BX0, BX1 = 400, 772
LPO, LPI = 424, 492
RPI, RPO = 690, 740
PURPLE, AMBER, GREY, INK, RED = "#9B72CF", "#E9A23B", "#9AA5B1", "#C7D0DA", "#E76F51"

def pad(side, name):
    rows = LEFT if side == "L" else RIGHT
    for r, (a, b) in enumerate(rows):
        y = TOP + r * PITCH
        if a == name:
            return ((LPO if side == "L" else RPI), y)
        if b == name:
            return ((LPI if side == "L" else RPO), y)
    raise KeyError(name)

o = []
A = o.append
A('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d" width="%d" height="%d"' % (W, H, W, H))
A('     role="img" aria-label="Which header pin each wire lands on for the ESP32 2-relay '
  'board, and where a resistor is needed: piezo buzzer through 100 ohms to G27, open limit '
  'switch to G32, closed limit switch to G33, vibration sensor DO to G25 with VCC to 3V3 '
  'and GND to GND, status LED through 220 ohms to G23. Both relays are driven on-board.">')
A('  <title>PetDoor — which header pin each wire goes to</title>')
A('''  <style>
    .h1  { font: 600 17px ui-sans-serif,-apple-system,Segoe UI,Roboto,Arial,sans-serif; fill:#8A96A3; }
    .sub { font: 13px ui-sans-serif,-apple-system,Segoe UI,Roboto,Arial,sans-serif; fill:#9AA5B1; }
    .pin { font: 600 11px ui-monospace,SFMono-Regular,Menlo,Consolas,monospace; fill:#C7D0DA; }
    .hdr { font: 600 10px ui-sans-serif,-apple-system,Segoe UI,Roboto,Arial,sans-serif;
           fill:#8A96A3; letter-spacing:0.09em; }
    .ttl { font: 600 13px ui-sans-serif,-apple-system,Segoe UI,Roboto,Arial,sans-serif; }
    .wire{ font: 12px ui-monospace,SFMono-Regular,Menlo,Consolas,monospace; fill:#9AA5B1; }
    .note{ font: 12px ui-sans-serif,-apple-system,Segoe UI,Roboto,Arial,sans-serif; fill:#9AA5B1; }
    .box { fill:none; stroke:#7A8794; stroke-width:1; opacity:0.45; }
  </style>''')
A('  <text x="20" y="30" class="h1">Which header pin each wire goes to</text>')
A('  <text x="20" y="52" class="sub">Pin names are the board’s own. Both relays are driven '
  'on-board from GPIO 16 and 17 — there is nothing to wire for those.</text>')

bh = TOP + 9 * PITCH + 44 - 92
A('  <rect x="%d" y="92" width="%d" height="%d" rx="10" fill="#24425E"/>' % (BX0, BX1 - BX0, bh))
A('  <rect x="546" y="150" width="96" height="214" rx="6" fill="#9AA5B1" opacity="0.45"/>')
A('  <text x="594" y="252" text-anchor="middle" class="ttl" fill="#17212B">ESP32</text>')
A('  <text x="594" y="270" text-anchor="middle" class="pin" fill="#17212B">WROOM</text>')
A('  <text x="%d" y="114" class="hdr">LEFT HEADER</text>' % (LPO - 12))
A('  <text x="%d" y="114" class="hdr" text-anchor="end">RIGHT HEADER</text>' % (RPO + 12))

for side, rows, po, pi in (("L", LEFT, LPO, LPI), ("R", RIGHT, RPI, RPO)):
    for r, (a, b) in enumerate(rows):
        y = TOP + r * PITCH
        for col, name in ((0, a), (1, b)):
            if not name:
                continue
            x = po if col == 0 else pi
            A('  <circle cx="%d" cy="%d" r="5.5" fill="#0F151B" stroke="#9AA5B1" stroke-width="1.5"/>'
              % (x, y))
            # Labels always point inward, leaving the outside of each header
            # clear for wires to approach without crossing a pin name.
            if side == "L":
                A('  <text x="%d" y="%d" class="pin">%s</text>' % (x + 12, y + 4, name))
            else:
                A('  <text x="%d" y="%d" class="pin" text-anchor="end">%s</text>' % (x - 12, y + 4, name))

def wire(pts, colour, dash=False):
    d = "M%g %g " % pts[0] + " ".join("L%g %g" % p for p in pts[1:])
    A('  <path d="%s" fill="none" stroke="%s" stroke-width="2"%s stroke-linejoin="round"/>'
      % (d, colour, ' stroke-dasharray="6 4"' if dash else ""))

def box(x, y, w, h, colour, title, lines, dash=False):
    A('  <rect x="%d" y="%d" width="%d" height="%d" rx="6" class="box"%s/>'
      % (x, y, w, h, ' stroke-dasharray="5 4"' if dash else ""))
    A('  <rect x="%d" y="%d" width="4" height="%d" rx="2" fill="%s"/>' % (x, y, h, colour))
    A('  <text x="%d" y="%d" class="ttl" fill="%s">%s</text>' % (x + 16, y + 21, colour, title))
    for i, ln in enumerate(lines):
        A('  <text x="%d" y="%d" class="wire">%s</text>' % (x + 16, y + 42 + i * 19, ln))

PX, PW = 24, 266
EDGE = PX + PW
LANE = {"G27": 300, "G32": 316, "G33": 332, "G25": 348, "3V3": 364, "GND": 380}

def to_pad(name, from_y, colour, dash=False):
    x, y = pad("L", name)
    lane = LANE[name]
    if x == LPO:
        wire([(EDGE, from_y), (lane, from_y), (lane, y), (x - 7, y)], colour, dash)
    else:
        # Inner column: cross the outer column halfway between two rows, where
        # there is neither a pad nor a label. The top row has no gap above it
        # (the header title is there), so that one comes in from below.
        gap = y + PITCH // 2 if y == TOP else y - PITCH // 2
        wire([(EDGE, from_y), (lane, from_y), (lane, gap), (x, gap),
              (x, y + (7 if gap > y else -7))], colour, dash)

box(PX, 150, PW, 80, PURPLE, "Piezo buzzer",
    ["G27 → [100 Ω] → piezo → GND",
     "resistor REQUIRED — note 1"])
to_pad("G27", 190, PURPLE)

box(PX, 258, PW, 96, AMBER, "Limit switches",
    ["OPEN   → G32, other leg → GND",
     "CLOSED → G33, other leg → GND",
     "no resistor — note 2"], dash=True)
to_pad("G32", 300, AMBER, True)
to_pad("G33", 319, AMBER, True)

box(PX, 382, PW, 114, AMBER, "Vibration sensor",
    ["DO  → G25", "VCC → 3V3  (never 5 V)", "GND → GND",
     "no resistor — note 3"], dash=True)
to_pad("G25", 424, AMBER, True)
to_pad("3V3", 443, AMBER, True)
to_pad("GND", 462, AMBER, True)

x23, y23 = pad("R", "G23")
box(842, y23 - 31, 250, 80, GREY, "Status LED  (optional)",
    ["G23 → [220 Ω] → LED → GND",
     "resistor REQUIRED — note 1"])
wire([(x23 + 7, y23), (812, y23), (842, y23)], GREY)

ny = 92 + bh + 52
A('  <text x="24" y="%d" class="ttl" fill="%s">Do I need a resistor?</text>' % (ny, INK))
A('  <text x="24" y="%d" class="note"><tspan font-weight="700" fill="%s">1 — LED and piezo: YES.</tspan> An ESP32 pin is 3.3 V with no current limiting of its own. 220 Ω–1 kΩ for the LED, about 100 Ω for a piezo.</text>' % (ny + 22, INK))
A('  <text x="24" y="%d" class="note">Without one the pin sources well past its 12 mA rating and degrades — slowly, then intermittently, which is the worst way for it to fail.</text>' % (ny + 41))
A('  <text x="24" y="%d" class="note"><tspan font-weight="700" fill="%s">2 — Limit switches: NO.</tspan> The firmware enables the ESP32’s internal pull-up and the switch simply shorts the pin to GND.</text>' % (ny + 66, INK))
A('  <text x="24" y="%d" class="note">Over a run longer than a metre or two, add an external 4.7–10 kΩ pull-up to 3V3 at the BOARD end. The internal one is about 45 kΩ, and a long</text>' % (ny + 85))
A('  <text x="24" y="%d" class="note">unshielded wire into a weak pull-up is an aerial; phantom triggers are the symptom.</text>' % (ny + 104))
A('  <text x="24" y="%d" class="note"><tspan font-weight="700" fill="%s">3 — Vibration module: NO.</tspan> It is a powered board with its own comparator and pull-up, and it drives the pin itself.</text>' % (ny + 129, INK))

sy = ny + 150
A('  <rect x="20" y="%d" width="%d" height="80" rx="6" fill="%s" opacity="0.07"/>' % (sy, W - 40, RED))
A('  <rect x="20" y="%d" width="4" height="80" rx="2" fill="%s"/>' % (sy, RED))
A('  <text x="42" y="%d" class="ttl" fill="%s">Check your own board before you wire anything</text>' % (sy + 24, RED))
A('  <text x="42" y="%d" class="note">This map is the pin card for this board family, and relay counts differ across it. On the 8-relay version G32, G25, G27 and G12 drive relays;</text>' % (sy + 46))
A('  <text x="42" y="%d" class="note">on this 2-relay board the relays sit on GPIO 16 and 17 and those four pins are free. Meter them before you trust either statement.</text>' % (sy + 65))
A('</svg>')

p = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                 "wiring-esp32-2relay-headers.svg")
open(p, "w").write("\n".join(o) + "\n")
print("  wrote %d bytes" % os.path.getsize(p))
