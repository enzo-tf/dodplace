"""The scene IR writer must match the C implementation byte for byte."""

from __future__ import annotations

import numpy as np
import pytest

from dodplace.ir import (
    Scene,
    SceneBuilder,
    read_scene,
    scene_to_bytes,
    write_scene,
)
from dodplace.ir.spec import (
    COMP_LOCKED,
    DEGRADED_BY_NAME,
    NO_ID,
    POLY_KIND_BOARD_OUTLINE,
    POLY_KIND_COURTYARD,
    TAIL,
)


def test_golden_fixture_is_reproduced_byte_for_byte(golden_scene):
    """Decode the C golden fixture and re-encode it: identical bytes.

    This is the contract between src/io.c and this package: section order,
    offsets, padding, header fields and the JSON tail all have to agree.
    """
    original = golden_scene.read_bytes()
    scene = read_scene(str(golden_scene))
    assert scene_to_bytes(scene) == original


def test_golden_fixture_decodes_to_the_expected_scene(golden_scene):
    scene = read_scene(str(golden_scene))
    assert scene.counts == {
        "comps": 3,
        "pins": 8,
        "nets": 3,
        "net_entries": 8,
        "polygons": 3,
        "vertices": 13,
        "decoupling": 1,
        "thermal": 1,
        "symmetry": 1,
        "diffpairs": 1,
    }
    assert scene.num_net_classes == 2
    assert scene.tail == TAIL
    assert scene.board.degraded_mask == (
        DEGRADED_BY_NAME["NO_3D_HEIGHT"]
        | DEGRADED_BY_NAME["NO_MASS"]
        | DEGRADED_BY_NAME["NO_PIN_CURRENT"]
        | DEGRADED_BY_NAME["COURTYARD_BBOX"]
        | DEGRADED_BY_NAME["NO_MAX_LENGTH"]
        | DEGRADED_BY_NAME["NO_SEGREGATION"]
    )
    # C1 was rebased onto its pad-bbox centre by finalize().
    half_w = scene.arrays["comps.half_w"]
    # C1 has no exact courtyard: bbox of pad centres + margin, rebased.
    assert half_w[1] == pytest.approx(0.8 + 0.25, abs=1e-6)


def test_builder_round_trip(tmp_path):
    builder = SceneBuilder()
    builder.add_component(1.0, 2.0, flags=COMP_LOCKED, ref="U1")
    builder.add_component(3.0, 4.0, ref="R1")
    outline = builder.add_polygon(POLY_KIND_BOARD_OUTLINE, [(0, 0), (10, 0), (10, 10), (0, 10)])
    courtyard = builder.add_polygon(POLY_KIND_COURTYARD, [(-1, -1), (1, -1), (1, 1), (-1, 1)])
    net = builder.add_net(weight=2.0, net_class=0)
    pin_a = builder.add_pin(0, -0.5, 0.0, flags=0, max_current=1.0)
    pin_b = builder.add_pin(1, 0.5, 0.0)
    builder.connect(pin_a, net)
    builder.set_clearance_matrix([[0.2]])
    builder.degraded = DEGRADED_BY_NAME["NO_MASS"]

    scene = builder.build()
    assert outline == 0 and courtyard == 1
    assert isinstance(scene, Scene)
    assert scene.counts["net_entries"] == 1
    assert scene.counts["vertices"] == 8

    path = tmp_path / "scene.bin"
    write_scene(str(path), scene)
    again = read_scene(str(path))

    for name, array in scene.arrays.items():
        assert np.array_equal(again.arrays[name], array), name
    assert again.counts == scene.counts
    assert again.board.degraded_mask == scene.board.degraded_mask


def test_builder_rejects_inconsistent_input():
    builder = SceneBuilder()
    builder.add_component(0.0, 0.0)
    with pytest.raises(ValueError):
        builder.add_polygon(POLY_KIND_COURTYARD, [(0, 0), (1, 1)])  # 2 points
    with pytest.raises(ValueError):
        builder.set_clearance_matrix([[0.2, 0.3], [0.4]])  # not square


def test_writer_requires_the_arrays_it_declares():
    scene = Scene(counts={"comps": 2}, num_net_classes=0)
    with pytest.raises(ValueError):
        scene_to_bytes(scene)


def test_unknown_degraded_bit_is_refused():
    builder = SceneBuilder()
    builder.add_component(0.0, 0.0)
    builder.degraded = 1 << 31
    with pytest.raises(ValueError):
        builder.build()


def test_unconnected_pins_stay_out_of_the_csr(golden_scene):
    """pins.net_id == NO_ID must not be counted in net_entries."""
    builder = SceneBuilder()
    builder.add_component(0.0, 0.0)
    builder.add_pin(0, 0.0, 0.0)  # unconnected
    assert builder.counts()["net_entries"] == 0
    assert builder._columns["pins.net_id"][0] == NO_ID
