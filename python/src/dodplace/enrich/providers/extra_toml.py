"""``extra.toml``: the hand-authored user rules.

This is the strongest source in the precedence chain and the only one that can
express the hard constraints no catalogue or datasheet carries: locked parts,
fixed orientations, symmetry, thermal envelopes, decoupling targets, keepouts
and board-level mechanical limits.

Example (also shipped as ``extra.example.toml`` at the repo root)::

    [board]
    allowed_sides = "both"        # both | top | bottom
    ceiling_height_mm = 25.0
    airflow = [1.0, 0.0]
    via_under_body_allowed = false
    courtyard_margin_mm = 0.25

    [board.net_clearance]         # net glob -> mm
    VCC = 0.4

    [board.net_class]             # net glob -> class name
    "GND*" = "Power"

    [board.class_clearance]       # class name -> mm (overrides the board's)
    Power = 0.4

    # A [[component]] block is selected by a reference glob, a footprint glob,
    # or both. Later blocks win field by field, so a specific rule placed after
    # a family rule overrides it.
    [[component]]
    ref = "U1"                    # or "FID*", "U1?", "D1[0-9]"
    # fpid = "Tuile_LED:*"        # footprint id glob
    height_mm = 1.2
    mass_g = 0.5
    orientation_deg = 90
    rot_fixed = true
    locked = true
    pin_roles = { "8" = "power", "4" = "ground" }
    pin_currents = { "8" = 0.5 }

    [[thermal]]
    component = "U1"
    power_w = 0.3
    radius_mm = 3.0

    [[decoupling]]
    ic = "U1"
    cap = "C1"
    pin = "8"
    max_distance_mm = 5.0

    [[symmetry]]
    a = "R1"
    b = "R2"
    axis = "x"

    [[diffpair]]
    p = "USB1_P"
    n = "USB1_N"
    max_skew_mm = 0.1

    [[keepout]]
    layer_mask = 3
    polygon = [[40, 2], [48, 2], [48, 8], [40, 8]]
"""

from __future__ import annotations

from dataclasses import dataclass, field
import fnmatch
import tomllib
from pathlib import Path

from ..overlay import (
    BoardEnrichment,
    ComponentEnrichment,
    DecouplingRow,
    DiffPairRow,
    EnrichmentOverlay,
    KeepoutRow,
    SymmetryRow,
    ThermalRow,
)
from ..model import PIN_ROLE_NAMES
from ...ir.spec import SIDE_BOTTOM, SIDE_BOTH, SIDE_TOP

SOURCE = "extra_toml"

_SIDES = {"both": SIDE_BOTH, "top": SIDE_TOP, "bottom": SIDE_BOTTOM}
_AXES = ("x", "y", "fixed")


class ExtraTomlError(ValueError):
    """The rules file is malformed."""


@dataclass
class ComponentRule:
    """One ``[[component]]`` block, with its selectors.

    ``ref`` and ``fpid`` are globs, so a single block can cover a family
    (``FID*``) and a later block can override one member of it.
    """

    entry: ComponentEnrichment
    ref_pattern: str | None = None
    fpid_pattern: str | None = None
    order: int = 0

    def matches(self, component: dict) -> bool:
        if self.ref_pattern and not fnmatch.fnmatchcase(str(component.get("ref", "")),
                                                        self.ref_pattern):
            return False
        if self.fpid_pattern and not fnmatch.fnmatchcase(str(component.get("fpid", "")),
                                                         self.fpid_pattern):
            return False
        return True

    def describe(self) -> str:
        parts = []
        if self.ref_pattern:
            parts.append(f"ref={self.ref_pattern}")
        if self.fpid_pattern:
            parts.append(f"fpid={self.fpid_pattern}")
        return " ".join(parts) or "?"


@dataclass
class ExtraConfig:
    """An ``extra.toml``: board rules, ordered component rules, and rows."""

    overlay: EnrichmentOverlay = field(default_factory=EnrichmentOverlay)
    component_rules: list[ComponentRule] = field(default_factory=list)
    source_ref: str = ""

    def materialize(self, components: list[dict]) -> None:
        """Resolve the ordered rules against the board's components.

        The overlay handed to the rest of the pipeline only ever holds concrete,
        per-reference entries; globs exist in the file, not downstream.
        """
        for component in components:
            for rule in self.component_rules:
                if rule.matches(component):
                    entry = self.overlay.component(str(component["ref"]))
                    _merge_component(entry, rule.entry)


def _number(value, field_name: str, *, positive: bool = True) -> float:
    if not isinstance(value, (int, float)) or isinstance(value, bool):
        raise ExtraTomlError(f"{field_name} must be a number")
    number = float(value)
    if positive and number <= 0.0:
        raise ExtraTomlError(f"{field_name} must be positive")
    return number


def _pin_numbers(mapping, field_name: str) -> dict[str, object]:
    if not isinstance(mapping, dict):
        raise ExtraTomlError(f"{field_name} must be a table of pad -> value")
    return {str(pin): value for pin, value in mapping.items()}


def _parse_roles(mapping, ref: str) -> dict[str, int]:
    roles = {}
    for pin, role in _pin_numbers(mapping, f"{ref}.pin_roles").items():
        if not isinstance(role, str) or role.lower() not in PIN_ROLE_NAMES:
            raise ExtraTomlError(
                f"{ref}: pin {pin} has unknown role {role!r}; "
                f"expected one of {', '.join(sorted(PIN_ROLE_NAMES))}"
            )
        flag_name = PIN_ROLE_NAMES[role.lower()]
        roles[pin] = 0 if flag_name == "0" else _role_flag(flag_name)
    return roles


def _role_flag(name: str) -> int:
    from ...ir.spec import PIN_CLOCK, PIN_DIFF_N, PIN_DIFF_P, PIN_GROUND, PIN_POWER

    return {
        "PIN_POWER": PIN_POWER,
        "PIN_GROUND": PIN_GROUND,
        "PIN_DIFF_P": PIN_DIFF_P,
        "PIN_DIFF_N": PIN_DIFF_N,
        "PIN_CLOCK": PIN_CLOCK,
    }[name]


def parse(path: str | Path) -> ExtraConfig:
    """Read an ``extra.toml`` into board rules, component rules and rows."""
    path = Path(path)
    try:
        document = tomllib.loads(path.read_text())
    except (OSError, tomllib.TOMLDecodeError) as exc:
        raise ExtraTomlError(f"cannot read {path}: {exc}") from exc

    config = ExtraConfig(source_ref=path.name)
    overlay = config.overlay
    source_ref = path.name

    board_table = document.get("board", {})
    if not isinstance(board_table, dict):
        raise ExtraTomlError("[board] must be a table")
    board = BoardEnrichment()
    if "allowed_sides" in board_table:
        value = str(board_table["allowed_sides"]).lower()
        if value not in _SIDES:
            raise ExtraTomlError("board.allowed_sides must be both, top or bottom")
        board.allowed_sides = _SIDES[value]
    if "ceiling_height_mm" in board_table:
        board.ceiling_height_mm = _number(board_table["ceiling_height_mm"], "ceiling_height_mm")
    if "airflow" in board_table:
        airflow = board_table["airflow"]
        if not isinstance(airflow, list) or len(airflow) != 2:
            raise ExtraTomlError("board.airflow must be [x, y]")
        board.airflow = (float(airflow[0]), float(airflow[1]))
    if "via_under_body_allowed" in board_table:
        board.via_under_body_allowed = bool(board_table["via_under_body_allowed"])
    if "courtyard_margin_mm" in board_table:
        board.courtyard_margin_mm = _number(
            board_table["courtyard_margin_mm"], "courtyard_margin_mm"
        )
    for key, target in (("net_clearance", board.net_clearance),
                        ("net_class", board.net_class_of),
                        ("class_clearance", board.class_clearance)):
        table = board_table.get(key, {})
        if not isinstance(table, dict):
            raise ExtraTomlError(f"board.{key} must be a table")
        for name, value in table.items():
            target[str(name)] = float(value) if key != "net_class" else str(value)
    overlay.board = board

    for index, entry in enumerate(_table_list(document, "component", source_ref)):
        config.component_rules.append(_parse_component_rule(entry, index))

    for entry in _table_list(document, "thermal", source_ref):
        overlay.thermal.append(
            ThermalRow(
                component=_require(entry, "component", "thermal"),
                power_w=_number(entry.get("power_w", 0.0), "thermal.power_w", positive=False),
                radius_mm=_number(entry.get("radius_mm", 3.0), "thermal.radius_mm"),
                r_theta_ja=_number(entry.get("r_theta_ja", 0.0), "thermal.r_theta_ja",
                                   positive=False),
            )
        )
    for entry in _table_list(document, "decoupling", source_ref):
        overlay.decoupling.append(
            DecouplingRow(
                ic=_require(entry, "ic", "decoupling"),
                cap=_require(entry, "cap", "decoupling"),
                pin=str(entry["pin"]) if "pin" in entry else None,
                max_distance_mm=_number(
                    entry.get("max_distance_mm", 5.0), "decoupling.max_distance_mm"
                ),
                weight=_number(entry.get("weight", 1.0), "decoupling.weight"),
            )
        )
    for entry in _table_list(document, "symmetry", source_ref):
        axis = str(entry.get("axis", "x")).lower()
        if axis not in _AXES:
            raise ExtraTomlError(f"symmetry.axis must be one of {', '.join(_AXES)}")
        overlay.symmetry.append(
            SymmetryRow(
                a=_require(entry, "a", "symmetry"),
                b=_require(entry, "b", "symmetry"),
                axis=axis,
                weight=_number(entry.get("weight", 1.0), "symmetry.weight"),
            )
        )
    for entry in _table_list(document, "diffpair", source_ref):
        overlay.diffpairs.append(
            DiffPairRow(
                p=_require(entry, "p", "diffpair"),
                n=_require(entry, "n", "diffpair"),
                max_skew_mm=_number(entry.get("max_skew_mm", 0.0), "diffpair.max_skew_mm",
                                    positive=False),
            )
        )
    for entry in _table_list(document, "keepout", source_ref):
        polygon = entry.get("polygon")
        if not isinstance(polygon, list) or len(polygon) < 3:
            raise ExtraTomlError("keepout.polygon needs at least 3 [x, y] points")
        points = [[float(point[0]), float(point[1])] for point in polygon]
        overlay.board.keepouts.append(
            KeepoutRow(polygon=points, layer_mask=int(entry.get("layer_mask", 0)))
        )

    return config


def _table_list(document: dict, key: str, source_ref: str) -> list[dict]:
    entries = document.get(key, [])
    if not isinstance(entries, list):
        raise ExtraTomlError(f"[[{key}]] must be a list of tables")
    return entries


def _require(entry: dict, key: str, table: str) -> str:
    if key not in entry:
        raise ExtraTomlError(f"[[{table}]] needs a '{key}'")
    return str(entry[key])


def _parse_component_rule(entry: dict, index: int) -> ComponentRule:
    if "ref" not in entry and "fpid" not in entry:
        raise ExtraTomlError(f"[[component]] #{index} needs a 'ref' or an 'fpid' selector")
    ref = str(entry.get("ref", entry.get("fpid", "")))
    component = _parse_component_fields(entry, ref)
    return ComponentRule(
        entry=component,
        ref_pattern=str(entry["ref"]) if "ref" in entry else None,
        fpid_pattern=str(entry["fpid"]) if "fpid" in entry else None,
        order=index,
    )


def _parse_component_fields(entry: dict, ref: str) -> ComponentEnrichment:
    component = ComponentEnrichment(ref=ref)
    if "height_mm" in entry:
        component.height_mm = _number(entry["height_mm"], f"{ref}.height_mm", positive=False)
    if "mass_g" in entry:
        component.mass_g = _number(entry["mass_g"], f"{ref}.mass_g", positive=False)
    if "orientation_deg" in entry:
        component.orientation_deg = float(entry["orientation_deg"])
    if "rot_fixed" in entry:
        component.rot_fixed = bool(entry["rot_fixed"])
    if "locked" in entry:
        component.locked = bool(entry["locked"])
    if "side" in entry:
        side = str(entry["side"]).lower()
        if side not in ("top", "bottom"):
            raise ExtraTomlError(f"{ref}.side must be top or bottom")
        component.side = side
    if "pin_roles" in entry:
        component.pin_roles = _parse_roles(entry["pin_roles"], ref)
    if "pin_currents" in entry:
        component.pin_currents = {
            pin: _number(value, f"{ref}.pin_currents[{pin}]", positive=False)
            for pin, value in _pin_numbers(entry["pin_currents"], f"{ref}.pin_currents").items()
        }
    if "power_w" in entry:
        component.power_w = _number(entry["power_w"], f"{ref}.power_w", positive=False)
    if "r_theta_ja" in entry:
        component.r_theta_ja = _number(entry["r_theta_ja"], f"{ref}.r_theta_ja", positive=False)
    if "exclusion_radius_mm" in entry:
        component.exclusion_radius_mm = _number(
            entry["exclusion_radius_mm"], f"{ref}.exclusion_radius_mm"
        )
    if "part_number" in entry:
        component.part_number = str(entry["part_number"])
    return component


def _merge_component(target: ComponentEnrichment, incoming: ComponentEnrichment) -> None:
    """Later [[component]] blocks win per field."""
    for name in (
        "height_mm",
        "mass_g",
        "orientation_deg",
        "rot_fixed",
        "locked",
        "side",
        "power_w",
        "r_theta_ja",
        "exclusion_radius_mm",
        "part_number",
    ):
        value = getattr(incoming, name)
        if value is not None:
            setattr(target, name, value)
    target.pin_roles.update(incoming.pin_roles)
    target.pin_currents.update(incoming.pin_currents)


def match_glob(patterns: dict, name: str):
    """First glob in ``patterns`` matching ``name`` (case sensitive)."""
    for pattern, value in patterns.items():
        if fnmatch.fnmatchcase(name, pattern):
            return value
    return None
