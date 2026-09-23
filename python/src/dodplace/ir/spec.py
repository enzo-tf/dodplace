"""Mirror of ``ir/spec.md`` - the single source of truth for the IR layout.

Kept in lockstep with ``include/place/io.h``. The golden fixture
``ir/fixtures/scene_minimal.bin`` is the executable contract: this module must
be able to decode and re-encode it byte for byte.

All dtypes carry an explicit little-endian byte order so the file is identical
whichever machine produced it.
"""

from __future__ import annotations

import enum
import struct
from dataclasses import dataclass, field

MAGIC = b"DODP"
RESULT_MAGIC = b"DODR"
VERSION_MAJOR = 1
VERSION_MINOR = 0
HEADER_SIZE = 128
SECTION_ENTRY_SIZE = 32
ALIGNMENT = 64
MAX_SECTIONS = 256
FLAG_LITTLE_ENDIAN = 0x00000001

#: Free-form JSON tail; must match PLACE_STATIC_TAIL in src/io.c.
TAIL = b'{"generator":"dodplace","ir":"1.0"}'

#: Sentinel for "no such element" in every u32 id field.
NO_ID = 0xFFFFFFFF

#: ``counts[]`` order in the header.
COUNT_SLOTS = (
    "comps",
    "pins",
    "nets",
    "net_entries",
    "polygons",
    "vertices",
    "decoupling",
    "thermal",
    "symmetry",
    "diffpairs",
)

BOARD_CONFIG_SIZE = 96


class Section(enum.IntEnum):
    """Section kinds, values fixed by the spec."""

    COMP_X = 1
    COMP_Y = 2
    COMP_HALF_W = 3
    COMP_HALF_H = 4
    COMP_HEIGHT = 5
    COMP_MASS = 6
    COMP_FLAGS = 7
    COMP_PIN_COUNT = 8
    COMP_POLYGON_ID = 9

    PIN_OFFSET_X = 10
    PIN_OFFSET_Y = 11
    PIN_COMP_ID = 12
    PIN_NET_ID = 13
    PIN_FLAGS = 14
    PIN_MAX_CURRENT = 15

    NET_WEIGHT = 16
    NET_CLASS = 17

    POLY_VX = 18
    POLY_VY = 19
    POLY_OFFSETS = 20
    POLY_KIND = 21
    POLY_LAYER_MASK = 22

    DECOUPLING_IC = 23
    DECOUPLING_CAP = 24
    DECOUPLING_PIN = 25
    DECOUPLING_MAX_DIST_SQ = 26
    DECOUPLING_WEIGHT = 27

    THERMAL_COMP = 28
    THERMAL_POWER = 29
    THERMAL_RADIUS = 30
    THERMAL_R_THETA = 31

    SYMMETRY_A = 32
    SYMMETRY_B = 33
    SYMMETRY_AXIS = 34
    SYMMETRY_WEIGHT = 35

    DIFFPAIR_P = 36
    DIFFPAIR_N = 37
    DIFFPAIR_SKEW = 38

    CLEARANCE_MATRIX = 39
    BOARD_CONFIG = 40

    # Added after the first release: kinds are stable identifiers, never
    # renumbered, so a new one continues the series.
    COMP_KIND = 41

    # The copper of each pad, as half extents in the footprint's own frame.
    # Optional: zero means "not supplied" and the engine falls back to the pad
    # centre, which is what every scene written before this section did.
    PIN_HALF_X = 42
    PIN_HALF_Y = 43

    # The part's identity, so the solver may exchange it with another instance
    # of the same part. Hashed on the producer side; 0 = unknown, never swapped.
    PART_ID = 44

    JSON_TAIL = 900


# Component kinds (place/types.h), derived by the producer from the reference
# designator prefix. The engine has no names, and the semantic clustering stage
# needs to know what it is looking at.
COMP_KIND_UNKNOWN = 0
COMP_KIND_IC = 1
COMP_KIND_CAPACITOR = 2
COMP_KIND_RESISTOR = 3
COMP_KIND_INDUCTOR = 4
COMP_KIND_DIODE_LED = 5
COMP_KIND_TRANSISTOR = 6
COMP_KIND_CRYSTAL = 7
COMP_KIND_CONNECTOR = 8
COMP_KIND_MECHANICAL = 9
COMP_KIND_OTHER = 10

#: Reference prefix -> kind. KiCad's convention, and the only place a part's
#: role is available without a datasheet.
KIND_BY_PREFIX = {
    "U": COMP_KIND_IC,
    "IC": COMP_KIND_IC,
    "C": COMP_KIND_CAPACITOR,
    "CE": COMP_KIND_CAPACITOR,
    "R": COMP_KIND_RESISTOR,
    "L": COMP_KIND_INDUCTOR,
    "FB": COMP_KIND_INDUCTOR,
    "D": COMP_KIND_DIODE_LED,
    "LED": COMP_KIND_DIODE_LED,
    "Q": COMP_KIND_TRANSISTOR,
    "Y": COMP_KIND_CRYSTAL,
    "X": COMP_KIND_CRYSTAL,
    "XT": COMP_KIND_CRYSTAL,
    "J": COMP_KIND_CONNECTOR,
    "P": COMP_KIND_CONNECTOR,
    "CN": COMP_KIND_CONNECTOR,
    "H": COMP_KIND_MECHANICAL,
    "MK": COMP_KIND_MECHANICAL,
    "FID": COMP_KIND_MECHANICAL,
    "TP": COMP_KIND_MECHANICAL,
}

# Component flags (place/types.h).
COMP_ORIENT_MASK = 0x03
COMP_SIDE_BOTTOM = 0x04
COMP_LOCKED = 0x08
COMP_ROT_FIXED = 0x10
COMP_POLY_COURTYARD = 0x20
COMP_HAS_HEIGHT = 0x40
COMP_HAS_MASS = 0x80

# Pin flags (place/types.h).
PIN_POWER = 0x01
PIN_GROUND = 0x02
PIN_DIFF_P = 0x04
PIN_DIFF_N = 0x08
PIN_CLOCK = 0x10
PIN_PTH = 0x20
PIN_INFERRED = 0x80

# Polygon kinds (place/constraints.h).
POLY_KIND_COURTYARD = 0
POLY_KIND_BOARD_OUTLINE = 1
POLY_KIND_KEEPOUT = 2

# Symmetry axes.
SYM_AXIS_X = 0
SYM_AXIS_Y = 1
SYM_FIXED = 2

# Placement record flags.
PLACEMENT_FLAG_LOCKED = 0x01
PLACEMENT_FLAG_MOVED = 0x02
PLACEMENT_FLAG_UNPLACED = 0x04

# Side mask (place/context.h).
SIDE_TOP = 0x01
SIDE_BOTTOM = 0x02
SIDE_BOTH = 0x03

# Degraded-mode bits, in the order they appear in place/context.h.
DEGRADED_BITS: tuple[tuple[int, str], ...] = (
    (1 << 0, "NO_3D_HEIGHT"),
    (1 << 1, "NO_MASS"),
    (1 << 2, "NO_ROTATION_DATA"),
    (1 << 3, "NO_PIN_ELEC"),
    (1 << 4, "NO_PIN_CURRENT"),
    (1 << 5, "NO_NET_WEIGHTS"),
    (1 << 6, "NO_NET_CLASSES"),
    (1 << 7, "NO_DIFFPAIRS"),
    (1 << 8, "NO_KEEPOUTS"),
    (1 << 9, "NO_STACKUP"),
    (1 << 10, "NO_CLEARANCE_MATRIX"),
    (1 << 11, "NO_DECOUPLING"),
    (1 << 12, "NO_THERMAL"),
    (1 << 13, "NO_SYMMETRY"),
    (1 << 14, "NO_AIRFLOW"),
    (1 << 15, "NO_CEILING"),
    (1 << 16, "NO_VIA_RESTRICTION"),
    (1 << 17, "COURTYARD_BBOX"),
    (1 << 18, "BOARD_OUTLINE_BBOX"),
    (1 << 19, "NO_MAX_LENGTH"),
    (1 << 20, "NO_SEGREGATION"),
)

DEGRADED_BY_NAME = {name: bit for bit, name in DEGRADED_BITS}
DEGRADED_ALL_MASK = 0
for _bit, _name in DEGRADED_BITS:
    DEGRADED_ALL_MASK |= _bit
del _bit, _name


@dataclass(frozen=True)
class SectionDef:
    """One section of the scene file.

    ``count`` names a ``counts[]`` slot, or one of the special forms
    ``polygons+1``, ``net_classes^2``, ``tail`` or ``one``.
    """

    section: Section
    name: str
    dtype: str
    count: str


#: Emission order, identical to build_save_sections() in src/io.c. The golden
#: fixture follows it, and byte-for-byte round-trip depends on it.
SECTION_DEFS: tuple[SectionDef, ...] = (
    SectionDef(Section.COMP_X, "comps.x", "<f4", "comps"),
    SectionDef(Section.COMP_Y, "comps.y", "<f4", "comps"),
    SectionDef(Section.COMP_HALF_W, "comps.half_w", "<f4", "comps"),
    SectionDef(Section.COMP_HALF_H, "comps.half_h", "<f4", "comps"),
    SectionDef(Section.COMP_HEIGHT, "comps.height", "<f4", "comps"),
    SectionDef(Section.COMP_MASS, "comps.mass", "<f4", "comps"),
    SectionDef(Section.COMP_FLAGS, "comps.flags", "u1", "comps"),
    SectionDef(Section.COMP_PIN_COUNT, "comps.pin_count", "<u2", "comps"),
    SectionDef(Section.COMP_POLYGON_ID, "comps.polygon_id", "<u4", "comps"),
    # Emitted here to keep the components together; the kind number (41) is
    # deliberately not in this position.
    SectionDef(Section.COMP_KIND, "comps.kind", "u1", "comps"),
    # Right after the kind: the C writer emits it there, and the two orders are
    # the byte-for-byte contract the golden fixture checks.
    SectionDef(Section.PART_ID, "comps.part_id", "<u4", "comps"),
    SectionDef(Section.PIN_OFFSET_X, "pins.offset_x", "<f4", "pins"),
    SectionDef(Section.PIN_OFFSET_Y, "pins.offset_y", "<f4", "pins"),
    SectionDef(Section.PIN_COMP_ID, "pins.comp_id", "<u4", "pins"),
    SectionDef(Section.PIN_NET_ID, "pins.net_id", "<u4", "pins"),
    SectionDef(Section.PIN_FLAGS, "pins.flags", "u1", "pins"),
    SectionDef(Section.PIN_MAX_CURRENT, "pins.max_current", "<f4", "pins"),
    # Emitted after the component columns for the same reason as the kind.
    SectionDef(Section.PIN_HALF_X, "pins.half_x", "<f4", "pins"),
    SectionDef(Section.PIN_HALF_Y, "pins.half_y", "<f4", "pins"),
    SectionDef(Section.NET_WEIGHT, "nets.weights", "<f4", "nets"),
    SectionDef(Section.NET_CLASS, "nets.net_class", "u1", "nets"),
    SectionDef(Section.POLY_VX, "poly.vx", "<f4", "vertices"),
    SectionDef(Section.POLY_VY, "poly.vy", "<f4", "vertices"),
    SectionDef(Section.POLY_OFFSETS, "poly.offsets", "<u4", "polygons+1"),
    SectionDef(Section.POLY_KIND, "poly.kind", "u1", "polygons"),
    SectionDef(Section.POLY_LAYER_MASK, "poly.layer_mask", "<u4", "polygons"),
    SectionDef(Section.DECOUPLING_IC, "decoupling.ic_comp", "<u4", "decoupling"),
    SectionDef(Section.DECOUPLING_CAP, "decoupling.cap_comp", "<u4", "decoupling"),
    SectionDef(Section.DECOUPLING_PIN, "decoupling.ic_pin", "<u4", "decoupling"),
    SectionDef(Section.DECOUPLING_MAX_DIST_SQ, "decoupling.max_dist_sq", "<f4", "decoupling"),
    SectionDef(Section.DECOUPLING_WEIGHT, "decoupling.weight", "<f4", "decoupling"),
    SectionDef(Section.THERMAL_COMP, "thermal.comp", "<u4", "thermal"),
    SectionDef(Section.THERMAL_POWER, "thermal.power_w", "<f4", "thermal"),
    SectionDef(Section.THERMAL_RADIUS, "thermal.exclusion_radius", "<f4", "thermal"),
    SectionDef(Section.THERMAL_R_THETA, "thermal.r_theta_ja", "<f4", "thermal"),
    SectionDef(Section.SYMMETRY_A, "symmetry.comp_a", "<u4", "symmetry"),
    SectionDef(Section.SYMMETRY_B, "symmetry.comp_b", "<u4", "symmetry"),
    SectionDef(Section.SYMMETRY_AXIS, "symmetry.axis", "u1", "symmetry"),
    SectionDef(Section.SYMMETRY_WEIGHT, "symmetry.weight", "<f4", "symmetry"),
    SectionDef(Section.DIFFPAIR_P, "diffpairs.net_p", "<u4", "diffpairs"),
    SectionDef(Section.DIFFPAIR_N, "diffpairs.net_n", "<u4", "diffpairs"),
    SectionDef(Section.DIFFPAIR_SKEW, "diffpairs.max_skew_mm", "<f4", "diffpairs"),
    SectionDef(Section.CLEARANCE_MATRIX, "rules.class_clearance", "<f4", "net_classes^2"),
)

SECTION_BY_KIND = {d.section: d for d in SECTION_DEFS}


@dataclass
class BoardConfig:
    """The 96-byte BOARD_CONFIG payload (ir/spec.md section 7)."""

    grid_origin_x: float = 0.0
    grid_origin_y: float = 0.0
    grid_fine: float = 0.1
    grid_coarse: float = 0.5
    courtyard_fallback_margin: float = 0.25
    global_clearance: float = 0.2
    courtyard_clearance: float = 0.2
    min_track_width: float = 0.2
    total_thickness: float = 0.0
    ceiling_height: float = 0.0
    airflow_x: float = 0.0
    airflow_y: float = 0.0
    allowed_sides: int = SIDE_BOTH
    copper_layers: int = 0
    has_stackup: int = 0
    allow_vias_under_body: int = 1
    disable_mask: int = 0
    degraded_mask: int = 0
    reserved: tuple[int, ...] = field(default_factory=lambda: (0,) * 6)

    _FMT = "<12f12I"

    def to_bytes(self) -> bytes:
        return struct.pack(
            self._FMT,
            self.grid_origin_x,
            self.grid_origin_y,
            self.grid_fine,
            self.grid_coarse,
            self.courtyard_fallback_margin,
            self.global_clearance,
            self.courtyard_clearance,
            self.min_track_width,
            self.total_thickness,
            self.ceiling_height,
            self.airflow_x,
            self.airflow_y,
            self.allowed_sides,
            self.copper_layers,
            self.has_stackup,
            self.allow_vias_under_body,
            self.disable_mask,
            self.degraded_mask,
            *self.reserved,
        )

    @classmethod
    def from_bytes(cls, raw: bytes) -> BoardConfig:
        values = struct.unpack(cls._FMT, raw)
        return cls(
            *values[:12],
            allowed_sides=values[12],
            copper_layers=values[13],
            has_stackup=values[14],
            allow_vias_under_body=values[15],
            disable_mask=values[16],
            degraded_mask=values[17],
            reserved=tuple(values[18:24]),
        )


def degraded_names(mask: int) -> list[str]:
    """Human-readable names for the set bits of ``mask``."""
    return [name for bit, name in DEGRADED_BITS if mask & bit]
