"""STEP models: part height from the board's own 3D model references.

No OCCT, no 500 MB wheel. A STEP file is a text format and every vertex of the
geometry is a ``CARTESIAN_POINT`` entity; for a bounding box that is enough,
because the convex hull of a B-spline's control points contains its surface, so
the box computed from *all* points can only over-estimate. Over-estimating a
height is the safe direction for a z-interference check.

What this cannot give is mass (no volume without a kernel) or the pad-to-model
alignment, so those stay unresolved. The optional ``[step]`` extra can refine
this later; the interface will not change.
"""

from __future__ import annotations

import math
import os
import re
from dataclasses import dataclass
from pathlib import Path

#: CARTESIAN_POINT('',(1.0, 2.0, 3.0)) with any whitespace and exponent style.
_POINT = re.compile(
    r"CARTESIAN_POINT\s*\([^,]*,\s*\(\s*"
    r"([-+0-9.eE]+)\s*,\s*([-+0-9.eE]+)\s*,\s*([-+0-9.eE]+)\s*\)\s*\)"
)

#: Extensions that carry measurable geometry.
STEP_SUFFIXES = (".step", ".stp")

#: Extensions a footprint may reference for the 3D viewer.
MODEL_SUFFIXES = (".step", ".stp", ".wrl", ".wrz", ".igs", ".iges")

#: Where KiCad keeps its official 3D models, by platform.
DEFAULT_MODEL_ROOTS = (
    "/Applications/KiCad/KiCad.app/Contents/SharedSupport/3dmodels",
    "/usr/share/kicad/3dmodels",
    "/usr/local/share/kicad/3dmodels",
    str(Path.home() / "Library/Application Support/kicad/3dmodels"),
)


class StepModelError(ValueError):
    """The model file cannot be read."""


@dataclass
class StepBBox:
    min: tuple[float, float, float]
    max: tuple[float, float, float]

    @property
    def size(self) -> tuple[float, float, float]:
        return tuple(hi - lo for lo, hi in zip(self.min, self.max))  # type: ignore[return-value]

    @property
    def height_mm(self) -> float:
        return self.max[2] - self.min[2]


def read_bbox(path: str | Path) -> StepBBox:
    """Bounding box of every cartesian point in the file."""
    path = Path(path)
    try:
        text = path.read_text(errors="ignore")
    except OSError as exc:
        raise StepModelError(f"cannot read {path}: {exc}") from exc

    lows = [math.inf, math.inf, math.inf]
    highs = [-math.inf, -math.inf, -math.inf]
    count = 0
    for match in _POINT.finditer(text):
        count += 1
        for axis in range(3):
            value = float(match.group(axis + 1))
            if value < lows[axis]:
                lows[axis] = value
            if value > highs[axis]:
                highs[axis] = value
    if count == 0:
        raise StepModelError(f"{path.name}: no CARTESIAN_POINT found (not a STEP file?)")
    return StepBBox(min=(lows[0], lows[1], lows[2]), max=(highs[0], highs[1], highs[2]))


def height_mm(path: str | Path) -> float:
    """Z extent of the model, in mm (STEP models are authored in mm, Z up)."""
    return read_bbox(path).height_mm


def _step_candidates(raw: str):
    """Model references to try, best first.

    Custom libraries reference the VRML model for the 3D viewer and ship a STEP
    file with the same name next to it (verified on a real board: 18 of 18 pairs
    complete). VRML is what the viewer wants; STEP is what can be measured, so
    the sibling is tried first and the reference itself only when it already
    points at a STEP file.
    """
    lowered = raw.lower()
    if lowered.endswith(STEP_SUFFIXES):
        yield raw
        return
    if lowered.endswith(MODEL_SUFFIXES):
        stem = raw[: -len(raw.rsplit(".", 1)[-1]) - 1]
        for suffix in STEP_SUFFIXES:
            yield stem + suffix
        return
    yield raw  # no extension to reason about: try it as given


def resolve_step_path(raw: str, board_path: str | Path) -> Path | None:
    """A readable STEP file for a footprint's model reference, or None.

    Returns None rather than a VRML path: a file we cannot measure is worse than
    an unresolved height, because the degraded mode says so out loud.
    """
    for candidate in _step_candidates(raw):
        path = resolve_model_path(candidate, board_path)
        if path is not None:
            return path
    return None


def model_roots() -> list[Path]:
    """KiCad's 3D model directories that actually exist on this machine."""
    roots = [Path(os.environ[name]) for name in os.environ if name.startswith("KICAD") and
             "3DMODEL" in name.upper()]
    roots += [Path(path) for path in DEFAULT_MODEL_ROOTS]
    return [root for root in roots if root.is_dir()]


def resolve_model_path(raw: str, board_path: str | Path) -> Path | None:
    """Turn a footprint's model reference into an existing file.

    Handles the three shapes seen in real boards:

    * ``${KIPRJMOD}/...`` - relative to the project directory
    * ``${KICAD10_3DMODEL_DIR}/...`` - an environment variable pcbnew would have
      expanded, resolved here against the known install locations
    * a plain absolute path
    """
    if not raw:
        return None
    board_dir = Path(board_path).resolve().parent

    if raw.startswith("${") and "}" in raw:
        variable, _, remainder = raw[2:].partition("}")
        remainder = remainder.lstrip("/")
        if variable == "KIPRJMOD":
            candidate = board_dir / remainder
            return candidate if candidate.is_file() else None
        value = os.environ.get(variable)
        if value:
            candidate = Path(value) / remainder
            if candidate.is_file():
                return candidate
        for root in model_roots():
            candidate = root / remainder
            if candidate.is_file():
                return candidate
        return None

    candidate = Path(raw)
    if not candidate.is_absolute():
        candidate = board_dir / candidate
    return candidate if candidate.is_file() else None
