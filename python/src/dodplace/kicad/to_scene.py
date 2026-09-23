"""Turn a KiCad extract (plus resolved enrichment) into the scene IR.

Division of labour with the C engine
-------------------------------------
The producer resolves everything it has data for; whatever it cannot resolve is
left absent and the *engine* applies its documented fallback (raising the same
DEGRADED_* bit). To avoid implementing a fallback twice, this module raises only
the bits that follow from **coverage** - a field is degraded because some
component or pin still lacks it after every source has been consulted.

Board-level bits (outline from bbox, keepouts, stackup, clearance matrix,
ceiling, airflow, via restriction, length bound, segregation) are left to
``placer_context_finalize``, which derives them from the data it receives.
"""

from __future__ import annotations

import math

from dataclasses import dataclass, field

from ..ir.spec import (
    COMP_HAS_HEIGHT,
    COMP_KIND_UNKNOWN,
    KIND_BY_PREFIX,
    COMP_HAS_MASS,
    COMP_LOCKED,
    COMP_POLY_COURTYARD,
    COMP_ROT_FIXED,
    COMP_SIDE_BOTTOM,
    DEGRADED_BY_NAME,
    NO_ID,
    PIN_INFERRED,
    PIN_NPTH,
    PIN_PTH,
    POLY_KIND_BOARD_OUTLINE,
    POLY_KIND_COURTYARD,
    POLY_KIND_KEEPOUT,
    SIDE_BOTH,
)
from ..ir.writer import SceneBuilder
from ..enrich.overlay import EnrichmentOverlay
from ..enrich.resolver import ResolveReport
from ..enrich.providers.extra_toml import match_glob
from .naming import find_diff_pairs, infer_pin_flags
from .schema import validate

_MIN_CLEARANCE_MM = 0.01
_DEFAULT_CLEARANCE_MM = 0.2
_DEFAULT_TRACK_MM = 0.2
_DEFAULT_THERMAL_RADIUS_MM = 3.0

_AXIS_INDEX = {"x": 0, "y": 1, "fixed": 2}


@dataclass
class IngestReport:
    """What the conversion did, for the CLI and the provenance report."""

    n_components: int = 0
    n_pins: int = 0
    n_nets: int = 0
    n_net_entries: int = 0
    derived_courtyards: list[str] = field(default_factory=list)
    inferred_pin_roles: int = 0
    authoritative_pin_roles: int = 0
    diff_pairs: list[tuple[str, str]] = field(default_factory=list)
    netclass_names: list[str] = field(default_factory=list)
    degraded: int = 0
    warnings: list[str] = field(default_factory=list)
    enrichment: ResolveReport | None = None
    coverage: dict[str, int] = field(default_factory=dict)
    constraints: dict[str, int] = field(default_factory=dict)

    def degraded_names(self) -> list[str]:
        from ..ir.spec import degraded_names

        return degraded_names(self.degraded)


@dataclass
class IngestResult:
    scene: object  # ir.Scene
    builder: SceneBuilder
    report: IngestReport


_REF_PREFIX = __import__("re").compile(r"^([A-Za-z]+)")


def _fnv1a(text: str) -> int:
    """32-bit FNV-1a: the part's identity, stable across runs and machines."""
    digest = 0x811C9DC5
    for byte in text.encode("utf-8"):
        digest = ((digest ^ byte) * 0x01000193) & 0xFFFFFFFF
    return digest


def part_id_of(comp: dict) -> int:
    """The supplier part number, else the value; 0 means no identity to swap on."""
    fields = comp.get("fields") or {}
    key = fields.get("LCSC Part") or comp.get("value") or ""
    key = str(key).strip()
    return _fnv1a(key) if key else 0


def _pad_half_extents(pad: dict) -> tuple[float, float]:
    """The pad's axis-aligned half extents in the footprint frame, in mm.

    A pad carries its own rotation: a 0.54x0.64 pad turned 90 degrees is 0.32
    wide and 0.27 tall. Zero means the producer had nothing to say.
    """
    # A drilled pad obstructs at least its drill: an NPTH mounting hole is
    # 4.0 mm of hole in a 4.0x3.2 pad, and modelling the pad alone left 0.4 mm
    # of the hole invisible to the copper test - which is a copper-to-edge
    # violation in KiCad's report, against a neighbour on the far side.
    drill = float(pad.get("drill", 0.0))
    sx = max(float(pad.get("size_x", 0.0)), drill) * 0.5
    sy = max(float(pad.get("size_y", 0.0)), drill) * 0.5
    if sx <= 0.0 and sy <= 0.0:
        return (0.0, 0.0)
    theta = math.radians(float(pad.get("rot_deg", 0.0)))
    c, sn = abs(math.cos(theta)), abs(math.sin(theta))
    return (c * sx + sn * sy, sn * sx + c * sy)


def kind_from_ref(reference: str) -> int:
    """Component role, from the reference designator prefix.

    `U` IC, `C` capacitor, `L` inductor, `Y` crystal... KiCad's own convention,
    and the only semantic signal available without a datasheet. Longest prefix
    wins, so `FID1` is not read as `F` and `CE1` is not read as `C` by accident.
    """
    match = _REF_PREFIX.match(reference or "")
    if not match:
        return COMP_KIND_UNKNOWN
    prefix = match.group(1).upper()
    for length in range(min(len(prefix), 3), 0, -1):
        kind = KIND_BY_PREFIX.get(prefix[:length])
        if kind is not None:
            return kind
    return COMP_KIND_UNKNOWN


def _positive(value, fallback: float) -> float:
    try:
        number = float(value)
    except (TypeError, ValueError):
        return fallback
    return number if number > 0.0 else fallback


def pad_bbox_cycle(comp: dict, margin_mm: float) -> list[list[float]]:
    """Courtyard from pad extents plus margin, in footprint-local coordinates.

    A pad's own rotation is not carried by the extract, so the larger of its two
    dimensions is used as a half-extent on both axes: a conservative rectangle
    that can only over-estimate the courtyard.
    """
    pads = comp.get("pads", [])
    if not pads:
        half = margin_mm
        return [[-half, -half], [half, -half], [half, half], [-half, half]]

    def extent(pad) -> float:
        return max(float(pad["size_x"]), float(pad["size_y"])) / 2.0

    min_x = min(pad["x"] - extent(pad) for pad in pads)
    max_x = max(pad["x"] + extent(pad) for pad in pads)
    min_y = min(pad["y"] - extent(pad) for pad in pads)
    max_y = max(pad["y"] + extent(pad) for pad in pads)
    return [
        [min_x - margin_mm, min_y - margin_mm],
        [max_x + margin_mm, min_y - margin_mm],
        [max_x + margin_mm, max_y + margin_mm],
        [min_x - margin_mm, max_y + margin_mm],
    ]


def _netclass_table(
    doc: dict, overlay: EnrichmentOverlay
) -> tuple[list[str], dict[str, int], dict[str, float], dict[str, str], list[str]]:
    """Class order, name->index, class->clearance and net->class overrides.

    A net named in ``board.net_clearance`` gets its own class, so a clearance
    override never leaks onto unrelated nets that share the default class.
    """
    definitions = {nc["name"]: nc for nc in doc["netclasses"]}
    definitions.setdefault(
        "Default", {"name": "Default", "clearance_mm": _DEFAULT_CLEARANCE_MM,
                    "track_width_mm": _DEFAULT_TRACK_MM}
    )
    warnings: list[str] = []
    class_of_net: dict[str, str] = {}
    net_names = sorted(
        {pad["net"] for comp in doc["components"] for pad in comp["pads"] if pad["net"]}
    )
    board_class_of = {entry["name"]: entry["netclass"] for entry in doc.get("nets", [])}

    for net in net_names:
        pattern_match = match_glob(overlay.board.net_class_of, net)
        if pattern_match:
            class_of_net[net] = pattern_match
        elif net in overlay.board.net_clearance:
            class_of_net[net] = f"net:{net}"
        else:
            class_of_net[net] = board_class_of.get(net, "Default")

    # Every referenced class needs a definition; a clearance override or an
    # explicit class_clearance may introduce a class the board never had.
    for net, class_name in class_of_net.items():
        if class_name.startswith("net:"):
            definitions[class_name] = {
                "name": class_name,
                "clearance_mm": overlay.board.net_clearance[net],
                "track_width_mm": definitions["Default"]["track_width_mm"],
            }
        elif class_name not in definitions:
            known = {nc["name"] for nc in doc["netclasses"]} | set(
                overlay.board.class_clearance
            )
            if class_name not in known:
                warnings.append(
                    f"netclass '{class_name}' is referenced by a net but not defined "
                    f"by the board; using the default clearance"
                )
            definitions[class_name] = {
                "name": class_name,
                "clearance_mm": overlay.board.class_clearance.get(
                    class_name, _DEFAULT_CLEARANCE_MM
                ),
                "track_width_mm": _DEFAULT_TRACK_MM,
            }
    for class_name, clearance in overlay.board.class_clearance.items():
        if class_name in definitions:
            definitions[class_name] = {**definitions[class_name], "clearance_mm": clearance}

    order = sorted(definitions, key=lambda name: (name != "Default", name))
    index = {name: position for position, name in enumerate(order)}
    clearances = {
        name: _positive(definitions[name].get("clearance_mm"), _DEFAULT_CLEARANCE_MM)
        for name in order
    }
    return order, index, clearances, class_of_net, warnings


def build_scene(
    doc: dict,
    *,
    margin_mm: float = 0.25,
    allowed_sides: int = SIDE_BOTH,
    enrichment: EnrichmentOverlay | None = None,
    enrichment_report: ResolveReport | None = None,
) -> IngestResult:
    """Convert a validated extract (+ optional enrichment) into an IR scene."""
    validate(doc)

    overlay = enrichment or EnrichmentOverlay()
    report = IngestReport(
        warnings=list(doc.get("warnings", [])) + list(overlay.warnings),
        enrichment=enrichment_report,
    )
    builder = SceneBuilder()
    degraded = 0

    if overlay.board.courtyard_margin_mm is not None:
        margin_mm = overlay.board.courtyard_margin_mm
    if overlay.board.allowed_sides is not None:
        allowed_sides = overlay.board.allowed_sides

    # --- netclasses -> clearance matrix -----------------------------------
    order, class_index, clearances, class_of_net, netclass_warnings = _netclass_table(doc, overlay)
    report.warnings.extend(netclass_warnings)
    report.netclass_names = order
    size = len(order)
    matrix = [
        [max(clearances[order[i]], clearances[order[j]], _MIN_CLEARANCE_MM) for j in range(size)]
        for i in range(size)
    ]
    builder.set_clearance_matrix(matrix)

    # --- nets --------------------------------------------------------------
    net_names = sorted(
        {pad["net"] for comp in doc["components"] for pad in comp["pads"] if pad["net"]}
    )
    nets: dict[str, int] = {}
    non_default = False
    for name in net_names:
        class_name = class_of_net.get(name, "Default")
        if class_name not in class_index:
            report.warnings.append(f"net '{name}' uses unknown netclass '{class_name}'")
            class_name = "Default"
        if class_name != "Default":
            non_default = True
        nets[name] = builder.add_net(weight=1.0, net_class=class_index[class_name])

    if not non_default:
        degraded |= DEGRADED_BY_NAME["NO_NET_CLASSES"]
    degraded |= DEGRADED_BY_NAME["NO_NET_WEIGHTS"]

    # --- components, pins, courtyards --------------------------------------
    missing_height = 0
    missing_mass = 0
    missing_orientation = 0
    missing_current = 0
    missing_roles = 0
    inferred_roles = 0

    for comp in doc["components"]:
        ref = comp["ref"]
        extra = overlay.components.get(ref)

        rotation = float(comp["rot_deg"])
        if extra is not None and extra.orientation_deg is not None:
            rotation = float(extra.orientation_deg)
        else:
            missing_orientation += 1

        quarters = int(round(rotation / 90.0))
        flags = quarters % 4
        if abs(rotation - 90.0 * quarters) > 1e-3:
            flags |= COMP_ROT_FIXED

        side = comp["side"]
        if extra is not None and extra.side is not None:
            side = extra.side
        if side == "bottom":
            flags |= COMP_SIDE_BOTTOM

        locked = bool(comp["locked"])
        if extra is not None and extra.locked is not None:
            locked = extra.locked
        if locked:
            flags |= COMP_LOCKED
        if extra is not None and extra.rot_fixed:
            flags |= COMP_ROT_FIXED

        height = 0.0
        if extra is not None and extra.height_mm is not None:
            height = float(extra.height_mm)
            flags |= COMP_HAS_HEIGHT
        else:
            missing_height += 1

        mass = 0.0
        if extra is not None and extra.mass_g is not None:
            mass = float(extra.mass_g)
            flags |= COMP_HAS_MASS
        else:
            missing_mass += 1

        cycles = comp.get("courtyard") or []
        if cycles:
            courtyard = [[float(x), float(y)] for x, y in cycles[0]]
        else:
            courtyard = pad_bbox_cycle(comp, margin_mm)
            report.derived_courtyards.append(ref)
        polygon_id = builder.add_polygon(POLY_KIND_COURTYARD, courtyard)
        flags |= COMP_POLY_COURTYARD

        comp_id = builder.add_component(
            comp["x"],
            comp["y"],
            half_w=0.0,
            half_h=0.0,
            height=height,
            mass=mass,
            flags=flags,
            kind=kind_from_ref(ref),
            polygon_id=polygon_id,
            ref=ref,
            part_id=part_id_of(comp),
        )

        for pad in comp.get("pads", []):
            number = str(pad.get("number", ""))
            net_name = pad.get("net")
            resolved = extra.pin_roles.get(number) if extra is not None else None
            if resolved is not None:
                pin_flags = resolved
                report.authoritative_pin_roles += 1
            else:
                # No authoritative role: try the net name, and remember that the
                # pin is unresolved even when the heuristic happens to recognise
                # nothing (a plain signal is still a guess).
                missing_roles += 1
                pin_flags = infer_pin_flags(net_name)
                if pin_flags & PIN_INFERRED:
                    inferred_roles += 1
                    report.inferred_pin_roles += 1
            current = 0.0
            if extra is not None and number in extra.pin_currents:
                current = float(extra.pin_currents[number])
            if current <= 0.0:
                missing_current += 1
            # The copper of the pad, as half extents in the footprint's own
            # frame: a pad carries its own rotation, so a 0.54x0.64 pad turned
            # 90 degrees is 0.32 wide and 0.27 tall. The engine uses these to
            # keep courtyards that are smaller than the metal from letting two
            # parts touch; zero means "not supplied".
            half_x, half_y = _pad_half_extents(pad)
            if pad.get("attrib") == "pth":
                pin_flags |= PIN_PTH
            elif pad.get("attrib") == "npth":
                # A non-plated hole is not copper, but it is still a hole: its
                # drill and mask opening cross every layer, so the far side of
                # the board is not free either.
                pin_flags |= PIN_NPTH
            builder.add_pin(
                comp_id,
                pad["x"],
                pad["y"],
                net=nets.get(net_name, NO_ID) if net_name else NO_ID,
                flags=pin_flags,
                max_current=current,
                half_x=half_x,
                half_y=half_y,
            )

    if report.derived_courtyards:
        degraded |= DEGRADED_BY_NAME["COURTYARD_BBOX"]
    if missing_height:
        degraded |= DEGRADED_BY_NAME["NO_3D_HEIGHT"]
    if missing_mass:
        degraded |= DEGRADED_BY_NAME["NO_MASS"]
    if missing_orientation:
        degraded |= DEGRADED_BY_NAME["NO_ROTATION_DATA"]
    if missing_roles:
        degraded |= DEGRADED_BY_NAME["NO_PIN_ELEC"]
    if missing_current:
        degraded |= DEGRADED_BY_NAME["NO_PIN_CURRENT"]

    ncomp = len(doc["components"])
    npin = len(builder._columns["pins.net_id"])
    report.coverage = {
        "height_mm": ncomp - missing_height,
        "mass_g": ncomp - missing_mass,
        "orientation_deg": ncomp - missing_orientation,
        "pin_current": npin - missing_current,
        "pin_role": npin - missing_roles,
        "components": ncomp,
        "pins": npin,
    }

    # --- board outline and keepouts ---------------------------------------
    outline = doc["board"].get("outline") or []
    if outline and len(outline[0]) >= 3:
        builder.add_polygon(POLY_KIND_BOARD_OUTLINE, outline[0])
    for keepout in doc["keepouts"]:
        for cycle in keepout.get("xy", []):
            builder.add_polygon(
                POLY_KIND_KEEPOUT, cycle, layer_mask=int(keepout.get("layer_mask", 0))
            )
    for keepout in overlay.board.keepouts:
        builder.add_polygon(POLY_KIND_KEEPOUT, keepout.polygon, layer_mask=keepout.layer_mask)

    # --- differential pairs ------------------------------------------------
    pairs = find_diff_pairs(net_names)
    for positive, negative in pairs:
        builder.add_diffpair(nets[positive], nets[negative], 0.0)
    report.diff_pairs = pairs
    for row in overlay.diffpairs:
        if row.p in nets and row.n in nets:
            builder.add_diffpair(nets[row.p], nets[row.n], row.max_skew_mm)
            report.diff_pairs.append((row.p, row.n))
        else:
            report.warnings.append(
                f"diffpair {row.p}/{row.n} refers to nets missing from the board"
            )
    if not report.diff_pairs:
        degraded |= DEGRADED_BY_NAME["NO_DIFFPAIRS"]

    # --- constraints from enrichment --------------------------------------
    ref_index = {comp["ref"]: index for index, comp in enumerate(doc["components"])}
    report.constraints = {"decoupling": 0, "thermal": 0, "symmetry": 0}

    # The engine's sparse tables are indexed by their primary component through a
    # CSR array built by a counting pass, so rows must arrive grouped by that
    # component in ascending order. Sort here, once, rather than making every
    # provider responsible for it.
    decoupling_rows = []
    for row in overlay.decoupling:
        ic = ref_index.get(row.ic)
        cap = ref_index.get(row.cap)
        if ic is None or cap is None:
            report.warnings.append(f"decoupling {row.ic}->{row.cap}: unknown component")
            continue
        decoupling_rows.append((ic, cap, row))
    decoupling_rows.sort(key=lambda item: (item[0], item[1]))
    for ic, cap, row in decoupling_rows:
        builder.add_decoupling(ic, cap, NO_ID, row.max_distance_mm, row.weight)
        report.constraints["decoupling"] += 1

    thermal_rows = []
    for row in overlay.thermal:
        index = ref_index.get(row.component)
        if index is None:
            report.warnings.append(f"thermal {row.component}: unknown component")
            continue
        thermal_rows.append((index, row))
    thermal_rows.sort(key=lambda item: item[0])
    for index, row in thermal_rows:
        builder.add_thermal(index, row.power_w, row.radius_mm or _DEFAULT_THERMAL_RADIUS_MM,
                            row.r_theta_ja)
        report.constraints["thermal"] += 1

    symmetry_rows = []
    for row in overlay.symmetry:
        a = ref_index.get(row.a)
        b = ref_index.get(row.b)
        if a is None or b is None or a == b:
            report.warnings.append(f"symmetry {row.a}/{row.b}: invalid components")
            continue
        symmetry_rows.append((a, b, row))
    symmetry_rows.sort(key=lambda item: (item[0], item[1]))
    for a, b, row in symmetry_rows:
        builder.add_symmetry(a, b, _AXIS_INDEX.get(row.axis, 0), row.weight)
        report.constraints["symmetry"] += 1

    # --- board configuration ----------------------------------------------
    copper_layers = int(doc["board"].get("copper_layers") or 0)
    board = builder.board
    board.allowed_sides = allowed_sides
    board.copper_layers = copper_layers
    board.has_stackup = 1 if copper_layers > 0 else 0
    board.global_clearance = clearances["Default"]
    board.courtyard_clearance = clearances["Default"]
    default_class = next(
        (nc for nc in doc["netclasses"] if nc["name"] == "Default"), None
    )
    board.min_track_width = _positive(
        (default_class or {}).get("track_width_mm"), _DEFAULT_TRACK_MM
    )
    board.courtyard_fallback_margin = margin_mm
    if overlay.board.ceiling_height_mm is not None:
        board.ceiling_height = overlay.board.ceiling_height_mm
    if overlay.board.airflow is not None:
        board.airflow_x, board.airflow_y = overlay.board.airflow
    if overlay.board.via_under_body_allowed is not None:
        board.allow_vias_under_body = 1 if overlay.board.via_under_body_allowed else 0

    builder.degraded = degraded
    counts = builder.counts()
    report.n_components = counts["comps"]
    report.n_pins = counts["pins"]
    report.n_nets = counts["nets"]
    report.n_net_entries = counts["net_entries"]
    report.degraded = degraded

    return IngestResult(scene=builder.build(), builder=builder, report=report)
