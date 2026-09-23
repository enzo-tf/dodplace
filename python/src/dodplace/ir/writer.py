"""Encode a :class:`~dodplace.ir.reader.Scene` back to ``scene.bin``.

Section emission order and padding mirror ``build_save_sections()`` in
``src/io.c`` exactly, so a scene decoded from a C-written file re-encodes to
the same bytes. That equivalence is asserted by ``tests/test_writer.py`` and is
what makes the golden fixture a real contract.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field

import numpy as np

from .reader import Scene, SceneFormatError
from .spec import (
    ALIGNMENT,
    COUNT_SLOTS,
    DEGRADED_ALL_MASK,
    FLAG_LITTLE_ENDIAN,
    HEADER_SIZE,
    MAGIC,
    MAX_SECTIONS,
    NO_ID,
    SECTION_DEFS,
    SECTION_ENTRY_SIZE,
    TAIL,
    VERSION_MAJOR,
    VERSION_MINOR,
    BoardConfig,
    Section,
)


def _align_up(value: int, alignment: int) -> int:
    return (value + alignment - 1) & ~(alignment - 1)


def _section_count(key: str, counts: dict[str, int], num_net_classes: int) -> int:
    if key == "polygons+1":
        return counts["polygons"] + 1 if counts["polygons"] else 0
    if key == "net_classes^2":
        return num_net_classes * num_net_classes
    return counts[key]


def scene_to_bytes(scene: Scene) -> bytes:
    """Serialise ``scene`` to the IR byte layout."""
    counts = {slot: int(scene.counts.get(slot, 0)) for slot in COUNT_SLOTS}
    num_net_classes = int(scene.num_net_classes)

    payloads: list[tuple[int, int, int, bytes]] = []
    for definition in SECTION_DEFS:
        count = _section_count(definition.count, counts, num_net_classes)
        if count == 0:
            continue
        array = scene.arrays.get(definition.name)
        if array is None:
            raise SceneFormatError(f"scene is missing the '{definition.name}' array")
        array = np.ascontiguousarray(array, dtype=definition.dtype)
        if len(array) != count:
            raise SceneFormatError(
                f"array '{definition.name}' has {len(array)} elements, expected {count}"
            )
        payloads.append((int(definition.section), count, array.dtype.itemsize, array.tobytes()))

    payloads.append((int(Section.JSON_TAIL), len(TAIL), 1, scene.tail or TAIL))
    board_blob = scene.board.to_bytes()
    payloads.append((int(Section.BOARD_CONFIG), 1, len(board_blob), board_blob))

    if len(payloads) > MAX_SECTIONS:
        raise SceneFormatError(f"too many sections ({len(payloads)})")
    nsec = len(payloads)

    header = struct.pack(
        "<4sHHIIII",
        MAGIC,
        VERSION_MAJOR,
        VERSION_MINOR,
        FLAG_LITTLE_ENDIAN,
        HEADER_SIZE,
        nsec,
        0,
    )
    header += struct.pack("<10I", *(counts[slot] for slot in COUNT_SLOTS))
    header += struct.pack("<I", num_net_classes)
    header += struct.pack("<13I", *([0] * 13))
    header += struct.pack("<Q", 0)
    assert len(header) == HEADER_SIZE

    offset = _align_up(HEADER_SIZE + nsec * SECTION_ENTRY_SIZE, ALIGNMENT)
    table = bytearray()
    for kind, count, elem_size, blob in payloads:
        table += struct.pack("<IIIIQQ", kind, offset, count, elem_size, 0, 0)
        offset = _align_up(offset + len(blob), ALIGNMENT)

    out = bytearray(header)
    out += table
    for _kind, _count, _elem_size, blob in payloads:
        if len(out) % ALIGNMENT:
            out += b"\x00" * (ALIGNMENT - len(out) % ALIGNMENT)
        out += blob
    return bytes(out)


def write_scene(path: str, scene: Scene) -> None:
    """Serialise ``scene`` to ``path``."""
    with open(path, "wb") as handle:
        handle.write(scene_to_bytes(scene))


# ---------------------------------------------------------------------------
# Builder
# ---------------------------------------------------------------------------


@dataclass
class SceneBuilder:
    """Accumulates a scene column-wise, mirroring the C push API.

    Every column is a plain list during construction and becomes a numpy array
    in :meth:`build`, so the producer never allocates per element.
    """

    board: BoardConfig = field(default_factory=BoardConfig)
    degraded: int = 0
    refs: list[str] = field(default_factory=list)  # reporting only, not serialised

    def __post_init__(self) -> None:
        self._columns: dict[str, list] = {d.name: [] for d in SECTION_DEFS}
        # POLY_OFFSETS is a CSR prefix: one leading zero, then one entry per
        # polygon holding the cumulative vertex count.
        self._columns["poly.offsets"] = [0]
        self._num_net_classes = 0

    # --- construction -----------------------------------------------------

    def add_component(
        self,
        x: float,
        y: float,
        *,
        half_w: float = 0.0,
        half_h: float = 0.0,
        height: float = 0.0,
        mass: float = 0.0,
        flags: int = 0,
        kind: int = 0,
        polygon_id: int = NO_ID,
        ref: str = "",
        part_id: int = 0,
    ) -> int:
        col = self._columns
        comp = len(col["comps.x"])
        col["comps.x"].append(float(x))
        col["comps.y"].append(float(y))
        col["comps.half_w"].append(float(half_w))
        col["comps.half_h"].append(float(half_h))
        col["comps.height"].append(float(height))
        col["comps.mass"].append(float(mass))
        col["comps.flags"].append(int(flags))
        col["comps.kind"].append(int(kind))
        col["comps.part_id"].append(int(part_id) & 0xFFFFFFFF)
        col["comps.pin_count"].append(0)
        col["comps.polygon_id"].append(int(polygon_id))
        self.refs.append(ref)
        return comp

    def add_pin(
        self,
        comp: int,
        offset_x: float,
        offset_y: float,
        *,
        net: int = NO_ID,
        flags: int = 0,
        max_current: float = 0.0,
        half_x: float = 0.0,
        half_y: float = 0.0,
    ) -> int:
        col = self._columns
        pin = len(col["pins.offset_x"])
        col["pins.offset_x"].append(float(offset_x))
        col["pins.offset_y"].append(float(offset_y))
        col["pins.comp_id"].append(int(comp))
        col["pins.net_id"].append(int(net))
        col["pins.flags"].append(int(flags))
        col["pins.max_current"].append(float(max_current))
        col["pins.half_x"].append(float(half_x))
        col["pins.half_y"].append(float(half_y))
        col["comps.pin_count"][comp] += 1
        return pin

    def add_net(self, *, weight: float = 1.0, net_class: int = 0) -> int:
        col = self._columns
        net = len(col["nets.weights"])
        col["nets.weights"].append(float(weight))
        col["nets.net_class"].append(int(net_class))
        return net

    def connect(self, pin: int, net: int) -> None:
        current = self._columns["pins.net_id"][pin]
        if current != NO_ID:
            raise SceneFormatError(f"pin {pin} is already connected to net {current}")
        self._columns["pins.net_id"][pin] = int(net)

    def add_polygon(self, kind: int, xy, *, layer_mask: int = 0) -> int:
        col = self._columns
        points = list(xy)
        if len(points) < 3:
            raise SceneFormatError("a polygon needs at least 3 vertices")
        poly = len(col["poly.kind"])
        for x, y in points:
            col["poly.vx"].append(float(x))
            col["poly.vy"].append(float(y))
        col["poly.offsets"].append(len(col["poly.vx"]))
        col["poly.kind"].append(int(kind))
        col["poly.layer_mask"].append(int(layer_mask))
        return poly

    def add_decoupling(
        self, ic_comp: int, cap_comp: int, ic_pin: int, max_dist_mm: float, weight: float = 1.0
    ) -> None:
        col = self._columns
        col["decoupling.ic_comp"].append(int(ic_comp))
        col["decoupling.cap_comp"].append(int(cap_comp))
        col["decoupling.ic_pin"].append(int(ic_pin))
        col["decoupling.max_dist_sq"].append(float(max_dist_mm) ** 2)
        col["decoupling.weight"].append(float(weight))

    def add_thermal(
        self, comp: int, power_w: float, exclusion_radius_mm: float, r_theta_ja: float = 0.0
    ) -> None:
        col = self._columns
        col["thermal.comp"].append(int(comp))
        col["thermal.power_w"].append(float(power_w))
        col["thermal.exclusion_radius"].append(float(exclusion_radius_mm))
        col["thermal.r_theta_ja"].append(float(r_theta_ja))

    def add_symmetry(self, comp_a: int, comp_b: int, axis: int, weight: float = 1.0) -> None:
        col = self._columns
        col["symmetry.comp_a"].append(int(comp_a))
        col["symmetry.comp_b"].append(int(comp_b))
        col["symmetry.axis"].append(int(axis))
        col["symmetry.weight"].append(float(weight))

    def add_diffpair(self, net_p: int, net_n: int, max_skew_mm: float = 0.0) -> None:
        col = self._columns
        col["diffpairs.net_p"].append(int(net_p))
        col["diffpairs.net_n"].append(int(net_n))
        col["diffpairs.max_skew_mm"].append(float(max_skew_mm))

    def set_clearance_matrix(self, clearances: list[list[float]]) -> None:
        """Square, row-major, symmetric matrix of per-class clearances (mm)."""
        n = len(clearances)
        for row in clearances:
            if len(row) != n:
                raise SceneFormatError("the clearance matrix must be square")
        flat = [float(value) for row in clearances for value in row]
        self._columns["rules.class_clearance"] = flat
        self._num_net_classes = n

    # --- output -----------------------------------------------------------

    def counts(self) -> dict[str, int]:
        col = self._columns
        return {
            "comps": len(col["comps.x"]),
            "pins": len(col["pins.offset_x"]),
            "nets": len(col["nets.weights"]),
            "net_entries": sum(1 for net in col["pins.net_id"] if net != NO_ID),
            "polygons": len(col["poly.kind"]),
            "vertices": len(col["poly.vx"]),
            "decoupling": len(col["decoupling.ic_comp"]),
            "thermal": len(col["thermal.comp"]),
            "symmetry": len(col["symmetry.comp_a"]),
            "diffpairs": len(col["diffpairs.net_p"]),
        }

    def build(self) -> Scene:
        counts = self.counts()
        arrays: dict[str, np.ndarray] = {}
        for definition in SECTION_DEFS:
            count = _section_count(definition.count, counts, self._num_net_classes)
            if count == 0:
                continue
            column = self._columns[definition.name]
            if len(column) != count:
                raise SceneFormatError(
                    f"column '{definition.name}' has {len(column)} values, expected {count}"
                )
            arrays[definition.name] = np.asarray(column, dtype=definition.dtype)

        board = self.board
        board.degraded_mask = self.degraded
        if board.degraded_mask & ~DEGRADED_ALL_MASK:
            raise SceneFormatError("unknown degraded-mode bit")
        return Scene(
            counts=counts,
            num_net_classes=self._num_net_classes,
            arrays=arrays,
            board=board,
            tail=TAIL,
        )
