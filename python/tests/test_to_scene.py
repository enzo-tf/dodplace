"""Extract -> scene conversion, without touching KiCad."""

from __future__ import annotations

import pytest

from dodplace.ir.spec import (
    COMP_LOCKED,
    COMP_POLY_COURTYARD,
    COMP_ROT_FIXED,
    COMP_SIDE_BOTTOM,
    DEGRADED_BY_NAME,
    NO_ID,
    PIN_GROUND,
    PIN_INFERRED,
    PIN_POWER,
    POLY_KIND_BOARD_OUTLINE,
    POLY_KIND_COURTYARD,
    POLY_KIND_KEEPOUT,
    SIDE_BOTTOM,
)
from dodplace.kicad import build_scene
from dodplace.kicad.schema import ExtractError, validate


def synthetic_extract() -> dict:
    return {
        "schema": "dodplace.extract/1",
        "backend": "test",
        "backend_version": "0",
        "board": {
            "path": "board.kicad_pcb",
            "copper_layers": 4,
            "outline": [[[0, 0], [50, 0], [50, 30], [0, 30]]],
        },
        "keepouts": [{"name": "ant", "layer_mask": 3, "xy": [[[40, 2], [48, 2], [48, 8], [40, 8]]]}],
        "netclasses": [
            {"name": "Default", "clearance_mm": 0.2, "track_width_mm": 0.25},
            {"name": "Power", "clearance_mm": 0.4, "track_width_mm": 0.5},
        ],
        "nets": [
            {"name": "GND", "netclass": "Default"},
            {"name": "VCC", "netclass": "Power"},
            {"name": "USB1_P", "netclass": "Default"},
            {"name": "USB1_N", "netclass": "Default"},
        ],
        "components": [
            {
                "ref": "U1",
                "value": "MCU",
                "fpid": "Pkg:QFN",
                "x": 10.0,
                "y": 10.0,
                "rot_deg": 90.0,
                "side": "top",
                "locked": False,
                "courtyard": [[[-2, -2], [2, -2], [2, 2], [-2, 2]]],
                "pads": [
                    {"number": "1", "net": "GND", "x": -1.0, "y": -1.0,
                     "size_x": 0.5, "size_y": 0.5, "attrib": "smd", "drill": 0.0},
                    {"number": "2", "net": "VCC", "x": 1.0, "y": -1.0,
                     "size_x": 0.5, "size_y": 0.5, "attrib": "smd", "drill": 0.0},
                    {"number": "3", "net": "USB1_P", "x": -1.0, "y": 1.0,
                     "size_x": 0.5, "size_y": 0.5, "attrib": "smd", "drill": 0.0},
                    {"number": "4", "net": "USB1_N", "x": 1.0, "y": 1.0,
                     "size_x": 0.5, "size_y": 0.5, "attrib": "smd", "drill": 0.0},
                ],
            },
            {
                "ref": "C1",
                "value": "100n",
                "fpid": "Pkg:0603",
                "x": 20.0,
                "y": 5.0,
                "rot_deg": 270.0,
                "side": "bottom",
                "locked": True,
                "courtyard": [],
                "pads": [
                    {"number": "1", "net": "VCC", "x": -0.8, "y": 0.0,
                     "size_x": 0.9, "size_y": 0.95, "attrib": "smd", "drill": 0.0},
                    {"number": "2", "net": "GND", "x": 0.8, "y": 0.0,
                     "size_x": 0.9, "size_y": 0.95, "attrib": "smd", "drill": 0.0},
                ],
            },
        ],
        "warnings": [],
    }


def test_counts_and_connectivity():
    result = build_scene(synthetic_extract())
    counts = result.builder.counts()
    assert counts["comps"] == 2
    assert counts["pins"] == 6
    assert counts["nets"] == 4
    assert counts["net_entries"] == 6
    assert counts["polygons"] == 4  # 2 courtyards + outline + keepout
    assert result.report.n_net_entries == 6


def test_component_flags_carry_side_rotation_and_lock():
    result = build_scene(synthetic_extract())
    flags = result.builder._columns["comps.flags"]
    assert flags[0] == 1 | COMP_POLY_COURTYARD  # 90 degrees, top, unlocked
    assert flags[1] & COMP_SIDE_BOTTOM
    assert flags[1] & COMP_LOCKED
    assert flags[1] & COMP_ROT_FIXED is 0  # 270 degrees is an exact quarter turn
    assert flags[1] & COMP_POLY_COURTYARD


def test_non_quarter_rotation_is_pinned():
    doc = synthetic_extract()
    doc["components"][0]["rot_deg"] = 45.0
    result = build_scene(doc)
    assert result.builder._columns["comps.flags"][0] & COMP_ROT_FIXED


def test_pin_roles_come_from_net_names():
    result = build_scene(synthetic_extract())
    flags = result.builder._columns["pins.flags"]
    nets = result.builder._columns["pins.net_id"]
    # Nets are created in sorted name order: GND, USB1_N, USB1_P, VCC.
    net_of = {index: name for index, name in enumerate(sorted(("GND", "VCC", "USB1_P", "USB1_N")))}
    by_pin = {}
    for pin, net in enumerate(nets):
        if net != NO_ID:
            by_pin.setdefault(net_of[net], []).append(flags[pin])
    assert all(value == (PIN_GROUND | PIN_INFERRED) for value in by_pin["GND"])
    assert all(value == (PIN_POWER | PIN_INFERRED) for value in by_pin["VCC"])


def test_netclass_matrix_is_symmetric_and_conservative():
    result = build_scene(synthetic_extract())
    # Default is index 0, Power is index 1.
    matrix = result.builder._columns["rules.class_clearance"]
    assert matrix == [0.2, 0.4, 0.4, 0.4]
    classes = result.builder._columns["nets.net_class"]
    assert classes == [0, 0, 0, 1]  # GND, USB1_N, USB1_P (default), VCC (Power)
    assert DEGRADED_BY_NAME["NO_NET_CLASSES"] & result.report.degraded == 0


def test_derived_courtyard_and_its_bit():
    result = build_scene(synthetic_extract(), margin_mm=0.25)
    assert result.report.derived_courtyards == ["C1"]
    assert result.report.degraded & DEGRADED_BY_NAME["COURTYARD_BBOX"]

    # Pad extents (+-0.8 in x, +-0.475 in y) plus the margin.
    vertices = result.builder._columns
    offsets = vertices["poly.offsets"]
    start, end = offsets[1], offsets[2]  # C1 is the second courtyard
    xs = vertices["poly.vx"][start:end]
    ys = vertices["poly.vy"][start:end]
    # The larger pad dimension is used on both axes (0.95 -> half 0.475).
    assert min(xs) == pytest.approx(-0.8 - 0.475 - 0.25)
    assert max(xs) == pytest.approx(0.8 + 0.475 + 0.25)
    assert min(ys) == pytest.approx(-0.475 - 0.25)
    assert max(ys) == pytest.approx(0.475 + 0.25)


def test_board_outline_and_keepout_become_polygons():
    result = build_scene(synthetic_extract())
    kinds = result.builder._columns["poly.kind"]
    assert kinds.count(POLY_KIND_BOARD_OUTLINE) == 1
    assert kinds.count(POLY_KIND_KEEPOUT) == 1
    assert kinds.count(POLY_KIND_COURTYARD) == 2
    masks = result.builder._columns["poly.layer_mask"]
    assert 3 in masks  # the keepout layer mask survived


def test_diffpair_detected_and_bit_cleared():
    result = build_scene(synthetic_extract())
    assert result.report.diff_pairs == [("USB1_P", "USB1_N")]
    assert result.report.degraded & DEGRADED_BY_NAME["NO_DIFFPAIRS"] == 0
    assert result.builder._columns["diffpairs.net_p"]
    assert DEGRADED_BY_NAME["NO_PIN_ELEC"] & result.report.degraded


def test_no_outline_is_left_to_the_engine():
    doc = synthetic_extract()
    doc["board"]["outline"] = []
    result = build_scene(doc)
    assert POLY_KIND_BOARD_OUTLINE not in result.builder._columns["poly.kind"]


def test_board_config_carries_layer_count_and_sides():
    result = build_scene(synthetic_extract(), allowed_sides=SIDE_BOTTOM)
    board = result.scene.board
    assert board.copper_layers == 4
    assert board.has_stackup == 1
    assert board.allowed_sides == SIDE_BOTTOM
    assert board.courtyard_fallback_margin == 0.25
    assert board.global_clearance == 0.2
    assert board.min_track_width == 0.25
    assert board.degraded_mask == result.report.degraded


def test_zero_clearance_is_clamped_positive():
    doc = synthetic_extract()
    doc["netclasses"][0]["clearance_mm"] = 0.0
    doc["netclasses"][0]["track_width_mm"] = 0.0
    result = build_scene(doc)
    assert all(value > 0 for value in result.builder._columns["rules.class_clearance"])
    assert result.scene.board.min_track_width > 0


def test_validation_rejects_malformed_documents():
    doc = synthetic_extract()
    doc["components"][0]["side"] = "middle"
    with pytest.raises(ExtractError):
        validate(doc)

    doc = synthetic_extract()
    del doc["netclasses"]
    with pytest.raises(ExtractError):
        validate(doc)

    doc = synthetic_extract()
    doc["components"][0]["pads"][0]["net"] = 42
    with pytest.raises(ExtractError):
        validate(doc)

    doc = synthetic_extract()
    doc["components"][1]["ref"] = "U1"  # duplicate reference
    with pytest.raises(ExtractError):
        validate(doc)


def test_unknown_netclass_falls_back_with_a_warning():
    doc = synthetic_extract()
    doc["nets"][1]["netclass"] = "DoesNotExist"
    result = build_scene(doc)
    assert any("DoesNotExist" in warning for warning in result.report.warnings)
    assert result.builder._columns["nets.net_class"][1] == 0
