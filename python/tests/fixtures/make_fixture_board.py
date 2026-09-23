"""Generate the committed test board with pcbnew.

Run with KiCad's interpreter (not the project venv):

    PYTHONDONTWRITEBYTECODE=1 \
    /Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/3.9/bin/python3 \
        make_fixture_board.py scene_fixture.kicad_pcb

The board is deliberately small but covers every extractor feature: both sides,
three rotations, a locked part, a courtyard-less case, a keepout rule area, a
differential pair, and net names that exercise the electrical-role inference.
"""

import os
import sys

import pcbnew

LIBROOT = "/Applications/KiCad/KiCad.app/Contents/SharedSupport/footprints"

OUTLINE = ((0.0, 0.0), (50.0, 0.0), (50.0, 30.0), (0.0, 30.0))
KEEPOUT = ((35.0, 2.0), (48.0, 2.0), (48.0, 12.0), (35.0, 12.0))

#: (library, footprint, reference, x, y, rotation, bottom, locked, pad nets, fields)
PARTS = (
    ("Resistor_SMD.pretty", "R_0603_1608Metric", "R1", 10.0, 10.0, 0.0, False, False,
     {"1": "VCC", "2": "GND"},
     {"LCSC": "C25804", "MPN": "RC0603FR-0710KL", "Manufacturer": "Yageo"}),
    ("Resistor_SMD.pretty", "R_0603_1608Metric", "R2", 20.0, 10.0, 90.0, False, True,
     {"1": "USB1_P", "2": "GND"},
     {"LCSC": "C25804", "MPN": "RC0603FR-0710KL", "Manufacturer": "Yageo"}),
    ("Capacitor_SMD.pretty", "C_0603_1608Metric", "C1", 30.0, 10.0, 0.0, True, False,
     {"1": "VCC", "2": "GND"},
     {"LCSC": "C14663", "MPN": "CL10B104KB8NNNC"}),
    ("Package_SO.pretty", "SOIC-8_3.9x4.9mm_P1.27mm", "U1", 25.0, 21.0, 180.0, False, False,
     {"4": "GND", "8": "VCC", "1": "USB1_P", "2": "USB1_N", "3": "MOSI", "5": "MISO"},
     {"LCSC": "C7955", "MPN": "STM32F103C8T6"}),
)


def add_outline(board):
    points = list(OUTLINE) + [OUTLINE[0]]
    for (x1, y1), (x2, y2) in zip(points, points[1:]):
        shape = pcbnew.PCB_SHAPE(board)
        shape.SetShape(pcbnew.SHAPE_T_SEGMENT)
        shape.SetLayer(pcbnew.Edge_Cuts)
        shape.SetStart(pcbnew.VECTOR2I_MM(x1, y1))
        shape.SetEnd(pcbnew.VECTOR2I_MM(x2, y2))
        board.Add(shape)


def add_keepout(board):
    zone = pcbnew.ZONE(board)
    zone.SetIsRuleArea(True)
    zone.SetZoneName("antenna-keepout")
    zone.SetLayerSet(pcbnew.LSET.AllCuMask())
    outline = zone.Outline()
    outline.NewOutline()
    for x, y in KEEPOUT:
        outline.Append(pcbnew.VECTOR2I_MM(x, y))
    board.Add(zone)


def main(argv):
    if len(argv) != 2:
        sys.stderr.write("usage: make_fixture_board.py <out.kicad_pcb>\n")
        return 2

    board = pcbnew.BOARD()
    add_outline(board)
    add_keepout(board)

    nets = {}
    for _lib, _name, ref, x, y, rot, bottom, locked, pad_nets, fields in PARTS:
        footprint = pcbnew.FootprintLoad(f"{LIBROOT}/{_lib}", _name)
        if footprint is None:
            sys.stderr.write("cannot load %s/%s\n" % (_lib, _name))
            return 1
        board.Add(footprint)
        # FootprintLoad leaves the library nickname empty; a board placed from a
        # library carries it, and the enrichment layer matches on `fpid`.
        footprint.SetFPID(pcbnew.LIB_ID(_lib[: -len(".pretty")], _name))
        footprint.SetReference(ref)
        footprint.SetPosition(pcbnew.VECTOR2I_MM(x, y))
        footprint.SetOrientationDegrees(rot)
        if bottom:
            footprint.Flip(footprint.GetPosition(), False)
        footprint.SetLocked(locked)
        for field_name, field_value in fields.items():
            footprint.SetField(field_name, field_value)

        # A courtyard-less part: strip the courtyard graphics so the extractor
        # has to derive one from pad extents (DEGRADED_COURTYARD_BBOX). The part
        # is flipped to the bottom first, so both courtyard layers are checked.
        if ref == "C1":
            for item in list(footprint.GraphicalItems()):
                if item.GetLayer() in (pcbnew.F_CrtYd, pcbnew.B_CrtYd):
                    footprint.Remove(item)

        for pad in footprint.Pads():
            net_name = pad_nets.get(pad.GetNumber())
            if not net_name:
                continue
            if net_name not in nets:
                item = pcbnew.NETINFO_ITEM(board, net_name)
                board.Add(item)
                nets[net_name] = item
            pad.SetNet(nets[net_name])

    board.Save(argv[1])

    # Save() also drops a project and local-settings file; the fixture is meant
    # to be a single portable .kicad_pcb, and the extraction is identical
    # without them (custom netclasses would live in the project).
    for suffix in (".kicad_pro", ".kicad_prl"):
        sidecar = argv[1][: -len(".kicad_pcb")] + suffix
        if os.path.exists(sidecar):
            os.remove(sidecar)

    sys.stderr.write(
        "wrote %s: %d footprints, %d nets\n" % (argv[1], len(list(board.GetFootprints())), len(nets))
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
