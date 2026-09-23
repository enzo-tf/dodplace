"""Run the pcbnew extractor under KiCad's own Python interpreter.

KiCad ships a Python that can import ``pcbnew`` (3.9 on macOS, a system Python
elsewhere). It is a different interpreter from ours, so the extractor is a
standalone script and the only thing crossing the boundary is JSON.

Why not the IPC API for extraction? Verified against KiCad 10.0.5: the IPC
surface has no board-outline helper, exposes courtyards and Edge.Cuts only as
raw segments (arcs included), and hides pad sizes inside a padstack. pcbnew
already returns *chained* geometry (``GetCourtyard``, ``GetBoardPolygonOutlines``)
from KiCad's own parser, headless. Reimplementing that chaining over IPC is the
format drift the IPC API exists to avoid, so IPC is reserved for what only it
can do: saving the open board and applying a placement live (phase 4).
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Iterator

from .schema import validate

#: A wxWidgets startup assertion is printed on every run; drop it from reports.
_NOISE = ("stdpbase.cpp", "wxApp")


class HeadlessUnavailable(RuntimeError):
    """No Python with ``pcbnew`` could be found."""


class HeadlessError(RuntimeError):
    """The extractor ran but failed."""


def _macos_frameworks() -> list[str]:
    base = Path("/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions")
    if not base.is_dir():
        return []
    versions = [
        entry
        for entry in base.iterdir()
        if entry.is_dir() and entry.name[0].isdigit()
    ]
    versions.sort(key=lambda p: [int(part) for part in p.name.split(".")], reverse=True)
    return [str(version / "bin" / "python3") for version in versions]


def candidate_interpreters() -> Iterator[str]:
    override = os.environ.get("DODPLACE_KICAD_PYTHON")
    if override:
        yield override
    yield from _macos_frameworks()
    for name in ("python3", "python"):
        found = shutil.which(name)
        if found:
            yield found


_probe_cache: dict[str, bool] = {}


def _can_import_pcbnew(python: str) -> bool:
    if python in _probe_cache:
        return _probe_cache[python]
    env = {**os.environ, "PYTHONDONTWRITEBYTECODE": "1"}
    try:
        proc = subprocess.run(
            [python, "-c", "import pcbnew"],
            capture_output=True,
            timeout=120,
            env=env,
        )
        ok = proc.returncode == 0
    except (OSError, subprocess.SubprocessError):
        ok = False
    _probe_cache[python] = ok
    return ok


def find_kicad_python() -> str:
    """First interpreter that can import ``pcbnew``."""
    tried = []
    for candidate in candidate_interpreters():
        if not Path(candidate).exists():
            continue
        tried.append(candidate)
        if _can_import_pcbnew(candidate):
            return candidate
    raise HeadlessUnavailable(
        "no Python with pcbnew found (tried: %s). Set DODPLACE_KICAD_PYTHON."
        % ", ".join(tried) if tried else "nothing on PATH"
    )


def _meaningful_stderr(stderr: str) -> str:
    lines = [line for line in stderr.splitlines() if line.strip()]
    lines = [line for line in lines if not any(noise in line for noise in _NOISE)]
    return "\n".join(lines[-8:]) if lines else "no diagnostics"


def extractor_script() -> Path:
    return Path(__file__).with_name("pcbnew_extract.py")


def extract_board_headless(board_path: str, *, timeout: int = 600) -> dict:
    """Extract ``board_path`` with KiCad's Python and return the extract dict."""
    if not Path(board_path).is_file():
        raise HeadlessError(f"board file not found: {board_path}")

    python = find_kicad_python()
    with tempfile.TemporaryDirectory(prefix="dodplace-extract-") as tmp:
        output = Path(tmp) / "extract.json"
        env = {**os.environ, "PYTHONDONTWRITEBYTECODE": "1"}
        try:
            proc = subprocess.run(
                [python, str(extractor_script()), str(board_path), str(output)],
                capture_output=True,
                text=True,
                timeout=timeout,
                env=env,
            )
        except subprocess.TimeoutExpired as exc:
            raise HeadlessError(f"extraction timed out after {timeout}s") from exc
        if proc.returncode != 0 or not output.is_file():
            raise HeadlessError(
                f"pcbnew extraction failed (rc={proc.returncode}):\n"
                f"{_meaningful_stderr(proc.stderr)}"
            )
        doc = json.loads(output.read_text())

    validate(doc)
    return doc


def kicad_python_version() -> str:
    """Best effort: the version of the interpreter that would be used."""
    try:
        python = find_kicad_python()
    except HeadlessUnavailable as exc:
        return str(exc)
    proc = subprocess.run(
        [python, "-c", "import sys; print(sys.version.split()[0])"],
        capture_output=True,
        text=True,
    )
    return proc.stdout.strip() or "unknown"


if __name__ == "__main__":  # pragma: no cover - convenience probe
    print("interpreter:", find_kicad_python())
    print("python:", kicad_python_version())
    sys.exit(0)
