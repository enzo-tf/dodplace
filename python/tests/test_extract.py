"""KiCad extraction: live (when KiCad's Python is available) and hermetic."""

from __future__ import annotations

from pathlib import Path

import pytest

from dodplace.kicad import extract_board
from dodplace.kicad.headless import HeadlessUnavailable, find_kicad_python

from conftest import normalize_extract


def test_live_extraction_matches_the_committed_extract(fixture_board, committed_extract):
    """The frozen extract must still be what pcbnew produces.

    Skipped when no interpreter with pcbnew exists; it is an integration check,
    not a unit test.
    """
    try:
        find_kicad_python()
    except HeadlessUnavailable as exc:
        pytest.skip(str(exc))
    doc = extract_board(str(fixture_board))
    assert normalize_extract(doc) == normalize_extract(committed_extract)


def test_committed_extract_describes_the_fixture_board(committed_extract):
    doc = committed_extract
    refs = [component["ref"] for component in doc["components"]]
    assert sorted(refs) == ["C1", "R1", "R2", "U1"]

    by_ref = {component["ref"]: component for component in doc["components"]}

    # Two sides, three rotations, one locked part.
    assert by_ref["C1"]["side"] == "bottom"
    assert by_ref["R2"]["rot_deg"] == pytest.approx(90.0)
    assert by_ref["R2"]["locked"] is True
    assert by_ref["U1"]["rot_deg"] == pytest.approx(180.0)

    # C1's courtyard graphics were stripped, so it must have none.
    assert by_ref["C1"]["courtyard"] == []
    assert len(by_ref["R1"]["courtyard"]) == 1
    assert len(by_ref["R1"]["courtyard"][0]) >= 4

    # Connectivity came through, including the differential pair.
    nets = {pad["net"] for component in doc["components"] for pad in component["pads"]}
    assert {"GND", "VCC", "USB1_P", "USB1_N"} <= nets

    # Board level data.
    assert doc["board"]["copper_layers"] >= 2
    assert len(doc["board"]["outline"]) == 1
    assert len(doc["board"]["outline"][0]) >= 4
    assert any(keepout["name"] == "antenna-keepout" for keepout in doc["keepouts"])
    assert doc["netclasses"][0]["clearance_mm"] > 0
    assert doc["warnings"] == []


def test_pad_offsets_are_local_to_the_footprint(committed_extract):
    """Pins are stored unrotated in the footprint frame.

    The fixture has parts at 0, 90 and 180 degrees; a 0603 pad sits about
    0.79 mm from the centre whatever the part's rotation is on the board.
    """
    for component in committed_extract["components"]:
        if "R_0603" not in component["fpid"]:
            continue
        distances = sorted(
            round((pad["x"] ** 2 + pad["y"] ** 2) ** 0.5, 3) for pad in component["pads"]
        )
        assert distances[0] == pytest.approx(distances[1], abs=1e-3)
        assert 0.5 < distances[0] < 1.0, component["ref"]


def test_extract_is_json_serialisable(committed_extract):
    import json

    assert json.loads(json.dumps(committed_extract)) == committed_extract
