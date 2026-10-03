#!/usr/bin/env python3
"""Generates docs/assets/wiring-esp32-2relay-headers.svg.

Pad positions come from the board's own pin card; this adds which wire lands on
which pad, and where a resistor goes. BOTH ends of every peripheral are drawn —
the return to GND is a wire you have to run, and a diagram that only shows the
signal leg leaves half the job implied.

Generated rather than hand-drawn: forty pads placed by hand drift, and a
drifted pad in a wiring diagram is somebody's 5 V into a GPIO. Each wire gets
its own vertical lane, and wires to an inner column cross between rows, so no
line is ever drawn across a pin name.
"""
import os

W, H = 1220, 1070
PITCH, TOP = 34, 128

LEFT = [("3V3", "GND"), ("SVP", "EN"), ("G34", "SVN"), ("G32", "G35"),
        ("G25", "G33"), ("G27", "G26"), ("G12", "G14"), ("SD2", "G13"),
        ("CMD", "SD3"), ("GND", "5V")]
RIGHT = [("GND", "G23"), ("G22", "TXD"), ("RXD", "G21"), ("G19", "G18"),
         ("G5", "G17"), ("G16", "G4"), ("G0", "SD1"), ("SD0", "CLK"),
         ("3V3", "GND")]

BX0, BX1 = 404, 776
LPO, LPI = 428, 496
RPI, RPO = 694, 744
PURPLE, AMBER, GREY, INK, RED = "#9B72CF", "#E9A23B", "#9AA5B1", "#C7D0DA", "#E76F51"
GNDC = "#7A8794"          # one colour for every ground run, because it is one net

def pads(side, name):
    """Every pad carrying this name — GND appears more than once per header."""
    rows = LEFT if side == "L" else RIGHT
    found = []
    for r, (a, b) in enumerate(rows):
        y = TOP + r * PITCH
        if a == name: found.append(((LPO if side == "L" else RPI), y))
        if b == name: found.append(((LPI if side == "L" else RPO), y))
    if not found: raise KeyError(name)
    return found

o = []
A = o.append
A('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d" width="%d" height="%d"' % (W, H, W, H))
A('     role="img" aria-label="Both wires of every peripheral traced to a header pin on the '
  'ESP32 2-relay board: piezo through 100 ohms to G27 and back to GND; open limit switch '
  'between G32 and GND; closed limit switch between G33 and GND; vibration sensor DO to G25, '
  'VCC to 3V3, GND to GND; status LED through 220 ohms from G23 to GND. Relays are on-board.">')
A('  <title>PetDoor — both wires of every peripheral, to the pin each lands on</title>')
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
A('  <text x="20" y="30" class="h1">Both wires of every peripheral, and the pin each lands on</text>')
A('  <text x="20" y="52" class="sub">Pin names are the board’s own. Both relays are driven '
  'on-board from GPIO 16 and 17 — there is nothing to wire for those.</text>')

bh = TOP + 9 * PITCH + 44 - 92
A('  <rect x="%d" y="92" width="%d" height="%d" rx="10" fill="#24425E"/>' % (BX0, BX1 - BX0, bh))
A('  <rect x="550" y="150" width="96" height="214" rx="6" fill="#9AA5B1" opacity="0.45"/>')
A('  <text x="598" y="252" text-anchor="middle" class="ttl" fill="#17212B">ESP32</text>')
A('  <text x="598" y="270" text-anchor="middle" class="pin" fill="#17212B">WROOM</text>')
A('  <text x="%d" y="114" class="hdr">LEFT HEADER</text>' % (LPO - 12))
A('  <text x="%d" y="114" class="hdr" text-anchor="end">RIGHT HEADER</text>' % (RPO + 12))

for side, rows, po, pi in (("L", LEFT, LPO, LPI), ("R", RIGHT, RPI, RPO)):
    for r, (a, b) in enumerate(rows):
        y = TOP + r * PITCH
        for col, name in ((0, a), (1, b)):
            if not name: continue
            x = po if col == 0 else pi
            fill = "#2A3F55" if name == "GND" else "#0F151B"
            A('  <circle cx="%d" cy="%d" r="5.5" fill="%s" stroke="%s" stroke-width="1.5"/>'
              % (x, y, fill, GNDC if name == "GND" else "#9AA5B1"))
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

PX, PW = 24, 262
EDGE = PX + PW
lane_n = [0]
def lane():
    lane_n[0] += 1
    return 296 + lane_n[0] * 10          # one lane per wire, never shared

def to_left(target, from_y, colour, dash=False, which=0):
    x, y = pads("L", target)[which]
    lx = lane()
    if x == LPO:
        wire([(EDGE, from_y), (lx, from_y), (lx, y), (x - 7, y)], colour, dash)
    else:
        gap = y + PITCH // 2 if y == TOP else y - PITCH // 2
        wire([(EDGE, from_y), (lx, from_y), (lx, gap), (x, gap),
              (x, y + (7 if gap > y else -7))], colour, dash)

# ---- left-hand peripherals, each with BOTH legs ----------------------------
box(PX, 148, PW, 78, PURPLE, "Piezo buzzer",
    ["G27 → [100 Ω] → piezo", "piezo → GND"])
to_left("G27", 186, PURPLE)
to_left("GND", 205, GNDC, which=0)                 # GND on the bottom row

box(PX, 250, PW, 78, AMBER, "OPEN limit switch", 
    ["G32 → reed", "reed → GND"], dash=True)
to_left("G32", 288, AMBER, True)
to_left("GND", 307, GNDC, True, which=0)

box(PX, 352, PW, 78, AMBER, "CLOSED limit switch",
    ["G33 → reed", "reed → GND"], dash=True)
to_left("G33", 390, AMBER, True)
to_left("GND", 409, GNDC, True, which=1)

box(PX, 454, PW, 97, AMBER, "Vibration sensor",
    ["DO → G25", "VCC → 3V3 (never 5 V)", "GND → GND"], dash=True)
to_left("G25", 492, AMBER, True)
to_left("3V3", 511, AMBER, True)
to_left("GND", 530, GNDC, True, which=1)           # GND on the top row

# ---- status LED, on the right header ---------------------------------------
(x23, y23) = pads("R", "G23")[0]
(xg, yg) = pads("R", "GND")[0]                     # top row, inner column
box(872, 148, 262, 78, GREY, "Status LED  (optional)",
    ["G23 → [220 Ω] → LED", "LED → GND"])
wire([(872, 186), (826, 179), (826, y23), (x23 + 7, y23)], GREY)
# GND is the inner pad: cross the outer column between rows, as on the left.
wire([(872, 205), (806, 198), (806, yg + PITCH // 2), (xg, yg + PITCH // 2),
      (xg, yg + 7)], GNDC)

ny = max(92 + bh, 551) + 46
A('  <text x="24" y="%d" class="ttl" fill="%s">Do I need a separate GND pin for each?</text>' % (ny, INK))
A('  <text x="24" y="%d" class="note">There are FOUR GND pads \u2014 two on each header \u2014 and all four are the same net, so it makes no electrical difference which you use. The two on</text>' % (ny + 21))
A('  <text x="24" y="%d" class="note">this side are drawn taking two returns each, nearest first.</text>' % (ny + 40))
A('  <text x="24" y="%d" class="note">What decides it is mechanical, not electrical: a 0.1 inch pin takes ONE Dupont connector. Two returns on one pad means joining them first \u2014 a</text>' % (ny + 63))
A('  <text x="24" y="%d" class="note">crimped splice, a WAGO, or a small screw terminal. Running every ground to one terminal block and taking a single wire from there to any</text>' % (ny + 82))
A('  <text x="24" y="%d" class="note">GND pad is just as correct, usually tidier, and is what most people end up with.</text>' % (ny + 101))

ry = ny + 130
A('  <text x="24" y="%d" class="ttl" fill="%s">Do I need a resistor?</text>' % (ry, INK))
A('  <text x="24" y="%d" class="note"><tspan font-weight="700" fill="%s">LED and piezo: YES.</tspan> An ESP32 pin is 3.3 V with no current limiting of its own. 220 Ω–1 kΩ for the LED, about 100 Ω for a piezo.</text>' % (ry + 22, INK))
A('  <text x="24" y="%d" class="note">Without one the pin sources well past its 12 mA rating and degrades — slowly, then intermittently, which is the worst way for it to fail.</text>' % (ry + 41))
A('  <text x="24" y="%d" class="note"><tspan font-weight="700" fill="%s">Limit switches: NO.</tspan> The firmware enables the ESP32’s internal pull-up and the reed simply shorts its pin to GND.</text>' % (ry + 66, INK))
A('  <text x="24" y="%d" class="note">Over a run longer than a metre or two, add an external 4.7–10 kΩ pull-up to 3V3 at the BOARD end: the internal one is about 45 kΩ, and a long</text>' % (ry + 85))
A('  <text x="24" y="%d" class="note">unshielded wire into a weak pull-up is an aerial. Phantom triggers are the symptom.</text>' % (ry + 104))
A('  <text x="24" y="%d" class="note"><tspan font-weight="700" fill="%s">Vibration module: NO.</tspan> It is a powered board with its own comparator and pull-up, and it drives the pin itself.</text>' % (ry + 129, INK))

sy = ry + 150
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
