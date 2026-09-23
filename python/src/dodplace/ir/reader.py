"""Decode a ``scene.bin`` into numpy arrays."""

from __future__ import annotations

import struct
from dataclasses import dataclass, field

import numpy as np

from .spec import (
    BOARD_CONFIG_SIZE,
    COUNT_SLOTS,
    FLAG_LITTLE_ENDIAN,
    HEADER_SIZE,
    MAGIC,
    SECTION_DEFS,
    SECTION_ENTRY_SIZE,
    VERSION_MAJOR,
    BoardConfig,
    Section,
)

#: kind value -> definition, for the fast path in the table loop.
_SECTION_BY_VALUE = {int(d.section): d for d in SECTION_DEFS}


class SceneFormatError(ValueError):
    """The file is not a well-formed dodplace scene."""


@dataclass
class Scene:
    """A decoded scene: raw arrays plus everything the header carries."""

    counts: dict[str, int]
    num_net_classes: int
    arrays: dict[str, np.ndarray] = field(default_factory=dict)
    board: BoardConfig = field(default_factory=BoardConfig)
    tail: bytes = b""
    version: tuple[int, int] = (VERSION_MAJOR, 0)


def _read_header(blob: bytes) -> tuple[dict[str, int], int, int, int]:
    if len(blob) < HEADER_SIZE:
        raise SceneFormatError("file is smaller than the scene header")
    magic, ver_major, ver_minor, flags, header_size, nsec, _res0 = struct.unpack_from(
        "<4sHHIIII", blob, 0
    )
    if magic != MAGIC:
        raise SceneFormatError("bad magic: not a dodplace scene file")
    if ver_major != VERSION_MAJOR:
        raise SceneFormatError(f"unsupported IR major version {ver_major}")
    if header_size != HEADER_SIZE:
        raise SceneFormatError(f"unexpected header size {header_size}")
    if not flags & FLAG_LITTLE_ENDIAN:
        raise SceneFormatError("file does not declare little-endian layout")
    if not 0 < nsec <= 256:
        raise SceneFormatError(f"invalid section count {nsec}")

    slots = struct.unpack_from("<10I", blob, 24)
    counts = dict(zip(COUNT_SLOTS, slots))
    (num_net_classes,) = struct.unpack_from("<I", blob, 64)
    return counts, num_net_classes, nsec, ver_minor


def read_scene(path: str) -> Scene:
    """Read ``path`` and return a :class:`Scene` with writable numpy arrays."""
    with open(path, "rb") as handle:
        blob = handle.read()

    counts, num_net_classes, nsec, ver_minor = _read_header(blob)
    scene = Scene(counts=counts, num_net_classes=num_net_classes, version=(VERSION_MAJOR, ver_minor))

    table_end = HEADER_SIZE + nsec * SECTION_ENTRY_SIZE
    if len(blob) < table_end:
        raise SceneFormatError("truncated section table")

    seen: set[int] = set()
    for i in range(nsec):
        kind, offset, count, elem_size = struct.unpack_from("<IIII", blob, HEADER_SIZE + i * 32)
        if kind in seen:
            raise SceneFormatError(f"duplicate section kind {kind}")
        seen.add(kind)
        if offset % 64 != 0:
            raise SceneFormatError(f"section {kind} offset is not 64-byte aligned")
        end = offset + count * elem_size
        if elem_size == 0 or end > len(blob):
            raise SceneFormatError(f"section {kind} payload falls outside the file")

        if kind == int(Section.JSON_TAIL):
            scene.tail = blob[offset:end]
            continue
        if kind == int(Section.BOARD_CONFIG):
            if count != 1 or elem_size != BOARD_CONFIG_SIZE:
                raise SceneFormatError("board configuration section has the wrong layout")
            scene.board = BoardConfig.from_bytes(blob[offset:end])
            continue

        definition = _SECTION_BY_VALUE.get(kind)
        if definition is None:
            continue  # unknown kinds are skipped, per the spec
        if elem_size != np.dtype(definition.dtype).itemsize:
            raise SceneFormatError(
                f"section {definition.name} has element size {elem_size}, "
                f"expected {np.dtype(definition.dtype).itemsize}"
            )
        array = np.frombuffer(blob, dtype=definition.dtype, count=count, offset=offset).copy()
        scene.arrays[definition.name] = array

    if int(Section.BOARD_CONFIG) not in seen:
        raise SceneFormatError("board configuration section is missing")
    return scene
