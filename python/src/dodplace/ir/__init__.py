"""Scene IR: decode and encode ``scene.bin`` (see ``ir/spec.md``)."""

from __future__ import annotations

from .spec import (
    ALIGNMENT,
    BOARD_CONFIG_SIZE,
    COUNT_SLOTS,
    FLAG_LITTLE_ENDIAN,
    HEADER_SIZE,
    MAGIC,
    MAX_SECTIONS,
    NO_ID,
    SECTION_BY_KIND,
    SECTION_DEFS,
    SECTION_ENTRY_SIZE,
    TAIL,
    VERSION_MAJOR,
    VERSION_MINOR,
    BoardConfig,
    Section,
    degraded_names,
)
from .reader import Scene, read_scene
from .result import Placement, read_placement, write_placement
from .writer import SceneBuilder, scene_to_bytes, write_scene

__all__ = [
    "ALIGNMENT",
    "BOARD_CONFIG_SIZE",
    "COUNT_SLOTS",
    "FLAG_LITTLE_ENDIAN",
    "HEADER_SIZE",
    "MAGIC",
    "MAX_SECTIONS",
    "NO_ID",
    "SECTION_BY_KIND",
    "SECTION_DEFS",
    "SECTION_ENTRY_SIZE",
    "TAIL",
    "VERSION_MAJOR",
    "VERSION_MINOR",
    "BoardConfig",
    "Placement",
    "Scene",
    "SceneBuilder",
    "Section",
    "degraded_names",
    "read_placement",
    "read_scene",
    "scene_to_bytes",
    "write_placement",
    "write_scene",
]
