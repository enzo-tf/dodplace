"""Headless placement applier built on KiCad's own ``pcbnew`` writer.

Runs under KiCad's bundled Python and uses only the standard library plus
``pcbnew``, so it never needs our project's dependencies. It reads a JSON list
of updates (reference, absolute position in mm, absolute rotation in degrees),
moves those footprints, and saves a **copy** of the board: the input file is
opened read-only and never written back.

Usage:
    pcbnew_apply.py <board_in.kicad_pcb> <updates.json> <board_out.kicad_pcb>

Why the reference and not the index: a reference is what the file carries and
what a human reads in the report, and it survives the two sides disagreeing
about ordering. A reference the board does not have is an error, not a silent
skip - it means the placement and the board are not the same design.
"""

import json
import math
import sys

import pcbnew

SCHEMA = "dodplace.apply/1"


def mm(value):
    """Internal units (nanometres) to millimetres."""
    return value / 1e6


def nm(value):
    """Millimetres to internal units."""
    return int(round(value * 1e6))


def apply_updates(board, updates):
    """Move the named footprints. Returns (applied, errors)."""
    by_ref = {}
    for fp in board.GetFootprints():
        by_ref[str(fp.GetReference())] = fp

    applied = 0
    errors = []
    for item in updates:
        ref = str(item["ref"])
        fp = by_ref.get(ref)
        if fp is None:
            errors.append("%s: no such footprint on the board" % ref)
            continue

        x, y, rot = float(item["x"]), float(item["y"]), float(item["rot_deg"])
        fp.SetPosition(pcbnew.VECTOR2I(nm(x), nm(y)))
        fp.SetOrientationDegrees(rot)

        # Self-check, like the extractor: read the pose back and confirm the
        # writer took it. A silent failure here would ship a wrong board.
        pos = fp.GetPosition()
        got_x, got_y = mm(pos.x), mm(pos.y)
        got_rot = fp.GetOrientationDegrees()
        if abs(got_x - x) > 1e-4 or abs(got_y - y) > 1e-4:
            errors.append("%s: position did not take (asked %.4f,%.4f got %.4f,%.4f)"
                          % (ref, x, y, got_x, got_y))
            continue
        delta = abs((got_rot - rot + 180.0) % 360.0 - 180.0)
        if delta > 1e-3:
            errors.append("%s: rotation did not take (asked %.3f got %.3f)"
                          % (ref, rot, got_rot))
            continue
        applied += 1
    return applied, errors


def main(argv):
    if len(argv) != 4:
        sys.stderr.write(__doc__)
        return 2
    board_in, updates_path, board_out = argv[1], argv[2], argv[3]

    with open(updates_path, "r", encoding="utf-8") as handle:
        doc = json.load(handle)
    if doc.get("schema") != SCHEMA:
        sys.stderr.write("error: updates file is not %s\n" % SCHEMA)
        return 2

    board = pcbnew.LoadBoard(board_in)
    if board is None:
        sys.stderr.write("error: pcbnew could not load %s\n" % board_in)
        return 1

    applied, errors = apply_updates(board, doc.get("updates", []))
    if errors:
        for line in errors[:20]:
            sys.stderr.write("error: %s\n" % line)
        return 1

    pcbnew.SaveBoard(board_out, board)
    sys.stdout.write(json.dumps({"applied": applied}) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
