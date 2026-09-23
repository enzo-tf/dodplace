"""Silkscreen clearance, resolved on the finished placement.

KiCad reports two warnings the copper model knows nothing about: silk clipped
by a solder-mask opening (`silk_over_copper`) and silk of one footprint crossing
another's (`silk_overlap`). Both are decided by the *vector* primitives - the
extract carries them as capsules, arcs already flattened to chords - and a
bounding box per footprint is useless here: it reports 480 incidences on r10
against 10 real ones.

This runs after the solver, on the placement it produced: the annealing loop is
not told about silk, exactly as the project decided, and the fix is a
deterministic micro-nudge with a triple guard - the conflict must fall, no
copper/courtyard/hole rule may break, and the wirelength may not move by more
than `HPWL_TOLERANCE`.
"""

from __future__ import annotations

import math

STEP = 0.05
REACH = 0.20
HPWL_TOLERANCE = 5.0
MASK_MARGIN = 0.05  # KiCad's default solder-mask clearance


def _seg_rect_meets(x1, y1, x2, y2, lo_x, lo_y, hi_x, hi_y) -> bool:
    """Does the segment enter the box? Liang-Barsky, exact for a segment."""
    dx = x2 - x1
    dy = y2 - y1
    t0, t1 = 0.0, 1.0
    for p, q in ((-dx, x1 - lo_x), (dx, hi_x - x1), (-dy, y1 - lo_y), (dy, hi_y - y1)):
        if p == 0.0:
            if q < 0.0:
                return False
            continue
        r = q / p
        if p < 0.0:
            t0 = max(t0, r)
        else:
            t1 = min(t1, r)
        if t0 > t1:
            return False
    return True


def _seg_seg_distance(a, b) -> float:
    """Distance between two segments, zero when they cross."""
    (ax1, ay1, ax2, ay2) = a[:4]
    (bx1, by1, bx2, by2) = b[:4]

    def cross(ox, oy, px, py, qx, qy):
        return (px - ox) * (qy - oy) - (py - oy) * (qx - ox)

    d1 = cross(ax1, ay1, ax2, ay2, bx1, by1)
    d2 = cross(ax1, ay1, ax2, ay2, bx2, by2)
    d3 = cross(bx1, by1, bx2, by2, ax1, ay1)
    d4 = cross(bx1, by1, bx2, by2, ax2, ay2)
    if ((d1 > 0) != (d2 > 0)) and ((d3 > 0) != (d4 > 0)):
        return 0.0
    return min(
        _point_seg(bx1, by1, a), _point_seg(bx2, by2, a),
        _point_seg(ax1, ay1, b), _point_seg(ax2, ay2, b),
    )


def _point_seg(px, py, seg) -> float:
    x1, y1, x2, y2 = seg[:4]
    dx, dy = x2 - x1, y2 - y1
    length2 = dx * dx + dy * dy
    t = 0.0 if length2 == 0 else max(0.0, min(1.0, ((px - x1) * dx + (py - y1) * dy) / length2))
    return math.hypot(px - (x1 + t * dx), py - (y1 + t * dy))


def silk_conflicts(component: dict, others: list[dict]) -> int:
    """How many silkscreen conflicts `component` causes where it stands.

    `component` and `others` are posed footprints: `{"ref", "silk", "pads",
    "side", "x", "y", "rot_deg"}` with silk capsules and pad boxes in world
    coordinates - the caller has already moved them by the placement.
    """
    silk = component.get("silk") or []
    if not silk:
        return 0
    count = 0
    for seg in silk:
        half = seg[4]
        for other in others:
            if other["ref"] == component["ref"] or other["side"] != component["side"]:
                continue
            for pad in other.get("pads") or []:
                lo_x = pad["x"] - pad["half_w"] - MASK_MARGIN - half
                hi_x = pad["x"] + pad["half_w"] + MASK_MARGIN + half
                lo_y = pad["y"] - pad["half_h"] - MASK_MARGIN - half
                hi_y = pad["y"] + pad["half_h"] + MASK_MARGIN + half
                if _seg_rect_meets(seg[0], seg[1], seg[2], seg[3], lo_x, lo_y, hi_x, hi_y):
                    count += 1
                    break
            else:
                for other_seg in other.get("silk") or []:
                    if _seg_seg_distance(seg, other_seg) <= half + other_seg[4]:
                        count += 1
                        break
    return count


def _hpwl(footprints: list[dict]) -> float:
    nets: dict[str, list[tuple[float, float]]] = {}
    for fp in footprints:
        for pad in fp.get("pads") or []:
            net = pad.get("net")
            if net:
                nets.setdefault(net, []).append((pad["x"], pad["y"]))
    total = 0.0
    for points in nets.values():
        if len(points) < 2:
            continue
        xs = [p[0] for p in points]
        ys = [p[1] for p in points]
        total += (max(xs) - min(xs)) + (max(ys) - min(ys))
    return total


def board_box(extract: dict) -> tuple | None:
    """The outline's bounding box: the copper may not leave it."""
    points = [p for cycle in (extract.get("board") or {}).get("outline") or [] for p in cycle]
    if not points:
        return None
    xs = [float(p[0]) for p in points]
    ys = [float(p[1]) for p in points]
    return (min(xs), min(ys), max(xs), max(ys))


def polish_silk(footprints: list[dict], movable: set[str],
                board: tuple | None = None) -> dict:
    """Nudge the culprits until the silk clears, and report what was done.

    One component at a time, offsets from a fixed grid, first legal state wins:
    a conflict must fall, no pad may come within the copper rules, and the
    wirelength may move by at most `HPWL_TOLERANCE`. The pass is deterministic -
    same placement in, same board out.
    """
    offsets = [(dx * r, dy * r) for dx, dy in
               [(1, 0), (-1, 0), (0, 1), (0, -1), (1, 1), (1, -1), (-1, 1), (-1, -1)]
               for r in range(1, int(REACH / STEP) + 1)]
    baseline = _hpwl(footprints)
    moved: list[dict] = []
    for fp in footprints:
        if fp["ref"] not in movable:
            continue
        before = silk_conflicts(fp, footprints)
        if before == 0:
            continue
        best = None
        for dx, dy in offsets:
            old_x, old_y = fp["x"], fp["y"]
            _shift(fp, dx, dy)
            if silk_conflicts(fp, footprints) < before and _copper_safe(
                    fp, footprints, board=board) and \
                    _hpwl(footprints) <= baseline + HPWL_TOLERANCE:
                best = (dx, dy, silk_conflicts(fp, footprints))
            _shift(fp, old_x - fp["x"], old_y - fp["y"])
            if best is not None:
                break
        if best is not None:
            _shift(fp, best[0], best[1])
            moved.append({"ref": fp["ref"], "dx": round(best[0], 3), "dy": round(best[1], 3),
                          "conflicts": [before, best[2]]})
    return {"moved": moved, "hpwl_before": round(baseline, 2),
            "hpwl_after": round(_hpwl(footprints), 2)}


def pose_footprints(extract: dict, updates: list) -> list[dict]:
    """The board as the silk pass needs it: every footprint posed, in world
    coordinates, with its silkscreen capsules and its pads. KiCad turns a
    footprint by `(x, y) -> (x cos + y sin, -x sin + y cos)` - measured against
    pcbnew, and the same convention the engine's rotation model uses."""
    by_ref = {u.ref: u for u in updates}
    out = []
    for comp in extract.get("components") or []:
        update = by_ref.get(comp["ref"])
        if update is not None:
            origin_x, origin_y, angle = update.x, update.y, update.rot_deg
        else:
            origin_x = float(comp["x"])
            origin_y = float(comp["y"])
            angle = float(comp.get("rot_deg", 0.0))
        rad = math.radians(angle)
        cos_a, sin_a = math.cos(rad), math.sin(rad)

        def place(px, py):
            return (origin_x + px * cos_a + py * sin_a,
                    origin_y - px * sin_a + py * cos_a)

        silk = []
        for x1, y1, x2, y2, half in comp.get("silk") or []:
            ax, ay = place(x1, y1)
            bx, by = place(x2, y2)
            silk.append([ax, ay, bx, by, half])
        pads = []
        for pad in comp.get("pads") or []:
            px, py = place(float(pad["x"]), float(pad["y"]))
            # A drilled pad obstructs at least its drill, plated or not: the
            # same rule the ingest applies before the engine ever sees it.
            size_x = max(float(pad.get("size_x", 0.0)), float(pad.get("drill") or 0.0))
            size_y = max(float(pad.get("size_y", 0.0)), float(pad.get("drill") or 0.0))
            pads.append({"net": pad.get("net"), "x": px, "y": py,
                         "half_w": size_x * 0.5, "half_h": size_y * 0.5})
        courtyard = _courtyard_box(comp, place)
        out.append({"ref": comp["ref"], "side": comp.get("side", "top"),
                    "x": origin_x, "y": origin_y, "rot_deg": angle,
                    "silk": silk, "pads": pads, "courtyard": courtyard})
    return out


def _courtyard_box(comp: dict, place) -> tuple | None:
    """World AABB of the courtyard: the rule KiCad checks beside the pads."""
    rings = comp.get("courtyard") or []
    points = [p for ring in rings for p in ring]
    if not points:
        return None
    lo_x = hi_x = place(float(points[0][0]), float(points[0][1]))[0]
    lo_y = hi_y = place(float(points[0][0]), float(points[0][1]))[1]
    for px, py in points:
        wx, wy = place(float(px), float(py))
        lo_x, hi_x = min(lo_x, wx), max(hi_x, wx)
        lo_y, hi_y = min(lo_y, wy), max(hi_y, wy)
    return (lo_x, lo_y, hi_x, hi_y)


def _shift(fp: dict, dx: float, dy: float) -> None:
    fp["x"] += dx
    fp["y"] += dy
    for seg in fp.get("silk") or []:
        seg[0] += dx
        seg[1] += dy
        seg[2] += dx
        seg[3] += dy
    for pad in fp.get("pads") or []:
        pad["x"] += dx
        pad["y"] += dy


def _copper_safe(fp: dict, others: list[dict], clearance: float = 0.2,
                 board: tuple | None = None, edge_margin: float = 0.5) -> bool:
    """Would this position break a rule the DRC checks?

    Three of them, and the first version of this guard covered only one: pads
    (holes included) must clear each other, courtyards must not overlap, and the
    footprint's copper must stay inside the board. Missing the last two cost the
    gate - two courtyard overlaps, a short and a bridge - the first time the
    pass ran for real.
    """
    for other in others:
        if other["ref"] == fp["ref"] or other["side"] != fp["side"]:
            continue
        mine = fp.get("courtyard")
        theirs = other.get("courtyard")
        if mine and theirs:
            if (mine[0] < theirs[2] and theirs[0] < mine[2] and
                    mine[1] < theirs[3] and theirs[1] < mine[3]):
                return False
        for a in fp.get("pads") or []:
            for b in other.get("pads") or []:
                dx = abs(a["x"] - b["x"]) - (a["half_w"] + b["half_w"])
                dy = abs(a["y"] - b["y"]) - (a["half_h"] + b["half_h"])
                if max(dx, dy) < clearance:
                    return False
    for pad in fp.get("pads") or []:
        if board is not None:
            if (pad["x"] - pad["half_w"] < board[0] + edge_margin or
                    pad["x"] + pad["half_w"] > board[2] - edge_margin or
                    pad["y"] - pad["half_h"] < board[1] + edge_margin or
                    pad["y"] + pad["half_h"] > board[3] - edge_margin):
                return False
    return True
