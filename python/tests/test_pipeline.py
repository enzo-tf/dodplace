"""End to end: extract -> scene.bin -> the C engine -> placement.bin -> numpy.

The C solver is found next to the checkout; the tests skip when it has not been
built, so the Python suite stays runnable on its own.
"""

from __future__ import annotations

import subprocess

from dodplace.ir import read_placement, read_scene, write_scene
from dodplace.kicad import build_scene

from dodplace.ir.spec import PLACEMENT_FLAG_LOCKED


def _scene_from_extract(committed_extract, tmp_path):
    result = build_scene(committed_extract)
    path = tmp_path / "scene.bin"
    write_scene(str(path), result.scene)
    return path, result


def test_solver_accepts_a_python_written_scene(committed_extract, tmp_path, solver_binary):
    scene_path, result = _scene_from_extract(committed_extract, tmp_path)
    proc = subprocess.run([str(solver_binary), str(scene_path)], capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr
    assert f"{result.report.n_components} components" in proc.stdout
    assert f"{result.report.n_pins} pins" in proc.stdout


def test_solver_round_trips_the_placement(committed_extract, tmp_path, solver_binary):
    scene_path, result = _scene_from_extract(committed_extract, tmp_path)
    placement_path = tmp_path / "placement.bin"
    proc = subprocess.run(
        [str(solver_binary), str(scene_path), "--identity", "-o", str(placement_path), "--quiet"],
        capture_output=True,
        text=True,
    )
    assert proc.returncode == 0, proc.stderr

    placement = read_placement(str(placement_path))
    assert placement.count == result.report.n_components

    # The identity placement reproduces what was ingested.
    scene = read_scene(str(scene_path))
    assert (placement.x == scene.arrays["comps.x"]).all()
    assert (placement.y == scene.arrays["comps.y"]).all()

    # R2 was locked and stays flagged as such.
    flags = result.scene.arrays["comps.flags"]
    locked = [index for index, value in enumerate(flags) if value & 0x08]
    assert locked, "the fixture has a locked component"
    for index in locked:
        assert placement.flags[index] & PLACEMENT_FLAG_LOCKED

    # Sides and orientations survive the round trip.
    for index, value in enumerate(flags):
        assert placement.orient[index] == (value & 0x03)
        assert placement.side[index] == (1 if value & 0x04 else 0)


def test_scene_decoded_by_python_matches_what_was_written(committed_extract, tmp_path):
    scene_path, result = _scene_from_extract(committed_extract, tmp_path)
    decoded = read_scene(str(scene_path))
    assert decoded.counts == result.scene.counts
    assert decoded.board.degraded_mask == result.report.degraded
    for name, array in result.scene.arrays.items():
        assert (decoded.arrays[name] == array).all(), name
