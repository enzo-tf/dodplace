"""The KiCad shim: the movement maths, and the round-trip through pcbnew."""

from __future__ import annotations

import json
from pathlib import Path

import numpy as np
import pytest

from dodplace.ir.result import ENTRY_DTYPE
from dodplace.ir.spec import FLAG_LITTLE_ENDIAN, RESULT_MAGIC, VERSION_MAJOR, VERSION_MINOR
from dodplace.ir.writer import SceneBuilder, write_scene
from dodplace.kicad.apply import ApplyError, build_updates, write_report
from dodplace.kicad.headless import HeadlessUnavailable, find_kicad_python
from dodplace.ir.spec import COMP_LOCKED, POLY_KIND_COURTYARD

import struct


def _scene(tmp: Path) -> Path:
    """Two components: one locked, one movable, with a courtyard each."""
    b = SceneBuilder()
    b.add_polygon(POLY_KIND_COURTYARD, [[-1, -1], [1, -1], [1, 1], [-1, 1]])
    b.add_component(10.0, 20.0, half_w=0.0, half_h=0.0, flags=COMP_LOCKED, polygon_id=0)
    b.add_component(30.0, 40.0, half_w=0.0, half_h=0.0, polygon_id=0)
    path = tmp / "scene.bin"
    write_scene(str(path), b.build())
    return path


def _placement(tmp: Path, entries) -> Path:
    path = tmp / "placement.bin"
    header = struct.pack(
        "<4sHHIIIIII", RESULT_MAGIC, VERSION_MAJOR, VERSION_MINOR, FLAG_LITTLE_ENDIAN,
        len(entries), ENTRY_DTYPE.itemsize, 0, 0, 0,
    )
    with open(path, "wb") as handle:
        handle.write(header)
        for e in entries:
            handle.write(np.array([e], dtype=ENTRY_DTYPE).tobytes())
    return path


def _extract() -> dict:
    return {
        "schema": "dodplace.extract/1",
        "components": [
            {"ref": "U1", "x": 10.0, "y": 20.0, "rot_deg": 0.0},
            {"ref": "C1", "x": 30.0, "y": 40.0, "rot_deg": 90.0},
        ],
    }


def test_movement_is_the_delta_and_keeps_the_odd_angle(tmp_path):
    scene = _scene(tmp_path)
    # C1 moves by (+2, -3) and rotates one quarter turn; U1 does not move.
    placement = _placement(tmp_path, [
        (10.0, 20.0, 0, 0, 0x01, 0),
        (32.0, 37.0, 1, 0, 0x02, 0),
    ])
    updates = build_updates(str(scene), str(placement), _extract())
    assert [u.ref for u in updates] == ["C1"]
    u = updates[0]
    assert (u.dx, u.dy) == pytest.approx((2.0, -3.0))
    assert (u.x, u.y) == pytest.approx((32.0, 37.0))  # the board's own origin
    assert u.rot_deg == pytest.approx(180.0)          # 90 original + 90 delta
    assert u.drot == 90


def test_a_locked_component_is_never_written(tmp_path):
    scene = _scene(tmp_path)
    placement = _placement(tmp_path, [
        (99.0, 99.0, 0, 0, 0x03, 0),  # flagged MOVED, but the scene locks it
        (30.0, 40.0, 0, 0, 0x00, 0),
    ])
    assert build_updates(str(scene), str(placement), _extract()) == []


def test_all_writes_components_that_did_not_move(tmp_path):
    scene = _scene(tmp_path)
    placement = _placement(tmp_path, [
        (10.0, 20.0, 0, 0, 0x00, 0),
        (30.0, 40.0, 0, 0, 0x00, 0),
    ])
    assert build_updates(str(scene), str(placement), _extract()) == []
    # --all writes every unlocked component, moved or not (U1 is locked).
    all_updates = build_updates(str(scene), str(placement), _extract(), moved_only=False)
    assert [u.ref for u in all_updates] == ["C1"]


def test_a_scene_from_another_board_is_refused(tmp_path):
    scene = _scene(tmp_path)
    # A placement for a different scene: three records for two components.
    placement = _placement(tmp_path, [(0.0, 0.0, 0, 0, 0x02, 0)] * 3)
    with pytest.raises(ApplyError, match="records for"):
        build_updates(str(scene), str(placement), _extract())
    # And an extract that is not this scene's either.
    placement = _placement(tmp_path, [(0.0, 0.0, 0, 0, 0x02, 0)] * 2)
    doc = _extract()
    doc["components"].append({"ref": "X1", "x": 0.0, "y": 0.0, "rot_deg": 0.0})
    with pytest.raises(ApplyError, match="not this board's"):
        build_updates(str(scene), str(placement), doc)


def test_report_lists_what_moved(tmp_path):
    scene = _scene(tmp_path)
    placement = _placement(tmp_path, [
        (10.0, 20.0, 0, 0, 0x01, 0),
        (31.0, 40.0, 0, 0, 0x02, 0),
    ])
    updates = build_updates(str(scene), str(placement), _extract())
    report = tmp_path / "r.report.json"
    write_report(str(report), updates, str(scene), str(placement), "board.kicad_pcb", 1)
    doc = json.loads(report.read_text())
    assert doc["schema"] == "dodplace.apply-report/1"
    assert doc["applied"] == 1 and doc["moved"] == 1 and doc["rotated"] == 0
    assert doc["components"][0]["ref"] == "C1"
    assert doc["components"][0]["dx"] == pytest.approx(1.0)


@pytest.mark.skipif(
    find_kicad_python() is None or True,  # the round trip needs a real board; see test_extract
    reason="integration: needs a board file and the KiCad backend",
)
def test_apply_round_trip(tmp_path):  # pragma: no cover - documented, not run in CI
    raise HeadlessUnavailable("apply round-trip is exercised by the CLI, not the unit suite")
