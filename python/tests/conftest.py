"""Shared fixtures: repo paths, the committed test board and its extract."""

from __future__ import annotations

import json
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
FIXTURES = Path(__file__).resolve().parent / "fixtures"

SOLVER_CANDIDATES = ("build/gcc-debug/dodplace-solve", "build/gcc-release/dodplace-solve")


@pytest.fixture(scope="session")
def repo_root() -> Path:
    return ROOT


@pytest.fixture(scope="session")
def fixture_board() -> Path:
    return FIXTURES / "scene_fixture.kicad_pcb"


@pytest.fixture(scope="session")
def committed_extract() -> dict:
    """The extract of the fixture board, frozen at commit time.

    It makes the IR tests hermetic (no KiCad needed) and pins the exact data the
    writer was validated against.
    """
    return json.loads((FIXTURES / "scene_fixture.extract.json").read_text())


@pytest.fixture(scope="session")
def golden_scene() -> Path:
    """The C-written golden scene fixture (ir/fixtures/scene_minimal.bin)."""
    return ROOT / "ir" / "fixtures" / "scene_minimal.bin"


@pytest.fixture(scope="session")
def solver_binary() -> Path:
    for relative in SOLVER_CANDIDATES:
        candidate = ROOT / relative
        if candidate.is_file():
            return candidate
    pytest.skip("dodplace-solve is not built; run cmake --build --preset gcc-debug")


def normalize_extract(doc: dict) -> dict:
    """Drop what legitimately varies between machines and KiCad builds."""
    normalized = json.loads(json.dumps(doc))
    board = normalized.get("board", {})
    if board.get("path"):
        board["path"] = Path(board["path"]).name
    normalized.pop("backend_version", None)
    return normalized
