"""Apply a solver placement to a board and write a legalised copy.

The solver works in the *courtyard-centre* frame: at load the engine rebases
every component so that its ``(x, y)`` is the geometric centre of the courtyard
(``finalize_courtyards``), while KiCad places a footprint by its origin. Rather
than rebuild that centre here - and risk drifting from the engine's convention -
the shim applies the **pure movement**:

    delta        = placement.x/y - scene.comps.x/y
    new origin   = extract origin + delta
    new rotation = extract rotation + (placement.orient - scene.orient) * 90

The rotation is a delta for the same reason: the sign convention of a footprint
on the bottom layer, and any angle that is not a multiple of ninety, survive
because an absolute orientation is never written.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

from ..ir.reader import read_scene
from ..ir.result import read_placement
from .headless import HeadlessError, find_kicad_python

SCHEMA = "dodplace.apply/1"

# Placement flags (ir/spec.md section 9).
FLAG_LOCKED = 0x01
FLAG_MOVED = 0x02

# Scene component flags (ir/spec.py COMP_*).
COMP_LOCKED = 0x08


class ApplyError(RuntimeError):
    """The placement cannot be applied to this board."""


@dataclass
class Update:
    """One footprint to move, in the board's own coordinates."""

    ref: str
    x: float
    y: float
    rot_deg: float
    dx: float
    dy: float
    drot: float
    flags: int

    def as_json(self) -> dict:
        return {
            "ref": self.ref,
            "x": round(self.x, 6),
            "y": round(self.y, 6),
            "rot_deg": round(self.rot_deg, 6),
            "dx": round(self.dx, 6),
            "dy": round(self.dy, 6),
            "drot": self.drot,
            "flags": self.flags,
        }


def _orient(flags: int) -> int:
    return int(flags) & 0x03


def build_updates(scene_path: str, placement_path: str, extract: dict,
                  moved_only: bool = True) -> list[Update]:
    """The movements to apply, in the board's coordinates.

    Raises ApplyError when the scene and the board disagree about how many
    components there are: that means the scene is not this board's, and moving
    footprints on the strength of it would scramble the layout.
    """
    scene = read_scene(scene_path)
    placement = read_placement(placement_path)
    comps = extract.get("components") or []

    n_scene = int(scene.counts.get("comps", 0))
    if n_scene != len(comps):
        raise ApplyError(
            f"scene has {n_scene} components, the board has {len(comps)} footprints; "
            "the scene is not this board's (re-run ingest)"
        )
    if placement.count != n_scene:
        raise ApplyError(
            f"placement has {placement.count} records for {n_scene} components"
        )

    a = scene.arrays
    sx, sy = a["comps.x"], a["comps.y"]
    sflags = a["comps.flags"]

    updates: list[Update] = []
    for i, comp in enumerate(comps):
        flags = int(placement.flags[i])
        if int(sflags[i]) & COMP_LOCKED:
            continue  # the designer's part: the solver never moved it, never write it
        if moved_only and not (flags & FLAG_MOVED):
            continue
        dx = float(placement.x[i]) - float(sx[i])
        dy = float(placement.y[i]) - float(sy[i])
        drot = (_orient(placement.orient[i]) - _orient(sflags[i])) * 90
        if moved_only and dx == 0.0 and dy == 0.0 and drot == 0:
            continue
        updates.append(
            Update(
                ref=str(comp.get("ref", "")),
                x=float(comp["x"]) + dx,
                y=float(comp["y"]) + dy,
                rot_deg=float(comp.get("rot_deg", 0.0)) + drot,
                dx=dx,
                dy=dy,
                drot=drot,
                flags=flags,
            )
        )
    return updates


def apply_placement_headless(board_path: str, updates: list[Update], out_path: str,
                             *, timeout: int = 600) -> int:
    """Move the footprints with KiCad's Python and save the copy. Returns the count."""
    if not Path(board_path).is_file():
        raise ApplyError(f"board file not found: {board_path}")
    python = find_kicad_python()
    script = Path(__file__).with_name("pcbnew_apply.py")
    payload = {"schema": SCHEMA, "updates": [u.as_json() for u in updates]}

    with tempfile.TemporaryDirectory(prefix="dodplace-apply-") as tmp:
        updates_path = Path(tmp) / "updates.json"
        updates_path.write_text(json.dumps(payload), encoding="utf-8")
        env = {**os.environ, "PYTHONDONTWRITEBYTECODE": "1"}
        try:
            proc = subprocess.run(
                [python, str(script), str(board_path), str(updates_path), str(out_path)],
                capture_output=True,
                text=True,
                timeout=timeout,
                env=env,
            )
        except subprocess.TimeoutExpired as exc:
            raise HeadlessError(f"applying the placement timed out after {timeout}s") from exc
        if proc.returncode != 0:
            raise ApplyError(
                "pcbnew could not apply the placement (rc=%d):\n%s"
                % (proc.returncode, proc.stderr.strip() or "(no message)")
            )
        try:
            return int(json.loads(proc.stdout.strip().splitlines()[-1])["applied"])
        except (ValueError, KeyError, IndexError):
            return len(updates)


def write_report(path: str, updates: list[Update], scene_path: str, placement_path: str,
                 board_path: str, applied: int) -> None:
    """The audit trail: what moved, by how much, and where it came from."""
    moved = [u for u in updates if u.dx or u.dy or u.drot]
    doc = {
        "schema": "dodplace.apply-report/1",
        "board": str(board_path),
        "scene": str(scene_path),
        "placement": str(placement_path),
        "applied": applied,
        "moved": len(moved),
        "rotated": sum(1 for u in updates if u.drot),
        "components": [u.as_json() for u in moved],
    }
    Path(path).write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8")


def run_drc(board_path: str, *, timeout: int = 900) -> tuple[int, str]:
    """KiCad's own DRC on the produced board: (violations, report text).

    Independent of our geometry: it is the tool's rules reading the tool's file,
    which is the only check that can confirm what a human will see in the GUI.
    """
    exe = "kicad-cli"
    with tempfile.TemporaryDirectory(prefix="dodplace-drc-") as tmp:
        out = Path(tmp) / "drc.txt"
        try:
            proc = subprocess.run(
                [exe, "pcb", "drc", "--exit-code-violations", "-o", str(out), str(board_path)],
                capture_output=True,
                text=True,
                timeout=timeout,
            )
        except FileNotFoundError as exc:
            raise ApplyError("kicad-cli is not on PATH") from exc
        except subprocess.TimeoutExpired as exc:
            raise HeadlessError(f"DRC timed out after {timeout}s") from exc
        report = out.read_text(encoding="utf-8", errors="replace") if out.is_file() else proc.stdout
        # kicad-cli exits 5 when there are violations, 0 when clean.
        return (0 if proc.returncode == 0 else 1), report


def _main(argv: list[str]) -> int:
    sys.stderr.write("apply.py is a library; use `dodplace apply`\n")
    return 2


if __name__ == "__main__":
    sys.exit(_main(sys.argv))
