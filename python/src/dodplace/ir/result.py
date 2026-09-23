"""Read and write ``placement.bin`` (ir/spec.md section 9)."""

from __future__ import annotations

import struct
from dataclasses import dataclass

import numpy as np

from .spec import (
    FLAG_LITTLE_ENDIAN,
    RESULT_MAGIC,
    VERSION_MAJOR,
    VERSION_MINOR,
)

HEADER_FMT = "<4sHHIIIIII"  # magic, major, minor, flags, count, entry_size, 3x reserved
HEADER_SIZE = struct.calcsize(HEADER_FMT)

ENTRY_DTYPE = np.dtype(
    [
        ("x", "<f4"),
        ("y", "<f4"),
        ("orient", "u1"),
        ("side", "u1"),
        ("flags", "u1"),
        ("reserved", "u1"),
    ]
)

assert ENTRY_DTYPE.itemsize == 12, "placement record layout is part of the IR"


class ResultFormatError(ValueError):
    """The file is not a well-formed placement result."""


@dataclass
class Placement:
    """Decoded placement: one record per component, in component-id order."""

    entries: np.ndarray

    @property
    def count(self) -> int:
        return len(self.entries)

    @property
    def x(self) -> np.ndarray:
        return self.entries["x"]

    @property
    def y(self) -> np.ndarray:
        return self.entries["y"]

    @property
    def orient(self) -> np.ndarray:
        return self.entries["orient"]

    @property
    def side(self) -> np.ndarray:
        return self.entries["side"]

    @property
    def flags(self) -> np.ndarray:
        return self.entries["flags"]

    def moved_mask(self) -> np.ndarray:
        from .spec import PLACEMENT_FLAG_MOVED

        return (self.flags & PLACEMENT_FLAG_MOVED) != 0


def read_placement(path: str) -> Placement:
    with open(path, "rb") as handle:
        blob = handle.read()
    if len(blob) < HEADER_SIZE:
        raise ResultFormatError("file is smaller than the result header")
    magic, major, _minor, flags, count, entry_size, *_reserved = struct.unpack_from(
        HEADER_FMT, blob, 0
    )
    if magic != RESULT_MAGIC:
        raise ResultFormatError("bad magic: not a dodplace result file")
    if major != VERSION_MAJOR:
        raise ResultFormatError(f"unsupported result major version {major}")
    if not flags & FLAG_LITTLE_ENDIAN:
        raise ResultFormatError("file does not declare little-endian layout")
    if entry_size != ENTRY_DTYPE.itemsize:
        raise ResultFormatError(f"unexpected entry size {entry_size}")
    expected = HEADER_SIZE + count * ENTRY_DTYPE.itemsize
    if len(blob) < expected:
        raise ResultFormatError("truncated placement records")
    entries = np.frombuffer(blob, dtype=ENTRY_DTYPE, count=count, offset=HEADER_SIZE).copy()
    return Placement(entries=entries)


def write_placement(path: str, entries: np.ndarray) -> None:
    """Write ``entries`` (an ENTRY_DTYPE array) to ``path``."""
    array = np.ascontiguousarray(entries, dtype=ENTRY_DTYPE)
    header = struct.pack(
        HEADER_FMT,
        RESULT_MAGIC,
        VERSION_MAJOR,
        VERSION_MINOR,
        FLAG_LITTLE_ENDIAN,
        len(array),
        ENTRY_DTYPE.itemsize,
        0,
        0,
        0,
    )
    with open(path, "wb") as handle:
        handle.write(header)
        handle.write(array.tobytes())
