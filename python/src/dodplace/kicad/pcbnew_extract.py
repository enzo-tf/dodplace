"""Headless board extractor built on KiCad's own ``pcbnew`` parser.

Runs under KiCad's bundled Python (3.9 on macOS) and uses **only** the standard
library plus ``pcbnew``, so it never needs our project's dependencies. Emits a
JSON document (the "extract" schema) that ``dodplace.kicad.to_scene`` turns into
the IR, or that the IPC backend produces identically.

Usage:
    pcbnew_extract.py <board.kicad_pcb> <out.json>

Why this backend exists: it is KiCad's own parser, so it cannot drift with the
file format, and it needs no running GUI.
"""

import json
import math
import sys

import pcbnew

SCHEMA = "dodplace.extract/1"

# Pad attributes (pcbnew.PAD_ATTRIB_*).
_ATTRIB_NAMES = {
    pcbnew.PAD_ATTRIB_PTH: "pth",
    pcbnew.PAD_ATTRIB_SMD: "smd",
    pcbnew.PAD_ATTRIB_CONN: "connector",
    pcbnew.PAD_ATTRIB_NPTH: "npth",
}


def mm(value):
    """Internal units (nanometres) to millimetres."""
    return value / 1e6


def text(value):
    """KiCad returns wxString objects; the IR wants plain str."""
    return "" if value is None else str(value)


def to_local(px, py, ox, oy, degrees):
    """Board coordinates -> footprint-local, unrotated.

    The engine's convention is ``board = origin + R(theta) * local`` with the
    standard rotation matrix applied in KiCad's y-down frame; this is the
    inverse. Verified below for every pad before the JSON is written.
    """
    theta = math.radians(degrees)
    c, s = math.cos(-theta), math.sin(-theta)
    dx, dy = px - ox, py - oy
    return (c * dx - s * dy, s * dx + c * dy)


def from_local(lx, ly, ox, oy, degrees):
    theta = math.radians(degrees)
    c, s = math.cos(theta), math.sin(theta)
    return (ox + c * lx - s * ly, oy + s * lx + c * ly)


def poly_to_cycles(poly):
    """SHAPE_POLY_SET -> list of closed vertex lists, in mm."""
    cycles = []
    for i in range(poly.OutlineCount()):
        chain = poly.Outline(i)
        points = [(mm(chain.CPoint(j).x), mm(chain.CPoint(j).y)) for j in range(chain.PointCount())]
        if len(points) >= 3:
            cycles.append(points)
    return cycles


def layer_mask(boards_layerset):
    mask = 0
    for layer in boards_layerset.Seq():
        if 0 <= layer <= 31:
            mask |= 1 << layer
    return mask


def extract_footprint(fp, errors):
    origin = fp.GetPosition()
    ox, oy = mm(origin.x), mm(origin.y)
    degrees = fp.GetOrientationDegrees()

    cycles = []
    for layer in (pcbnew.F_CrtYd, pcbnew.B_CrtYd):
        courtyard = fp.GetCourtyard(layer)
        if courtyard.OutlineCount() > 0:
            for cycle in poly_to_cycles(courtyard):
                cycles.append([to_local(x, y, ox, oy, degrees) for x, y in cycle])
            break

    pads = []
    for pad in fp.Pads():
        pos = pad.GetPosition()
        px, py = mm(pos.x), mm(pos.y)
        lx, ly = to_local(px, py, ox, oy, degrees)

        # Self-check: the local offsets must rebuild the absolute position.
        bx, by = from_local(lx, ly, ox, oy, degrees)
        if abs(bx - px) > 1e-4 or abs(by - py) > 1e-4:
            errors.append(
                "%s pad %s: rotation frame mismatch (%.6f, %.6f) vs (%.6f, %.6f)"
                % (text(fp.GetReference()), text(pad.GetNumber()), bx, by, px, py)
            )

        size = pad.GetSize()
        net = text(pad.GetNetname())
        pads.append(
            {
                "number": text(pad.GetNumber()),
                "net": net if net else None,
                "x": lx,
                "y": ly,
                "size_x": mm(size.x),
                "size_y": mm(size.y),
                "rot_deg": pad.GetOrientationDegrees(),
                "attrib": _ATTRIB_NAMES.get(pad.GetAttribute(), "other"),
                "drill": mm(pad.GetDrillSize().x),
            }
        )

    fpid = fp.GetFPID()

    # Custom footprint fields: this is where supplier part numbers live
    # (``LCSC``, ``MPN``, ``JLCPCB``...), and they are the join key the
    # enrichment layer matches parts files against.
    fields = {}
    try:
        for field in fp.GetFields():
            name = text(field.GetName())
            value = text(field.GetText())
            if not value or name in ("Reference", "Value"):
                continue
            fields[name] = value
    except Exception as exc:  # noqa: BLE001
        errors.append("%s: fields unavailable: %s" % (text(fp.GetReference()), exc))

    # 3D model references. The ${...} variables are left raw: pcbnew only
    # expands them with a fully loaded project, so the enrichment layer
    # resolves them (KIPRJMOD, KICAD*_3DMODEL_DIR) when it reads the file.
    models = []
    try:
        for model in fp.Models():
            filename = text(model.m_Filename)
            if filename:
                models.append(filename)
    except Exception as exc:  # noqa: BLE001
        errors.append("%s: 3D models unavailable: %s" % (text(fp.GetReference()), exc))

    return {
        "ref": text(fp.GetReference()),
        "value": text(fp.GetValue()),
        "fpid": "%s:%s" % (text(fpid.GetLibNickname()), text(fpid.GetLibItemName())),
        "x": ox,
        "y": oy,
        "rot_deg": degrees,
        "side": "bottom" if fp.GetLayer() == pcbnew.B_Cu else "top",
        "locked": bool(fp.IsLocked()),
        "courtyard": [[list(point) for point in cycle] for cycle in cycles],
        "pads": pads,
        "fields": fields,
        "models": models,
    }


def extract(board_path):
    errors = []
    board = pcbnew.LoadBoard(board_path)
    if board is None:
        raise RuntimeError("pcbnew could not load %s" % board_path)

    outline_poly = pcbnew.SHAPE_POLY_SET()
    try:
        board.GetBoardPolygonOutlines(outline_poly, True)
    except Exception as exc:  # noqa: BLE001 - an outline-less board is legal
        errors.append("board outline unavailable: %s" % exc)
    outlines = poly_to_cycles(outline_poly)
    outline = outlines[0] if outlines else []

    keepouts = []
    for zone in board.Zones():
        if not zone.GetIsRuleArea():
            continue
        cycles = poly_to_cycles(zone.Outline())
        if not cycles:
            continue
        keepouts.append(
            {
                "name": text(zone.GetZoneName()),
                "layer_mask": layer_mask(zone.GetLayerSet()),
                "xy": [[list(point) for point in cycle] for cycle in cycles],
            }
        )

    netclasses = []
    settings = board.GetDesignSettings()
    try:
        classes = settings.m_NetSettings.GetNetclasses()
        for name in classes:
            netclass = classes[name]
            netclasses.append(
                {
                    "name": text(name),
                    "clearance_mm": mm(netclass.GetClearance()),
                    "track_width_mm": mm(netclass.GetTrackWidth()),
                }
            )
    except Exception as exc:  # noqa: BLE001
        errors.append("netclasses unavailable: %s" % exc)
    if not netclasses:
        default = settings.m_NetSettings.GetDefaultNetclass()
        netclasses.append(
            {
                "name": "Default",
                "clearance_mm": mm(default.GetClearance()),
                "track_width_mm": mm(default.GetTrackWidth()),
            }
        )

    footprints = [extract_footprint(fp, errors) for fp in board.GetFootprints()]

    # Per-net effective netclass: KiCad resolves its own pattern/label
    # assignments, which is exactly the "clearance matrix" input of the IR.
    nets = []
    try:
        for name in sorted({pad["net"] for fp in footprints for pad in fp["pads"] if pad["net"]}):
            netclass = settings.m_NetSettings.GetEffectiveNetClass(name)
            nets.append({"name": text(name), "netclass": text(netclass.GetName())})
    except Exception as exc:  # noqa: BLE001
        errors.append("per-net netclass resolution failed: %s" % exc)

    return {
        "schema": SCHEMA,
        "backend": "pcbnew",
        "backend_version": text(pcbnew.GetBuildVersion()),
        "board": {
            "path": board_path,
            "copper_layers": settings.GetCopperLayerCount(),
            "outline": [[list(point) for point in outline]],
        },
        "keepouts": keepouts,
        "netclasses": netclasses,
        "nets": nets,
        "components": footprints,
        "warnings": errors,
    }


def main(argv):
    if len(argv) != 3:
        sys.stderr.write("usage: pcbnew_extract.py <board.kicad_pcb> <out.json>\n")
        return 2
    try:
        payload = extract(argv[1])
    except Exception as exc:  # noqa: BLE001 - report cleanly to the caller
        sys.stderr.write("extraction failed: %s\n" % exc)
        return 1
    with open(argv[2], "w") as handle:
        json.dump(payload, handle, indent=1, sort_keys=True)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
