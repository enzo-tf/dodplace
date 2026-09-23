"""Validation of the extract document produced by the KiCad backends.

The schema is deliberately small and flat so the IPC and headless backends can
both produce it and ``to_scene`` can consume it without branching.
"""

from __future__ import annotations

SCHEMA = "dodplace.extract/1"

REQUIRED_TOP = ("schema", "backend", "board", "components", "netclasses", "keepouts")


class ExtractError(ValueError):
    """The extract document is missing data or malformed."""


def validate(doc: dict) -> None:
    """Raise :class:`ExtractError` unless ``doc`` is a usable extract."""
    if not isinstance(doc, dict):
        raise ExtractError("extract must be a JSON object")
    for key in REQUIRED_TOP:
        if key not in doc:
            raise ExtractError(f"extract is missing '{key}'")
    if doc["schema"] != SCHEMA:
        raise ExtractError(f"unsupported extract schema {doc['schema']!r}")

    board = doc["board"]
    if not isinstance(board.get("copper_layers"), int) or board["copper_layers"] < 0:
        raise ExtractError("board.copper_layers must be a non-negative integer")
    outline = board.get("outline", [])
    if not isinstance(outline, list):
        raise ExtractError("board.outline must be a list of cycles")
    for cycle in outline:
        _check_cycle(cycle, "board.outline")

    for keepout in doc["keepouts"]:
        if not isinstance(keepout.get("layer_mask"), int):
            raise ExtractError("keepout.layer_mask must be an integer")
        for cycle in keepout.get("xy", []):
            _check_cycle(cycle, "keepout.xy")

    for netclass in doc["netclasses"]:
        if not isinstance(netclass.get("name"), str):
            raise ExtractError("netclass.name must be a string")
        for field in ("clearance_mm", "track_width_mm"):
            if not isinstance(netclass.get(field), (int, float)):
                raise ExtractError(f"netclass.{field} must be a number")

    for net in doc.get("nets", []):
        if not isinstance(net.get("name"), str) or not isinstance(net.get("netclass"), str):
            raise ExtractError("each net needs string 'name' and 'netclass'")

    refs: set[str] = set()
    for comp in doc["components"]:
        for field in ("ref", "fpid", "side"):
            if not isinstance(comp.get(field), str):
                raise ExtractError(f"component.{field} must be a string")
        for field in ("x", "y", "rot_deg"):
            if not isinstance(comp.get(field), (int, float)):
                raise ExtractError(f"component.{field} must be a number")
        if comp["side"] not in ("top", "bottom"):
            raise ExtractError(f"component {comp['ref']}: bad side {comp['side']!r}")
        if comp["ref"] in refs:
            raise ExtractError(f"duplicate reference {comp['ref']!r}")
        refs.add(comp["ref"])
        fields = comp.get("fields", {})
        if not isinstance(fields, dict) or not all(
            isinstance(key, str) and isinstance(value, str) for key, value in fields.items()
        ):
            raise ExtractError(f"component {comp['ref']}: fields must map strings to strings")
        models = comp.get("models", [])
        if not isinstance(models, list) or not all(isinstance(entry, str) for entry in models):
            raise ExtractError(f"component {comp['ref']}: models must be a list of strings")
        for cycle in comp.get("courtyard", []):
            _check_cycle(cycle, f"component {comp['ref']} courtyard")
        for pad in comp.get("pads", []):
            if not isinstance(pad.get("number"), str):
                raise ExtractError(f"component {comp['ref']}: pad.number must be a string")
            net = pad.get("net")
            if net is not None and not isinstance(net, str):
                raise ExtractError(f"component {comp['ref']}: pad.net must be a string or null")
            for field in ("x", "y", "size_x", "size_y"):
                if not isinstance(pad.get(field), (int, float)):
                    raise ExtractError(f"component {comp['ref']} pad {pad.get('number')}: "
                                       f"{field} must be a number")
        # Silkscreen, flattened to capsules in the footprint's own frame:
        # (x1, y1, x2, y2, half_width). Optional - a producer may leave it out,
        # and the engine then has no silk to check.
        for seg in comp.get("silk", []):
            if not isinstance(seg, list) or len(seg) != 5:
                raise ExtractError(
                    f"component {comp['ref']}: a silk segment is (x1, y1, x2, y2, half_width)"
                )
            if not all(isinstance(v, (int, float)) for v in seg):
                raise ExtractError(f"component {comp['ref']}: silk coordinates must be numbers")


def _check_cycle(cycle, where: str) -> None:
    if not isinstance(cycle, list) or len(cycle) < 3:
        raise ExtractError(f"{where}: a polygon needs at least 3 points")
    for point in cycle:
        if not isinstance(point, (list, tuple)) or len(point) != 2:
            raise ExtractError(f"{where}: each point must be [x, y]")


def net_names(doc: dict) -> list[str]:
    """All distinct pad net names, sorted for deterministic output."""
    names = set()
    for comp in doc["components"]:
        for pad in comp.get("pads", []):
            if pad.get("net"):
                names.add(pad["net"])
    return sorted(names)
