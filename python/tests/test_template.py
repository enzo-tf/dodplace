"""Generated starters: the parts-file skeleton and the extra.toml template.

These are the two files a user is meant to edit, so the tests check the round
trip (generated -> imported/parsed) rather than just the rendering.
"""

from __future__ import annotations

import csv
import io
import json

import pytest

from dodplace.enrich import template
from dodplace.enrich.providers import extra_toml, parts_file
from dodplace.enrich.store import EnrichmentStore


def board_doc(components: list[dict], netclasses=None) -> dict:
    return {
        "schema": "dodplace.extract/1",
        "backend": "test",
        "board": {"path": "/tmp/board.kicad_pcb", "copper_layers": 2, "outline": []},
        "keepouts": [],
        "netclasses": netclasses or [{"name": "Default", "clearance_mm": 0.2,
                                      "track_width_mm": 0.2}],
        "components": components,
        "warnings": [],
    }


def component(ref: str, fpid: str = "Pkg:Part", value: str = "", lcsc: str = "") -> dict:
    fields = {"LCSC Part": lcsc} if lcsc else {}
    return {
        "ref": ref,
        "value": value,
        "fpid": fpid,
        "x": 0.0,
        "y": 0.0,
        "rot_deg": 0.0,
        "side": "top",
        "locked": False,
        "courtyard": [],
        "pads": [{"number": "1", "net": "N1", "x": 0.0, "y": 0.0,
                  "size_x": 1.0, "size_y": 1.0, "attrib": "smd", "drill": 0.0}],
        "fields": fields,
        "models": [],
    }


# ===========================================================================
# Parts-file skeleton
# ===========================================================================


def test_skeleton_has_one_row_per_supplier_code(committed_extract):
    rows = template.skeleton_rows(committed_extract)
    codes = [row.code for row in rows]
    assert codes == sorted(codes)
    assert set(codes) == {"C25804", "C14663", "C7955"}

    # R1 and R2 share a code: one row, two designators.
    shared = next(row for row in rows if row.code == "C25804")
    assert shared.refs == ["R1", "R2"]
    assert shared.designators() == "2x R1 R2"
    assert shared.footprint.startswith("Resistor_SMD:")
    assert shared.value == "R_0603_1608Metric"


def test_skeleton_columns_are_all_understood_by_the_importer():
    """Every header must map to a known alias, or the file cannot be re-imported."""
    text = template.render_skeleton([template.SkeletonRow(code="C1", refs=["R1"])])
    lines = [line for line in text.splitlines() if not line.startswith("#")]
    header = next(csv.reader(io.StringIO(lines[0])))
    aliases = set(parts_file.CSV_ALIASES)
    for column in header:
        normalised = "".join(ch for ch in column.lower() if ch.isalnum())
        assert normalised in aliases, f"column {column!r} has no alias"


def test_skeleton_round_trip_joins_the_board(tmp_path, committed_extract):
    """Generated -> imported -> matched: the whole point of the skeleton."""
    path = tmp_path / "dodplace-parts.csv"
    path.write_text(template.render_skeleton(template.skeleton_rows(committed_extract)))

    store = EnrichmentStore(tmp_path / "cache")
    report = parts_file.import_file(path, store)
    assert report.records == 3

    matched = 0
    for comp in committed_extract["components"]:
        keys = parts_file.candidate_keys(comp)
        if store.get_first("parts", keys) is not None:
            matched += 1
    assert matched == len(committed_extract["components"]), "every part must match its row"


def test_skeleton_discovers_and_imports_next_to_a_board(tmp_path, committed_extract):
    board = tmp_path / "board.kicad_pcb"
    board.write_text("")
    (tmp_path / "dodplace-parts.csv").write_text(
        template.render_skeleton(template.skeleton_rows(committed_extract))
    )
    found = parts_file.discover(board)
    assert found == tmp_path / "dodplace-parts.csv"

    elsewhere = tmp_path / "other"
    elsewhere.mkdir()
    assert parts_file.discover(elsewhere / "board.kicad_pcb") is None


def test_parts_file_with_comments_anywhere_is_imported(tmp_path):
    """The generated file starts with `#` lines, and users add their own."""
    path = tmp_path / "bom.csv"
    path.write_text(
        "# my parts\n"
        "LCSC Part #,Comment,Mass (g)\n"
        "C1,10k,0.002\n"
        "# a note in the middle\n"
        "C2,100n,\n"
    )
    store = EnrichmentStore(tmp_path / "cache")
    report = parts_file.import_file(path, store)
    assert report.records == 2
    assert store.get("parts", "lcsc:C1").get("mass_g").value == pytest.approx(0.002)


# ===========================================================================
# extra.toml template
# ===========================================================================


MECHANICAL_BOARD = [
    component("H1"), component("H2"),
    component("FID1"), component("FID2"),
    component("J1", "Connector:FPC"),
    component("U1", "Pkg:QSOP-24_L8.7-W3.9-H1.6", "SM16306SJ", "C2830324"),
    component("Q1", "Pkg:SOP-8_L5.0-W4.0-H1.8"),
    component("D1", "Tuile_LED:LED-SMD"), component("D2", "Tuile_LED:LED-SMD"),
    component("C1", "Capacitor_SMD:C_0805_2012Metric", "100n"),
]


def test_template_seeds_the_mechanical_parts(tmp_path):
    doc = board_doc(MECHANICAL_BOARD)
    path = tmp_path / "extra.toml"
    template.write_template(doc, path)

    config = extra_toml.parse(path)
    selectors = [rule.describe() for rule in config.component_rules]
    assert "ref=H*" in selectors and "ref=FID*" in selectors and "ref=J*" in selectors

    config.materialize(MECHANICAL_BOARD)
    entries = config.overlay.components
    assert entries["H1"].locked is True
    assert entries["FID2"].locked is True
    assert entries["J1"].locked is True and entries["J1"].rot_fixed is True
    assert "C1" not in entries, "only the seeded families are touched"


def test_template_mentions_the_real_counts_and_refs(tmp_path):
    path = tmp_path / "extra.toml"
    template.write_template(board_doc(MECHANICAL_BOARD), path)
    text = path.read_text()
    assert "2 mounting holes" in text and "H1" in text
    assert "2 fiducials" in text
    assert "1 connector:" in text
    assert "SM16306SJ" in text, "the seeded thermal block names the part value"


def test_template_suggests_thermal_parts_with_placeholders(tmp_path):
    path = tmp_path / "extra.toml"
    template.write_template(board_doc(MECHANICAL_BOARD), path)
    text = path.read_text()
    assert "[[component]]" in text
    assert "power_w = 0.2" in text  # commented placeholder, to be filled
    config = extra_toml.parse(path)
    # The thermal blocks are all commented, so nothing imposes a power yet.
    assert config.overlay.thermal == []
    assert all(rule.entry.power_w is None for rule in config.component_rules)


def test_template_is_valid_toml_and_safe_as_is(tmp_path):
    """Leaving the generated file untouched must not change the scene."""
    path = tmp_path / "extra.toml"
    template.write_template(board_doc(MECHANICAL_BOARD), path)
    config = extra_toml.parse(path)
    config.materialize(MECHANICAL_BOARD)
    for ref, entry in config.overlay.components.items():
        # Only mechanical locks are active; nothing else is imposed.
        assert entry.height_mm is None and entry.mass_g is None, ref
        assert entry.power_w is None and entry.orientation_deg is None, ref


def test_template_refuses_to_overwrite(tmp_path):
    doc = board_doc(MECHANICAL_BOARD)
    path = tmp_path / "extra.toml"
    template.write_template(doc, path)
    path.write_text("# my own rules\n")
    with pytest.raises(FileExistsError):
        template.write_template(doc, path)
    assert path.read_text() == "# my own rules\n"
    template.write_template(doc, path, force=True)
    assert "dodplace user rules" in path.read_text()


def test_template_without_mechanical_parts_still_guides(tmp_path):
    doc = board_doc([component("U1"), component("C1")])
    path = tmp_path / "extra.toml"
    template.write_template(doc, path)
    text = path.read_text()
    assert "No mechanical part was recognised" in text
    assert "# ref = \"J*\"" in text


def test_template_lists_the_boards_netclasses(tmp_path):
    doc = board_doc(
        [component("U1")],
        netclasses=[{"name": "Default", "clearance_mm": 0.2, "track_width_mm": 0.2},
                    {"name": "Puissance", "clearance_mm": 0.4, "track_width_mm": 0.5}],
    )
    path = tmp_path / "extra.toml"
    template.write_template(doc, path)
    text = path.read_text()
    assert "Default, Puissance" in text
    assert "Puissance = 0.4" in text, "the override example names a real class"


def test_hints_sort_references_naturally():
    doc = board_doc([component("D2"), component("D10"), component("D1")])
    hints = template.hints_from_extract(doc)
    assert hints.refs("D") == ["D1", "D2", "D10"]


def test_generated_template_is_reported_by_the_run(tmp_path, committed_extract):
    """The file must survive a real analysis run without warnings."""
    path = tmp_path / "extra.toml"
    template.write_template(committed_extract, path)
    config = extra_toml.parse(path)
    assert config.component_rules, "a template with no rules would be useless"
    assert config.overlay.board.ceiling_height_mm is None  # nothing imposed
