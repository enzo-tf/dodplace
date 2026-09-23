"""Finding and running the C engine (`dodplace-solve`).

Shared by the CLI and the KiCad plugin action so both resolve the binary the
same way: explicit flag, then environment, then the usual build directories of
this checkout, then PATH.
"""

from __future__ import annotations

import os
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path

#: Build directories to probe, relative to the repository root.
BUILD_CANDIDATES = (
    "build/gcc-debug/dodplace-solve",
    "build/gcc-release/dodplace-solve",
    "build/dodplace-solve",
)


def repo_root() -> Path:
    """The checkout this package lives in, when it is a source checkout."""
    return Path(__file__).resolve().parents[3]


def find_solver(explicit: str | Path | None = None) -> Path | None:
    if explicit:
        candidate = Path(explicit)
        return candidate if candidate.is_file() else None
    override = os.environ.get("DODPLACE_SOLVER")
    if override:
        candidate = Path(override)
        if candidate.is_file():
            return candidate
    for relative in BUILD_CANDIDATES:
        candidate = repo_root() / relative
        if candidate.is_file():
            return candidate
    found = shutil.which("dodplace-solve")
    return Path(found) if found else None


@dataclass
class SolverRun:
    command: list[str]
    returncode: int
    stdout: str
    stderr: str

    @property
    def ok(self) -> bool:
        return self.returncode == 0


def run_solver(
    scene_path: str | Path,
    *,
    solver: str | Path | None = None,
    placement_path: str | Path | None = None,
    identity: bool = False,
    timeout: int = 600,
) -> SolverRun:
    """Run the engine on a scene. Raises FileNotFoundError if it is not built."""
    binary = find_solver(solver)
    if binary is None:
        raise FileNotFoundError(
            "dodplace-solve was not found; build it with "
            "`cmake --build --preset gcc-debug` or pass --solver PATH"
        )
    command = [str(binary), str(scene_path)]
    if placement_path is not None:
        command += ["--identity" if identity else "--identity", "-o", str(placement_path)]
    elif identity:
        command += ["--identity"]
    try:
        completed = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired as exc:
        raise TimeoutError(f"the engine timed out after {timeout}s") from exc
    return SolverRun(command, completed.returncode, completed.stdout, completed.stderr)
