"""KiCad board extraction: board file in, extract document out.

The extract document is the ONLY contract between the board-reading backends
and the IR conversion. Today one backend produces it (``pcbnew`` headless, see
the module docstring in ``headless.py`` for why); an IPC backend or a
schematic-only source can be added without touching ``to_scene``.
"""

from __future__ import annotations

from .headless import (
    HeadlessError,
    HeadlessUnavailable,
    extract_board_headless,
    find_kicad_python,
    kicad_python_version,
)
from .schema import SCHEMA, ExtractError, validate
from .to_scene import IngestReport, IngestResult, build_scene, pad_bbox_cycle

#: Extraction backends. "auto" resolves to the only available one.
BACKENDS = ("auto", "pcbnew")


def extract_board(board_path: str, backend: str = "auto") -> dict:
    """Read ``board_path`` and return a validated extract document."""
    if backend not in BACKENDS:
        raise ValueError(
            f"unknown extraction backend {backend!r}; available: {', '.join(BACKENDS)}"
        )
    return extract_board_headless(board_path)


__all__ = [
    "BACKENDS",
    "ExtractError",
    "HeadlessError",
    "HeadlessUnavailable",
    "IngestReport",
    "IngestResult",
    "SCHEMA",
    "build_scene",
    "extract_board",
    "extract_board_headless",
    "find_kicad_python",
    "kicad_python_version",
    "pad_bbox_cycle",
    "validate",
]
