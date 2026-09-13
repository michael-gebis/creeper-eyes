#!/usr/bin/env python3
"""Generate the carrier board: schematic, layout, footprint, project.

    "%LOCALAPPDATA%/Programs/KiCad/10.0/bin/python.exe" hardware/gen_board.py

Run with the Python that ships inside KiCad, because the layout is built
through KiCad's own `pcbnew` module -- there is no other way to get footprints
out of its libraries with their pad geometry intact.  The schematic is plain
s-expression text and is written directly.

Why a script rather than files drawn by hand: the whole board is a socket for
the DevKit, three connectors and a capacitor, wired together the way
docs/WIRING.md says.  There is one dimension that cannot be looked up -- the
pitch between the DevKit's two header rows, which varies between clones --
and it has to be measured with calipers.  Changing it moves half the board.
With a script that is one number and one re-run; by hand it is an afternoon
of dragging tracks.  The routing is a small set of coordinates derived from
the pitch, and the design rule check catches anything the derivation gets
wrong.

So there is one design and several boards: one per pitch in VARIANTS, each in
its own directory named for the number, so the zip you upload says which
DevKit it fits.

The layout is Manhattan: horizontal runs on the top copper, vertical runs on
the bottom, vias where they meet.  Horizontal runs sit on "lanes" halfway
between the socket rows, so they pass between pads.  Nothing clever.

Outputs, per variant, in rows-<pitch>mm/ beside this file:

    <name>.kicad_pro          project
    <name>.kicad_sch          schematic
    <name>.kicad_pcb          board
    <name>.pretty/            the DevKit footprint, so KiCad can find it
    <name>.kicad_sym          the symbols the schematic uses
    fp-lib-table, sym-lib-table pointing at the two above

Then `make_outputs.py` runs ERC, DRC and exports the Gerbers for each.
"""

from __future__ import annotations

import os
import sys
import uuid
from dataclasses import dataclass, field
from typing import Optional

HERE = os.path.dirname(os.path.abspath(__file__))

# THE ONE NUMBER TO MEASURE ---------------------------------------------------
#
# Centre-to-centre distance between the DevKit's two rows of pins, in mm.
# Nominally 25.4 (1.0") on the 30-pin DOIT ESP32 DevKit V1.  The board this
# was first built for -- a 30-pin "DevKit V1" clone -- came out at 25.10:
# 25.72 mm across the outside of both rows and 24.48 across the inside,
# measured with calipers at the base of the pins where they enter the spacer,
# not at the tips, which are often toed in.  The average is the pitch and the
# difference is the pin width (0.62, against 0.64 measured directly), which
# checks the measurement.
#
# Along each row the pitch is 2.54 on every board, because that is the header
# strip itself; only the distance between the rows is the clone's own choice.
#
# Measure yours, and if it is neither of these add a row.  27.94 (1.1")
# clones exist too, and the 38-pin boards differ again.
VARIANTS: dict[str, str] = {
    "25.10": "measured on the author's DevKit clone",
    "25.4": "the nominal 1.0\" of the DOIT reference design",
}


def variant_name(pitch: str) -> str:
    return f"creeper-eyes-rows-{pitch}mm"


def variant_dir(pitch: str) -> str:
    return os.path.join(HERE, f"rows-{pitch}mm")


# GEOMETRY ----------------------------------------------------------------------
#
# Everything is on a 2.54 mm row grid starting at ROW0_Y, so that every pad
# on the board shares a set of rows and the lanes between them are the same
# everywhere.  Row r is the y of the r-th pin (1-based); lane r is halfway
# between rows r and r+1.

P = 2.54
ROW0_Y = 6.0


def row(r: int) -> float:
    return ROW0_Y + P * (r - 1)


def lane(r: int) -> float:
    return row(r) + P / 2


J1_X = 23.3                  # DevKit left row (EN ... VIN), pin 1 at row 1
J3_X = 8.0                   # Frank's right eye (your left), rows 4-10
C1_X = J3_X                  # decoupling, + at 37.02, - at 39.02
C1_PLUS_Y = row(13) + 0.54   # 37.02: two millimetres above the GND run
C1_MINUS_Y = row(14)         # 39.02: on the GND run itself
GL_X = 13.0                  # GND rail, left, bottom copper
PL_X = 14.5                  # 3V3 rail, left
DC_ESC = J1_X - 1.6          # where DC leaves its pad row for its lane
BOARD_H = 56.0
HOLE_INSET = 4.0

TRACK = 0.25
VIA_D = 0.8
VIA_DRILL = 0.4

# The DevKit's own outline, for the silkscreen.  Measured on the same clone
# as the pitch: 6.5 mm from the centre of pin 1 to the antenna end, 9.5 from
# pin 15 to the USB end, 51.6 overall.  With pin 1 at ROW0_Y the antenna end
# sits half a millimetre past the board edge -- flush, near enough -- and
# nothing conductive is under it in any case.
DEVKIT_ABOVE_PIN1 = 6.5
DEVKIT_BELOW_PIN15 = 9.5

F, B = "F.Cu", "B.Cu"


@dataclass
class Layout:
    """Everything to the right of the DevKit's left row follows the pitch."""

    pitch: float
    J2_X: float = field(init=False)      # DevKit right row (D23 ... 3V3)
    J4_X: float = field(init=False)      # Frank's left eye (your right), rows 4-10
    J5_X: float = field(init=False)      # RTC, rows 12-17
    GR_X: float = field(init=False)      # GND rail, right
    PR_X: float = field(init=False)      # 3V3 rail, right
    CSR_ESC: float = field(init=False)   # escape columns: where a signal
    SDA_ESC: float = field(init=False)   # leaves its pad row for a lane
    CSL_ESC: float = field(init=False)
    SCL_ESC: float = field(init=False)
    BOARD_W: float = field(init=False)
    DEVKIT_W: float = field(init=False)
    tracks: list[tuple[str, str, list[tuple[float, float]]]] = field(init=False)
    vias: list[tuple[str, float, float]] = field(init=False)

    def __post_init__(self) -> None:
        self.J2_X = J1_X + self.pitch
        self.J4_X = self.J2_X + 15.3
        self.J5_X = self.J4_X
        self.GR_X = self.J2_X + 6.3
        self.PR_X = self.J2_X + 7.8
        self.CSR_ESC = self.J2_X - 1.6
        self.SDA_ESC = self.J2_X + 1.6
        self.CSL_ESC = self.J2_X + 2.8
        self.SCL_ESC = self.J2_X + 9.3
        self.BOARD_W = self.J4_X + 8.0
        self.DEVKIT_W = self.pitch + 2.9   # ~1.45 mm beyond the pins each side
        self.route()

    def route(self) -> None:
        J2_X, J4_X, J5_X = self.J2_X, self.J4_X, self.J5_X
        GR_X, PR_X = self.GR_X, self.PR_X
        CSR_ESC, SDA_ESC, CSL_ESC, SCL_ESC = (self.CSR_ESC, self.SDA_ESC,
                                              self.CSL_ESC, self.SCL_ESC)
        # Top copper: horizontal.  Bottom copper: vertical.  Each entry is a
        # polyline on one layer.  The comments say which pad each run starts
        # from, in the row numbering above.
        self.tracks = [
            # 3V3: J2 r15 up to lane 14, across between the rails, down each rail.
            ("3V3", F, [(J2_X, row(15)), (J2_X, lane(14))]),
            ("3V3", F, [(PL_X, lane(14)), (PR_X, lane(14))]),
            ("3V3", B, [(PL_X, row(4)), (PL_X, lane(14))]),
            ("3V3", F, [(J3_X, row(4)), (PL_X, row(4))]),          # J3 VCC
            ("3V3", F, [(C1_X, C1_PLUS_Y), (PL_X, C1_PLUS_Y)]),    # C1 +
            ("3V3", B, [(PR_X, row(4)), (PR_X, row(16))]),
            ("3V3", F, [(J4_X, row(4)), (PR_X, row(4))]),          # J4 VCC
            ("3V3", F, [(J5_X, row(16)), (PR_X, row(16))]),        # J5 VCC
            # GND: one run along row 14 from C1 through both DevKit GND pins
            # to the right rail, and a rail down each side.
            ("GND", F, [(C1_X, row(14)), (GR_X, row(14))]),
            ("GND", B, [(GL_X, row(5)), (GL_X, row(14))]),
            ("GND", F, [(J3_X, row(5)), (GL_X, row(5))]),          # J3 GND
            ("GND", B, [(GR_X, row(5)), (GR_X, row(17))]),
            ("GND", F, [(J4_X, row(5)), (GR_X, row(5))]),          # J4 GND
            ("GND", F, [(J5_X, row(17)), (GR_X, row(17))]),        # J5 GND
            # DIN on lane 6: J3 r6 and J4 r6 stub down, J2 r7 stubs up.
            ("DIN", F, [(J3_X, row(6)), (J3_X, lane(6))]),
            ("DIN", F, [(J3_X, lane(6)), (J4_X, lane(6))]),
            ("DIN", F, [(J2_X, row(7)), (J2_X, lane(6))]),
            ("DIN", F, [(J4_X, row(6)), (J4_X, lane(6))]),
            # CLK on lane 7: J3 r7, J4 r7 down; J2 r8 up.
            ("CLK", F, [(J3_X, row(7)), (J3_X, lane(7))]),
            ("CLK", F, [(J3_X, lane(7)), (J4_X, lane(7))]),
            ("CLK", F, [(J2_X, row(8)), (J2_X, lane(7))]),
            ("CLK", F, [(J4_X, row(7)), (J4_X, lane(7))]),
            # CS_R on lane 8 from J3 r8, then down an escape column to J2 r13.
            ("CS_R", F, [(J3_X, row(8)), (J3_X, lane(8))]),
            ("CS_R", F, [(J3_X, lane(8)), (CSR_ESC, lane(8))]),
            ("CS_R", B, [(CSR_ESC, lane(8)), (CSR_ESC, row(13))]),
            ("CS_R", F, [(CSR_ESC, row(13)), (J2_X, row(13))]),
            # CS_L: J2 r11 out to an escape column, up to lane 8, across to J4 r8.
            ("CS_L", F, [(J2_X, row(11)), (CSL_ESC, row(11))]),
            ("CS_L", B, [(CSL_ESC, row(11)), (CSL_ESC, lane(8))]),
            ("CS_L", F, [(CSL_ESC, lane(8)), (J4_X, lane(8))]),
            ("CS_L", F, [(J4_X, row(8)), (J4_X, lane(8))]),
            # DC: J1 r7 out to the left, down to lane 9, then the full width.
            ("DC", F, [(J1_X, row(7)), (DC_ESC, row(7))]),
            ("DC", B, [(DC_ESC, row(7)), (DC_ESC, lane(9))]),
            ("DC", F, [(J3_X, lane(9)), (J4_X, lane(9))]),
            ("DC", F, [(J3_X, row(9)), (J3_X, lane(9))]),
            ("DC", F, [(J4_X, row(9)), (J4_X, lane(9))]),
            # RST on lane 10: all three pads are on row 10 and stub down.
            ("RST", F, [(J1_X, row(10)), (J1_X, lane(10))]),
            ("RST", F, [(J3_X, lane(10)), (J4_X, lane(10))]),
            ("RST", F, [(J3_X, row(10)), (J3_X, lane(10))]),
            ("RST", F, [(J4_X, row(10)), (J4_X, lane(10))]),
            # SCL: J2 r2 right past the rails, down, in to J5 r14.
            ("SCL", F, [(J2_X, row(2)), (SCL_ESC, row(2))]),
            ("SCL", B, [(SCL_ESC, row(2)), (SCL_ESC, row(14))]),
            ("SCL", F, [(SCL_ESC, row(14)), (J5_X, row(14))]),
            # SDA: J2 r5 out one step, down, across to J5 r15.
            ("SDA", F, [(J2_X, row(5)), (SDA_ESC, row(5))]),
            ("SDA", B, [(SDA_ESC, row(5)), (SDA_ESC, row(15))]),
            ("SDA", F, [(SDA_ESC, row(15)), (J5_X, row(15))]),
        ]
        self.vias = [
            ("3V3", PL_X, lane(14)), ("3V3", PR_X, lane(14)),
            ("3V3", PL_X, row(4)), ("3V3", PL_X, C1_PLUS_Y),
            ("3V3", PR_X, row(4)), ("3V3", PR_X, row(16)),
            ("GND", GL_X, row(14)), ("GND", GR_X, row(14)),
            ("GND", GL_X, row(5)), ("GND", GR_X, row(5)), ("GND", GR_X, row(17)),
            ("CS_R", CSR_ESC, lane(8)), ("CS_R", CSR_ESC, row(13)),
            ("CS_L", CSL_ESC, row(11)), ("CS_L", CSL_ESC, lane(8)),
            ("DC", DC_ESC, row(7)), ("DC", DC_ESC, lane(9)),
            ("SCL", SCL_ESC, row(2)), ("SCL", SCL_ESC, row(14)),
            ("SDA", SDA_ESC, row(5)), ("SDA", SDA_ESC, row(15)),
        ]


# NETLIST -------------------------------------------------------------------------
#
# The DevKit is one footprint, U1, with pads 1-15 down the left row and 16-30
# down the right, the way the module's own silkscreen reads with the USB
# connector towards you.  Anything not listed is not connected.

DEVKIT_LEFT = ["EN", "VP", "VN", "D34", "D35", "D32", "D33", "D25", "D26",
               "D27", "D14", "D12", "D13", "GND", "VIN"]
DEVKIT_RIGHT = ["D23", "D22", "TX0", "RX0", "D21", "D19", "D18", "D5", "TX2",
                "RX2", "D4", "D2", "D15", "GND", "3V3"]
DEVKIT_PINS = DEVKIT_LEFT + DEVKIT_RIGHT

OLED_PINS = ["VCC", "GND", "DIN", "CLK", "CS", "DC", "RST"]
RTC_PINS = ["32K", "SQW", "SCL", "SDA", "VCC", "GND"]

# (reference, pad number, net)
CONNECTIONS: list[tuple[str, int, str]] = [
    ("U1", 7, "DC"),      # D33
    ("U1", 10, "RST"),    # D27
    ("U1", 14, "GND"),
    ("U1", 17, "SCL"),    # D22
    ("U1", 20, "SDA"),    # D21
    ("U1", 22, "DIN"),    # D18
    ("U1", 23, "CLK"),    # D5
    ("U1", 26, "CS_L"),   # D4
    ("U1", 28, "CS_R"),   # D15
    ("U1", 29, "GND"),
    ("U1", 30, "3V3"),
    ("J3", 1, "3V3"), ("J3", 2, "GND"), ("J3", 3, "DIN"), ("J3", 4, "CLK"),
    ("J3", 5, "CS_R"), ("J3", 6, "DC"), ("J3", 7, "RST"),
    ("J4", 1, "3V3"), ("J4", 2, "GND"), ("J4", 3, "DIN"), ("J4", 4, "CLK"),
    ("J4", 5, "CS_L"), ("J4", 6, "DC"), ("J4", 7, "RST"),
    ("J5", 3, "SCL"), ("J5", 4, "SDA"), ("J5", 5, "3V3"), ("J5", 6, "GND"),
    ("C1", 1, "3V3"), ("C1", 2, "GND"),
]

NETS = ["3V3", "GND", "DIN", "CLK", "CS_R", "CS_L", "DC", "RST", "SCL", "SDA"]

PIN_NAMES = {"U1": DEVKIT_PINS, "J3": OLED_PINS, "J4": OLED_PINS, "J5": RTC_PINS}

# Shown on both the schematic and the board, and checked to be the same.
VALUES = {"U1": "ESP32 DevKit V1 (30 pin)", "J3": "OLED, Frank's right eye",
          "J4": "OLED, Frank's left eye", "J5": "DS3231 RTC (optional)", "C1": "10u"}


def net_of(ref: str, pad: int) -> Optional[str]:
    for r, p, n in CONNECTIONS:
        if r == ref and p == pad:
            return n
    return None


# BOARD ---------------------------------------------------------------------------


def build_board(kicad_share: str, pitch: str, out: str, name: str) -> None:
    import pcbnew  # only importable from KiCad's own Python
    from pcbnew import VECTOR2I

    L = Layout(float(pitch))

    def mm(v: float) -> int:
        return pcbnew.FromMM(v)

    def pt(x: float, y: float) -> VECTOR2I:
        return VECTOR2I(mm(x), mm(y))

    board = pcbnew.BOARD()
    layer = {F: pcbnew.F_Cu, B: pcbnew.B_Cu}

    nets: dict[str, pcbnew.NETINFO_ITEM] = {}
    for n in NETS:
        item = pcbnew.NETINFO_ITEM(board, n)
        board.Add(item)
        nets[n] = item

    fplib = os.path.join(kicad_share, "footprints")

    def load(lib: str, fpname: str) -> pcbnew.FOOTPRINT:
        fp = pcbnew.FootprintLoad(os.path.join(fplib, lib + ".pretty"), fpname)
        if fp is None:
            raise SystemExit(f"footprint not found: {lib}:{fpname}")
        fp.SetFPID(pcbnew.LIB_ID(lib, fpname))  # loading by path loses the nickname
        return fp

    def place(fp: pcbnew.FOOTPRINT, ref: str, value: str, x: float,
              y: float, rot: float = 0) -> pcbnew.FOOTPRINT:
        fp.SetReference(ref)
        fp.SetValue(value)
        fp.SetPosition(pt(x, y))
        fp.SetOrientationDegrees(rot)
        fp.Reference().SetVisible(False)
        fp.Value().SetVisible(False)
        board.Add(fp)
        for pad in fp.Pads():
            try:
                num = int(pad.GetNumber())
            except ValueError:
                continue
            n = net_of(ref, num)
            if n:
                pad.SetNet(nets[n])
            elif ref in PIN_NAMES:
                # The schematic names a no-connect pin's net after the pin;
                # carrying the same name here keeps the parity check exact.
                n = f"unconnected-({ref}-{PIN_NAMES[ref][num - 1]}-Pad{num})"
                item = pcbnew.NETINFO_ITEM(board, n)
                board.Add(item)
                pad.SetNet(item)
        return fp

    # The DevKit footprint: built here rather than drawn, and saved to the
    # project library so the schematic's footprint field resolves.
    u1 = pcbnew.FOOTPRINT(board)
    u1.SetFPID(pcbnew.LIB_ID(name, "ESP32_DevKit_V1_30pin"))
    u1.SetLibDescription("ESP32 DevKit V1, 30 pin, socketed on two 1x15 "
                         f"headers {pitch} mm apart")
    half = L.pitch / 2
    span = P * 14
    for i in range(30):
        col = -half if i < 15 else half
        r = i % 15
        pad = pcbnew.PAD(u1)
        pad.SetNumber(str(i + 1))
        pad.SetAttribute(pcbnew.PAD_ATTRIB_PTH)
        pad.SetShape(pcbnew.PAD_SHAPE_RECT if i == 0 else pcbnew.PAD_SHAPE_CIRCLE)
        pad.SetSize(VECTOR2I(mm(1.7), mm(1.7)))
        pad.SetDrillSize(VECTOR2I(mm(1.0), mm(1.0)))
        pad.SetLayerSet(pad.PTHMask())
        pad.SetPosition(pt(col, -span / 2 + P * r))
        u1.Add(pad)
    for col in (-half, half):
        s = pcbnew.PCB_SHAPE(u1)
        s.SetShape(pcbnew.SHAPE_T_RECT)
        s.SetStart(pt(col - 1.27, -span / 2 - 1.27))
        s.SetEnd(pt(col + 1.27, span / 2 + 1.27))
        s.SetLayer(pcbnew.F_Fab)
        s.SetWidth(mm(0.1))
        u1.Add(s)
    cy = pcbnew.PCB_SHAPE(u1)
    cy.SetShape(pcbnew.SHAPE_T_RECT)
    cy.SetStart(pt(-half - 1.52, -span / 2 - 1.52))
    cy.SetEnd(pt(half + 1.52, span / 2 + 1.52))
    cy.SetLayer(pcbnew.F_CrtYd)
    cy.SetWidth(mm(0.05))
    u1.Add(cy)
    u1_y = (row(1) + row(15)) / 2
    place(u1, "U1", VALUES["U1"], (J1_X + L.J2_X) / 2, u1_y)

    place(load("Connector_PinHeader_2.54mm", "PinHeader_1x07_P2.54mm_Vertical"),
          "J3", VALUES["J3"], J3_X, row(4))
    place(load("Connector_PinHeader_2.54mm", "PinHeader_1x07_P2.54mm_Vertical"),
          "J4", VALUES["J4"], L.J4_X, row(4))
    place(load("Connector_PinHeader_2.54mm", "PinHeader_1x06_P2.54mm_Vertical"),
          "J5", VALUES["J5"], L.J5_X, row(12))
    c1 = load("Capacitor_THT", "CP_Radial_D5.0mm_P2.00mm")
    c1 = place(c1, "C1", VALUES["C1"], C1_X, C1_PLUS_Y, 90)
    # The rotation above is meant to put pin 2 directly below pin 1; check,
    # because getting it backwards would short 3V3 to GND through nothing.
    p2 = c1.FindPadByNumber("2").GetPosition()
    if abs(pcbnew.ToMM(p2.x) - C1_X) > 0.01 or abs(pcbnew.ToMM(p2.y) - C1_MINUS_Y) > 0.01:
        c1.SetOrientationDegrees(-90)
        p2 = c1.FindPadByNumber("2").GetPosition()
        if abs(pcbnew.ToMM(p2.x) - C1_X) > 0.01 or abs(pcbnew.ToMM(p2.y) - C1_MINUS_Y) > 0.01:
            raise SystemExit("C1 pad 2 is not where the GND run expects it")

    # Check every pad this script routes to is where the routing thinks.
    expect = {
        ("U1", 7): (J1_X, row(7)), ("U1", 10): (J1_X, row(10)),
        ("U1", 14): (J1_X, row(14)), ("U1", 17): (L.J2_X, row(2)),
        ("U1", 20): (L.J2_X, row(5)), ("U1", 22): (L.J2_X, row(7)),
        ("U1", 23): (L.J2_X, row(8)), ("U1", 26): (L.J2_X, row(11)),
        ("U1", 28): (L.J2_X, row(13)), ("U1", 29): (L.J2_X, row(14)),
        ("U1", 30): (L.J2_X, row(15)),
        ("J5", 3): (L.J5_X, row(14)), ("J5", 4): (L.J5_X, row(15)),
        ("J5", 5): (L.J5_X, row(16)), ("J5", 6): (L.J5_X, row(17)),
        ("C1", 1): (C1_X, C1_PLUS_Y), ("C1", 2): (C1_X, C1_MINUS_Y),
    }
    for i in range(1, 8):
        expect[("J3", i)] = (J3_X, row(3 + i))
        expect[("J4", i)] = (L.J4_X, row(3 + i))
    for fp in board.GetFootprints():
        for pad in fp.Pads():
            key = (fp.GetReference(), int(pad.GetNumber()) if pad.GetNumber().isdigit() else 0)
            if key in expect:
                ex, ey = expect[key]
                pos = pad.GetPosition()
                if abs(pcbnew.ToMM(pos.x) - ex) > 0.01 or abs(pcbnew.ToMM(pos.y) - ey) > 0.01:
                    raise SystemExit(f"{key} at ({pcbnew.ToMM(pos.x)}, {pcbnew.ToMM(pos.y)}), "
                                     f"expected ({ex}, {ey})")

    for i, (hx, hy) in enumerate([(HOLE_INSET, HOLE_INSET),
                                  (L.BOARD_W - HOLE_INSET, HOLE_INSET),
                                  (HOLE_INSET, BOARD_H - HOLE_INSET),
                                  (L.BOARD_W - HOLE_INSET, BOARD_H - HOLE_INSET)]):
        h = place(load("MountingHole", "MountingHole_3.2mm_M3"), f"H{i + 1}", "M3", hx, hy)
        h.SetBoardOnly(True)
        h.SetExcludedFromBOM(True)

    # Copper.
    for n, lay, pts in L.tracks:
        for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
            t = pcbnew.PCB_TRACK(board)
            t.SetStart(pt(x0, y0))
            t.SetEnd(pt(x1, y1))
            t.SetWidth(mm(TRACK))
            t.SetLayer(layer[lay])
            t.SetNet(nets[n])
            board.Add(t)
    for n, x, y in L.vias:
        v = pcbnew.PCB_VIA(board)
        v.SetPosition(pt(x, y))
        v.SetViaType(pcbnew.VIATYPE_THROUGH)
        v.SetDrill(mm(VIA_DRILL))
        v.SetWidth(mm(VIA_D))
        v.SetLayerPair(pcbnew.F_Cu, pcbnew.B_Cu)
        v.SetNet(nets[n])
        board.Add(v)

    # Outline.
    edge = pcbnew.PCB_SHAPE(board)
    edge.SetShape(pcbnew.SHAPE_T_RECT)
    edge.SetStart(pt(0, 0))
    edge.SetEnd(pt(L.BOARD_W, BOARD_H))
    edge.SetLayer(pcbnew.Edge_Cuts)
    edge.SetWidth(mm(0.1))
    board.Add(edge)

    # Silkscreen.
    def text(s: str, x: float, y: float, size: float = 1.0, just: str = "center",
             lay: int = pcbnew.F_SilkS, rot: float = 0, bold: bool = False) -> None:
        t = pcbnew.PCB_TEXT(board)
        t.SetText(s)
        t.SetPosition(pt(x, y))
        t.SetLayer(lay)
        t.SetTextSize(VECTOR2I(mm(size), mm(size)))
        t.SetTextThickness(mm(0.15 if size <= 1.0 else 0.2))
        t.SetTextAngleDegrees(rot)
        t.SetBold(bold)
        t.SetMirrored(lay == pcbnew.B_SilkS)
        t.SetHorizJustify({"left": pcbnew.GR_TEXT_H_ALIGN_LEFT,
                           "right": pcbnew.GR_TEXT_H_ALIGN_RIGHT,
                           "center": pcbnew.GR_TEXT_H_ALIGN_CENTER}[just])
        board.Add(t)

    def line(x0: float, y0: float, x1: float, y1: float, w: float = 0.15) -> None:
        s = pcbnew.PCB_SHAPE(board)
        s.SetShape(pcbnew.SHAPE_T_SEGMENT)
        s.SetStart(pt(x0, y0))
        s.SetEnd(pt(x1, y1))
        s.SetLayer(pcbnew.F_SilkS)
        s.SetWidth(mm(w))
        board.Add(s)

    # Every DevKit pin, named as the module's own silkscreen names it, so a
    # module with a different pinout is caught by eye before it is powered.
    for i, pn in enumerate(DEVKIT_LEFT):
        text(pn, J1_X - 2.3, row(i + 1), 0.9, "right")
    for i, pn in enumerate(DEVKIT_RIGHT):
        text(pn, L.J2_X + 2.3, row(i + 1), 0.9, "left")

    for i, pn in enumerate(OLED_PINS):
        text(pn, J3_X - 1.7, row(4 + i), 0.9, "right")
        text(pn, L.J4_X + 1.7, row(4 + i), 0.9, "left")
    for i, pn in enumerate(RTC_PINS):
        text(pn, L.J5_X + 1.7, row(12 + i), 0.9, "left")

    for x, who, you in ((J3_X + 1.0, "RIGHT", "(your left)"),
                        (L.J4_X - 1.0, "LEFT", "(your right)")):
        text("FRANK'S", x, row(4) - 6.2, 0.9, bold=True)
        text(who, x, row(4) - 4.6, 0.9, bold=True)
        text(you, x, row(4) - 3.0, 0.8)
    text("DS3231", L.J5_X - 1.0, row(12) - 2.2, 0.9, "center", bold=True)

    # The DevKit's two short edges, and which way round it goes.  The long
    # edges are the sockets themselves.  The antenna end may be past the
    # board edge, in which case its line is left off.
    dk_l = J1_X - L.DEVKIT_W / 2 + L.pitch / 2
    dk_r = dk_l + L.DEVKIT_W
    top = row(1) - DEVKIT_ABOVE_PIN1
    bottom = row(15) + DEVKIT_BELOW_PIN15
    line(dk_l, bottom, dk_r, bottom)
    if top > 0.5:
        line(dk_l, top, dk_r, top)
    cx = (J1_X + L.J2_X) / 2
    text("USB", cx, bottom - 1.4, 1.0, bold=True)
    text("ANTENNA", cx, max(top, 0.0) + 1.4, 1.0, bold=True)
    # The pitch, where it can be read with the DevKit unplugged: the one
    # thing that tells two otherwise identical boards apart.
    text(f"ESP32 DevKit V1  (30 pin, rows {pitch} mm)", cx, bottom + 1.6, 0.8)

    text(f"creeper-eyes carrier  rev A  rows {pitch} mm", cx, BOARD_H - 1.6, 1.0, bold=True)
    text("github.com/michael-gebis/creeper-eyes", L.BOARD_W / 2, 1.6, 0.8, lay=pcbnew.B_SilkS)
    text(f"rows {pitch} mm", L.BOARD_W / 2, BOARD_H - 1.6, 1.0, lay=pcbnew.B_SilkS, bold=True)

    # Design rules the fab is comfortable with and the DRC will hold us to.
    ds = board.GetDesignSettings()
    ds.m_TrackMinWidth = mm(0.2)
    ds.m_ViasMinSize = mm(0.6)
    ds.m_MinThroughDrill = mm(0.3)
    ds.m_MinClearance = mm(0.2)
    ds.m_CopperEdgeClearance = mm(0.3)
    ds.m_SolderMaskMinWidth = mm(0.1)

    board.Save(os.path.join(out, name + ".kicad_pcb"))

    # The DevKit footprint into a project library, for the schematic to point
    # at.  Reset the position first so it sits at its own origin.
    libdir = os.path.join(out, name + ".pretty")
    os.makedirs(libdir, exist_ok=True)
    io = pcbnew.PCB_IO_KICAD_SEXPR()
    copy = pcbnew.FOOTPRINT(u1)
    copy.SetPosition(VECTOR2I(0, 0))
    for pad in copy.Pads():
        pad.SetNet(pcbnew.NETINFO_ITEM(None, ""))
    io.FootprintSave(libdir, copy)


# SCHEMATIC ------------------------------------------------------------------------
#
# Written as text.  Three symbols are drawn here rather than taken from
# KiCad's libraries so that the pins carry the names that matter --
# "D18", "CS", "SDA" -- instead of "Pin_7".  Global labels do the wiring.


def uid() -> str:
    return str(uuid.uuid4())


ROOT_UUID = "3c0ffee0-5eed-4c0d-b0a7-c0ffee000001"

SCH_VERSION = 20250114


def sym_pin(pin: str, num: int, x: float, y: float, angle: int, kind: str = "passive",
            length: float = 5.08) -> str:
    return f"""			(pin {kind} line
				(at {x:g} {y:g} {angle})
				(length {length:g})
				(name "{pin}" (effects (font (size 1.27 1.27))))
				(number "{num}" (effects (font (size 1.27 1.27))))
			)
"""


def sym_header(lib: str, sym: str, ref: str, desc: str, fp: str = "") -> str:
    return f"""		(symbol "{lib}:{sym}"
			(pin_names (offset 1.016))
			(exclude_from_sim no) (in_bom yes) (on_board yes)
			(property "Reference" "{ref}" (at 0 0 0) (effects (font (size 1.27 1.27))))
			(property "Value" "{sym}" (at 0 0 0) (effects (font (size 1.27 1.27))))
			(property "Footprint" "{fp}" (at 0 0 0) (effects (font (size 1.27 1.27)) (hide yes)))
			(property "Datasheet" "" (at 0 0 0) (effects (font (size 1.27 1.27)) (hide yes)))
			(property "Description" "{desc}" (at 0 0 0) (effects (font (size 1.27 1.27)) (hide yes)))
"""


def rect(x0: float, y0: float, x1: float, y1: float) -> str:
    return f"""			(rectangle (start {x0:g} {y0:g}) (end {x1:g} {y1:g})
				(stroke (width 0.254) (type default)) (fill (type background)))
"""


def devkit_symbol(name: str, pitch: str) -> str:
    # 15 pins a side.  Left pins point right (angle 0) into the body from
    # x=-15.24; right pins point left (180) from x=+15.24.  y runs from
    # +17.78 down in 2.54 steps; symbol-library y is up.
    s = sym_header(name, "ESP32_DevKit_V1_30pin", "U",
                   f"ESP32 DevKit V1, 30 pin, as a socketed module, rows {pitch} mm apart",
                   f"{name}:ESP32_DevKit_V1_30pin")
    s += '			(symbol "ESP32_DevKit_V1_30pin_0_1"\n' + rect(-10.16, 20.32, 10.16, -20.32)
    s += """				(text "USB end" (at 0 -21.59 0) (effects (font (size 1 1))))
			)
			(symbol "ESP32_DevKit_V1_30pin_1_1"
"""
    for i, pn in enumerate(DEVKIT_LEFT):
        y = 17.78 - P * i
        kind = {"GND": "passive", "VIN": "power_in", "EN": "input",
                "VP": "input", "VN": "input"}.get(pn, "bidirectional")
        s += sym_pin(pn, i + 1, -15.24, y, 0, kind)
    for i, pn in enumerate(DEVKIT_RIGHT):
        y = 17.78 - P * i
        kind = {"GND": "passive", "3V3": "power_out",
                "TX0": "output", "RX0": "input"}.get(pn, "bidirectional")
        s += sym_pin(pn, i + 16, 15.24, y, 180, kind)
    return s + "			)\n		)\n"


def conn_symbol(name: str, sym: str, pins: list[str], desc: str, fp: str) -> str:
    n = len(pins)
    top = P * (n - 1) / 2
    s = sym_header(name, sym, "J", desc, fp)
    s += f'			(symbol "{sym}_0_1"\n' + rect(-2.54, top + 1.27, 7.62, -top - 1.27) + "			)\n"
    s += f'			(symbol "{sym}_1_1"\n'
    for i, pn in enumerate(pins):
        s += sym_pin(pn, i + 1, -7.62, top - P * i, 0)
    return s + "			)\n		)\n"


def cap_symbol(name: str) -> str:
    s = sym_header(name, "CP", "C", "Polarised capacitor",
                   "Capacitor_THT:CP_Radial_D5.0mm_P2.00mm")
    s += """			(symbol "CP_0_1"
				(rectangle (start -2.286 0.508) (end 2.286 1.016)
					(stroke (width 0) (type default)) (fill (type outline)))
				(rectangle (start -2.286 -1.016) (end 2.286 -0.508)
					(stroke (width 0) (type default)) (fill (type none)))
				(polyline (pts (xy -1.524 2.286) (xy -0.508 2.286)) (stroke (width 0) (type default)) (fill (type none)))
				(polyline (pts (xy -1.016 2.794) (xy -1.016 1.778)) (stroke (width 0) (type default)) (fill (type none)))
			)
			(symbol "CP_1_1"
"""
    s += sym_pin("+", 1, 0, 3.81, 270, "passive", 2.794)
    s += sym_pin("-", 2, 0, -3.81, 90, "passive", 2.794)
    return s + "			)\n		)\n"


def build_schematic(pitch: str, out: str, name: str) -> None:
    symbols = (devkit_symbol(name, pitch)
               + conn_symbol(name, "OLED_1.5in_7pin", OLED_PINS,
                             "Waveshare 1.5inch OLED / RGB OLED module header",
                             "Connector_PinHeader_2.54mm:PinHeader_1x07_P2.54mm_Vertical")
               + conn_symbol(name, "DS3231_module", RTC_PINS,
                             "DS3231 RTC module (ZS-042 pin order)",
                             "Connector_PinHeader_2.54mm:PinHeader_1x06_P2.54mm_Vertical")
               + cap_symbol(name))

    items = ""

    def instance(sym: str, ref: str, value: str, x: float, y: float,
                 fp: str, pin_count: int, half_height: float, beside: bool = False,
                 rot: int = 0) -> None:
        nonlocal items
        pins = "".join(f'		(pin "{i + 1}" (uuid "{uid()}"))\n' for i in range(pin_count))
        if beside:  # small parts: reference and value to the right
            rx, ry, vx, vy, just = x + 2.5, y - 1.3, x + 2.5, y + 1.3, " (justify left)"
        else:
            rx, ry, vx, vy, just = x, y - half_height - 2.0, x, y + half_height + 2.0, ""
        items += f"""	(symbol
		(lib_id "{name}:{sym}")
		(at {x:g} {y:g} {rot})
		(unit 1) (body_style 1) (exclude_from_sim no) (in_bom yes) (on_board yes) (dnp no)
		(uuid "{uid()}")
		(property "Reference" "{ref}" (at {rx:g} {ry:g} 0) (effects (font (size 1.27 1.27)){just}))
		(property "Value" "{value}" (at {vx:g} {vy:g} 0) (effects (font (size 1.27 1.27)){just}))
		(property "Footprint" "{fp}" (at {x:g} {y:g} 0) (effects (font (size 1.27 1.27)) (hide yes)))
		(property "Datasheet" "" (at {x:g} {y:g} 0) (effects (font (size 1.27 1.27)) (hide yes)))
		(property "Description" "" (at {x:g} {y:g} 0) (effects (font (size 1.27 1.27)) (hide yes)))
{pins}		(instances (project "{name}" (path "/{ROOT_UUID}" (reference "{ref}") (unit 1))))
	)
"""

    def glabel(label: str, x: float, y: float, angle: int) -> None:
        nonlocal items
        # KiCad pairs the angle with a justification: the label body extends
        # away from its anchor -- rightwards or upwards for 0 and 90.
        just = "left" if angle in (0, 90) else "right"
        items += f"""	(global_label "{label}" (shape passive) (at {x:g} {y:g} {angle}) (fields_autoplaced yes)
		(effects (font (size 1.27 1.27)) (justify {just}))
		(uuid "{uid()}")
		(property "Intersheetrefs" "${{INTERSHEET_REFS}}" (at {x:g} {y:g} 0)
			(effects (font (size 1.27 1.27)) (justify {just}) (hide yes)))
	)
"""

    def noconn(x: float, y: float) -> None:
        nonlocal items
        items += f'	(no_connect (at {x:g} {y:g}) (uuid "{uid()}"))\n'

    def text(s: str, x: float, y: float, size: float = 1.5) -> None:
        nonlocal items
        s = s.replace('"', '\\"')
        items += f"""	(text "{s}" (exclude_from_sim no) (at {x:g} {y:g} 0)
		(effects (font (size {size:g} {size:g})) (justify left))
		(uuid "{uid()}")
	)
"""

    # U1 in the middle.  Schematic y is down, so a symbol pin at library
    # (px, py) lands at (X + px, Y - py).
    ux, uy = 148.59, 100.33
    instance("ESP32_DevKit_V1_30pin", "U1", VALUES["U1"], ux, uy,
             f"{name}:ESP32_DevKit_V1_30pin", 30, 22.0)
    for i, pn in enumerate(DEVKIT_LEFT):
        x, y = ux - 15.24, uy - (17.78 - P * i)
        net = net_of("U1", i + 1)
        if net:
            glabel(net, x, y, 180)
        else:
            noconn(x, y)
    for i, pn in enumerate(DEVKIT_RIGHT):
        x, y = ux + 15.24, uy - (17.78 - P * i)
        net = net_of("U1", i + 16)
        if net:
            glabel(net, x, y, 0)
        else:
            noconn(x, y)

    def connector(sym: str, ref: str, value: str, x: float, y: float,
                  pins: list[str], fp: str) -> None:
        top = P * (len(pins) - 1) / 2
        instance(sym, ref, value, x, y, fp, len(pins), top + 1.27)
        for i in range(len(pins)):
            px, py = x - 7.62, y - (top - P * i)
            net = net_of(ref, i + 1)
            if net:
                glabel(net, px, py, 180)
            else:
                noconn(px, py)

    hdr = "Connector_PinHeader_2.54mm:PinHeader_1x0%d_P2.54mm_Vertical"
    connector("OLED_1.5in_7pin", "J3", VALUES["J3"], 60.96, 63.5, OLED_PINS, hdr % 7)
    connector("OLED_1.5in_7pin", "J4", VALUES["J4"], 60.96, 106.68, OLED_PINS, hdr % 7)
    connector("DS3231_module", "J5", VALUES["J5"], 60.96, 148.59, RTC_PINS, hdr % 6)

    cx, cy = 228.6, 100.33
    instance("CP", "C1", VALUES["C1"], cx, cy, "Capacitor_THT:CP_Radial_D5.0mm_P2.00mm",
             2, 1.5, beside=True)
    glabel("3V3", cx, cy - 3.81, 90)
    glabel("GND", cx, cy + 3.81, 270)

    text("Carrier board for the creeper-eyes Frankenstein head.", 20.32, 20.32, 2.0)
    text("Two Waveshare 1.5\" OLED panels on one SPI bus, chip selects D15 (Frank's right) and D4 (his left);", 20.32, 25.4)
    text("an optional DS3231 on the default I2C pins.  Wiring per docs/WIRING.md and docs/WIRING_RTC.md.", 20.32, 29.21)
    text("Both panels and the RTC are powered from the DevKit's 3V3 pin, never VIN.", 20.32, 33.02)
    text("Frank's right eye is the one on YOUR LEFT when you face him.", 20.32, 36.83)
    text(f"This variant: DevKit header rows {pitch} mm apart ({VARIANTS[pitch]}).", 20.32, 42.0)

    sch = f"""(kicad_sch
	(version {SCH_VERSION})
	(generator "eeschema")
	(generator_version "9.0")
	(uuid "{ROOT_UUID}")
	(paper "A4")
	(title_block
		(title "creeper-eyes carrier, rows {pitch} mm")
		(rev "A")
		(comment 1 "Generated by hardware/gen_board.py -- edit that, not this")
	)
	(lib_symbols
{symbols}	)
{items}	(sheet_instances
		(path "/" (page "1"))
	)
	(embedded_fonts no)
)
"""
    with open(os.path.join(out, name + ".kicad_sch"), "w", encoding="utf-8", newline="\n") as f:
        f.write(sch)

    # The same symbols as a library, so the schematic editor has somewhere to
    # find them when the user edits.  One less indent level than lib_symbols.
    lib_body = "\n".join(l[1:] if l.startswith("\t") else l
                         for l in symbols.replace(f'(symbol "{name}:', '(symbol "').split("\n"))
    lib = f"""(kicad_symbol_lib
	(version 20241209)
	(generator "kicad_symbol_editor")
	(generator_version "9.0")
{lib_body})
"""
    with open(os.path.join(out, name + ".kicad_sym"), "w", encoding="utf-8", newline="\n") as f:
        f.write(lib)


def build_project(out: str, name: str) -> None:
    pro = f"""{{
  "board": {{
    "design_settings": {{
      "defaults": {{}},
      "rules": {{
        "min_clearance": 0.2,
        "min_copper_edge_clearance": 0.3,
        "min_hole_clearance": 0.25,
        "min_through_hole_diameter": 0.3,
        "min_track_width": 0.2,
        "min_via_diameter": 0.6,
        "solder_mask_min_width": 0.1
      }},
      "track_widths": [0.0, 0.25, 0.5],
      "via_dimensions": [{{"diameter": 0.0, "drill": 0.0}}, {{"diameter": 0.8, "drill": 0.4}}]
    }}
  }},
  "meta": {{"filename": "{name}.kicad_pro", "version": 1}},
  "net_settings": {{
    "classes": [
      {{
        "bus_width": 12, "clearance": 0.2, "diff_pair_gap": 0.25, "diff_pair_via_gap": 0.25,
        "diff_pair_width": 0.2, "line_style": 0, "microvia_diameter": 0.3, "microvia_drill": 0.1,
        "name": "Default", "pcb_color": "rgba(0, 0, 0, 0.000)", "schematic_color": "rgba(0, 0, 0, 0.000)",
        "track_width": {TRACK}, "via_diameter": {VIA_D}, "via_drill": {VIA_DRILL}, "wire_width": 6
      }}
    ],
    "meta": {{"version": 4}}
  }},
  "pcbnew": {{"page_layout_descr_file": ""}},
  "schematic": {{"legacy_lib_dir": "", "legacy_lib_list": []}},
  "sheets": [["{ROOT_UUID}", "Root"]],
  "text_variables": {{}}
}}
"""
    with open(os.path.join(out, name + ".kicad_pro"), "w", encoding="utf-8", newline="\n") as f:
        f.write(pro)
    with open(os.path.join(out, "fp-lib-table"), "w", encoding="utf-8", newline="\n") as f:
        f.write(f'(fp_lib_table\n  (version 7)\n  (lib (name "{name}")(type "KiCad")'
                f'(uri "${{KIPRJMOD}}/{name}.pretty")(options "")(descr ""))\n)\n')
    with open(os.path.join(out, "sym-lib-table"), "w", encoding="utf-8", newline="\n") as f:
        f.write(f'(sym_lib_table\n  (version 7)\n  (lib (name "{name}")(type "KiCad")'
                f'(uri "${{KIPRJMOD}}/{name}.kicad_sym")(options "")(descr ""))\n)\n')


def find_kicad_share() -> str:
    exe = sys.executable
    # .../KiCad/10.0/bin/python.exe -> .../KiCad/10.0/share/kicad
    root = os.path.dirname(os.path.dirname(exe))
    share = os.path.join(root, "share", "kicad")
    if not os.path.isdir(os.path.join(share, "footprints")):
        raise SystemExit("run this with KiCad's own python.exe; "
                         f"no footprint library under {share}")
    return share


def main() -> None:
    share = find_kicad_share()
    wanted = sys.argv[1:] or list(VARIANTS)
    for pitch in wanted:
        if pitch not in VARIANTS:
            raise SystemExit(f"no variant for {pitch}; add it to VARIANTS")
        out, name = variant_dir(pitch), variant_name(pitch)
        os.makedirs(out, exist_ok=True)
        build_project(out, name)
        build_schematic(pitch, out, name)
        build_board(share, pitch, out, name)
        print(f"wrote {os.path.relpath(out, HERE)}/{name}.kicad_pro / .kicad_sch / .kicad_pcb")


if __name__ == "__main__":
    main()
