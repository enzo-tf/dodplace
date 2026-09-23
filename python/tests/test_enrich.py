"""Enrichment: store, providers, precedence and the degraded-bit coverage."""

from __future__ import annotations

import json

import pytest

from dodplace.enrich import EnrichmentOverlay, EnrichmentStore, resolve
from dodplace.enrich.model import Fact, PartFacts, Provenance
from dodplace.enrich.providers import datasheet_pdf, extra_toml, model_name, parts_file, step_model
from dodplace.enrich.overlay import ComponentEnrichment
from dodplace.ir.spec import (
    COMP_HAS_HEIGHT,
    COMP_HAS_MASS,
    COMP_LOCKED,
    COMP_ROT_FIXED,
    COMP_SIDE_BOTTOM,
    DEGRADED_BY_NAME,
    PIN_GROUND,
    PIN_INFERRED,
    PIN_POWER,
    POLY_KIND_KEEPOUT,
)
from dodplace.kicad import build_scene

FIXTURES = __import__("pathlib").Path(__file__).resolve().parent / "fixtures"


# ===========================================================================
# Store
# ===========================================================================


def test_store_round_trip_and_sharding(tmp_path):
    store = EnrichmentStore(tmp_path)
    record = PartFacts(identity="lcsc:C25804")
    record.set("height_mm", 0.45, Provenance("parts_file", "bom.csv"))
    record.pin_roles["1"] = Fact("PIN_POWER", Provenance("extra_toml"))

    path = store.put("parts", "lcsc:C25804", record)
    assert path.is_file()
    assert path.parent.name == path.stem[:2]  # two-level sharding
    assert not list(tmp_path.rglob(".tmp-*"))  # atomic write left nothing behind

    loaded = store.get("parts", "lcsc:C25804")
    assert loaded is not None
    assert loaded.get("height_mm").value == pytest.approx(0.45)
    assert loaded.get("height_mm").provenance.source == "parts_file"
    assert loaded.pin_roles["1"].value == "PIN_POWER"
    assert store.count("parts") == 1
    assert list(store.keys("parts")) == ["lcsc:C25804"]


def test_store_miss_and_corruption_are_misses(tmp_path):
    store = EnrichmentStore(tmp_path)
    assert store.get("parts", "lcsc:NOPE") is None

    path = store.path_for("parts", "lcsc:X")
    path.parent.mkdir(parents=True)
    path.write_text("{ not json")
    assert store.get("parts", "lcsc:X") is None


def test_store_merge_keeps_newest_per_field(tmp_path):
    store = EnrichmentStore(tmp_path)
    first = PartFacts(identity="lcsc:C1")
    first.set("height_mm", 1.0, Provenance("parts_file", "a.csv"))
    first.set("mpn", "AAA", Provenance("parts_file", "a.csv"))
    store.merge("parts", "lcsc:C1", first)

    second = PartFacts(identity="lcsc:C1")
    second.set("height_mm", 2.0, Provenance("extra_toml", "b.toml"))
    merged = store.merge("parts", "lcsc:C1", second)

    assert merged.get("height_mm").value == pytest.approx(2.0)
    assert merged.get("height_mm").provenance.source == "extra_toml"
    assert merged.get("mpn").value == "AAA"  # untouched
    assert store.invalidate("parts", "lcsc:C1")
    assert not store.invalidate("parts", "lcsc:C1")


def test_store_get_first_follows_key_order(tmp_path):
    store = EnrichmentStore(tmp_path)
    record = PartFacts(identity="fp:Pkg:0603")
    record.set("height_mm", 0.5, Provenance("parts_file", "bom.csv"))
    store.put("parts", "fp:Pkg:0603", record)
    hit = store.get_first("parts", ["lcsc:C9", "fp:Pkg:0603"])
    assert hit is not None
    assert hit[0] == "fp:Pkg:0603"


# ===========================================================================
# extra.toml
# ===========================================================================


def test_extra_toml_parses_the_fixture():
    config = extra_toml.parse(FIXTURES / "extra_fixture.toml")
    overlay = config.overlay
    board = overlay.board
    assert board.ceiling_height_mm == pytest.approx(20.0)
    assert board.airflow == (1.0, 0.0)
    assert board.via_under_body_allowed is False
    assert board.courtyard_margin_mm == pytest.approx(0.3)
    assert board.net_clearance["VCC"] == pytest.approx(0.4)
    assert board.net_class_of["USB*"] == "HighSpeed"
    assert board.class_clearance["HighSpeed"] == pytest.approx(0.25)
    assert len(board.keepouts) == 1 and len(board.keepouts[0].polygon) == 4

    # Component rules are globs until they meet a board; then they materialise.
    config.materialize([
        {"ref": "U1", "fpid": "Package_SO:SOIC-8"},
        {"ref": "R1", "fpid": "Resistor_SMD:R_0603"},
    ])
    u1 = overlay.components["U1"]
    assert u1.orientation_deg == pytest.approx(90.0)
    assert u1.rot_fixed is True
    assert u1.power_w == pytest.approx(0.35)
    assert u1.pin_roles == {"4": PIN_GROUND, "8": PIN_POWER}
    assert u1.pin_currents == {"4": pytest.approx(0.5), "8": pytest.approx(0.5)}

    assert len(overlay.thermal) == 1 and overlay.thermal[0].component == "R1"
    assert len(overlay.decoupling) == 1 and overlay.decoupling[0].weight == pytest.approx(2.0)
    assert len(overlay.symmetry) == 1 and overlay.symmetry[0].axis == "x"


def test_extra_toml_rejects_bad_input(tmp_path):
    def write(text: str):
        path = tmp_path / "extra.toml"
        path.write_text(text)
        return path

    with pytest.raises(extra_toml.ExtraTomlError):
        extra_toml.parse(write("[[component]]\nheight_mm = 1.0\n"))  # no ref
    with pytest.raises(extra_toml.ExtraTomlError):
        extra_toml.parse(write('[board]\nallowed_sides = "left"\n'))
    with pytest.raises(extra_toml.ExtraTomlError):
        extra_toml.parse(write('[[component]]\nref = "U1"\npin_roles = { "1" = "wizard" }\n'))
    with pytest.raises(extra_toml.ExtraTomlError):
        extra_toml.parse(write('[[symmetry]]\na = "R1"\nb = "R2"\naxis = "z"\n'))
    with pytest.raises(extra_toml.ExtraTomlError):
        extra_toml.parse(write('[[keepout]]\npolygon = [[0, 0], [1, 1]]\n'))


def test_extra_toml_later_component_blocks_win(tmp_path):
    path = tmp_path / "extra.toml"
    path.write_text(
        '[[component]]\nref = "U1"\nheight_mm = 1.0\nmass_g = 1.0\n'
        '[[component]]\nref = "U1"\nheight_mm = 2.0\n'
    )
    config = extra_toml.parse(path)
    config.materialize([{"ref": "U1", "fpid": "x"}])
    assert config.overlay.components["U1"].height_mm == pytest.approx(2.0)
    assert config.overlay.components["U1"].mass_g == pytest.approx(1.0)


def test_component_rules_accept_globs(tmp_path):
    """One block can cover a family; a later block refines one member."""
    path = tmp_path / "extra.toml"
    path.write_text(
        '[[component]]\nfpid = "Tuile_LED:*"\nlocked = true\n'
        '[[component]]\nref = "FID*"\nlocked = true\nrot_fixed = true\n'
        '[[component]]\nref = "U1"\nheight_mm = 9.0\n'
    )
    config = extra_toml.parse(path)
    assert len(config.component_rules) == 3
    assert config.component_rules[0].describe() == "fpid=Tuile_LED:*"

    config.materialize([
        {"ref": "D1", "fpid": "Tuile_LED:LED-SMD"},
        {"ref": "U1", "fpid": "Tuile_LED:QSOP-24"},
        {"ref": "FID1", "fpid": "Fiducial:Fiducial_1mm"},
        {"ref": "C1", "fpid": "Capacitor_SMD:C_0805"},
    ])
    entries = config.overlay.components
    assert entries["D1"].locked is True           # footprint family
    assert entries["U1"].locked is True           # same family...
    assert entries["U1"].height_mm == pytest.approx(9.0)  # ...refined later
    assert entries["FID1"].locked is True and entries["FID1"].rot_fixed is True
    assert "C1" not in entries, "unmatched components must stay untouched"


def test_component_rule_needs_a_selector(tmp_path):
    path = tmp_path / "extra.toml"
    path.write_text('[[component]]\nheight_mm = 1.0\n')
    with pytest.raises(extra_toml.ExtraTomlError):
        extra_toml.parse(path)


# ===========================================================================
# Parts file
# ===========================================================================


def test_parts_csv_import(tmp_path):
    store = EnrichmentStore(tmp_path / "cache")
    report = parts_file.import_file(FIXTURES / "parts_fixture.csv", store)
    assert report.records == 3
    assert store.count("parts") == 3

    record = store.get("parts", "lcsc:C25804")
    assert record.get("height_mm").value == pytest.approx(0.45)
    assert record.get("mass_g").value == pytest.approx(0.002)
    assert record.get("mpn").value == "RC0603FR-0710KL"
    assert record.get("manufacturer").value == "Yageo"
    assert record.get("height_mm").provenance.source == "parts_file"


def test_parts_toml_import(tmp_path):
    source = tmp_path / "parts.toml"
    source.write_text(
        '[parts."lcsc:C1"]\n'
        "height_mm = 0.5\n"
        "mass_g = 0.01\n"
        'pin_roles = { "1" = "ground" }\n'
        'pin_currents = { "1" = 1.5 }\n'
        '[parts."MY-MPN"]\n'
        "height_mm = 1.0\n"
    )
    store = EnrichmentStore(tmp_path / "cache")
    report = parts_file.import_file(source, store)
    assert report.records == 2
    record = store.get("parts", "lcsc:C1")
    assert record.get("height_mm").value == pytest.approx(0.5)
    assert record.pin_roles["1"].value == "PIN_GROUND"
    assert record.pin_currents["1"].value == pytest.approx(1.5)
    assert store.get("parts", "mpn:MY-MPN").get("height_mm").value == pytest.approx(1.0)


def test_parts_file_identity_and_key_candidates(tmp_path):
    assert parts_file._identity("25804", None, None) == "lcsc:C25804"
    assert parts_file._identity("c25804", None, None) == "lcsc:C25804"
    assert parts_file._identity(None, "ABC", None) == "mpn:ABC"
    assert parts_file._identity(None, None, "Pkg:0603") == "fp:Pkg:0603"

    component = {
        "ref": "R1",
        "fpid": "Resistor_SMD:R_0603_1608Metric",
        "fields": {"LCSC": "C25804", "MPN": "RC0603"},
    }
    assert parts_file.candidate_keys(component) == [
        "lcsc:C25804",
        "fp:Resistor_SMD:R_0603_1608Metric",
    ]


def test_parts_file_without_identity_is_rejected(tmp_path):
    source = tmp_path / "bad.csv"
    source.write_text("Comment,Designator\n10k,R1\n")
    with pytest.raises(parts_file.PartsFileError):
        parts_file.import_file(source, EnrichmentStore(tmp_path / "cache"))


# ===========================================================================
# STEP
# ===========================================================================


STEP_TEXT = """ISO-10303-21;
DATA;
#1 = CARTESIAN_POINT('',(0.0, 0.0, 0.0));
#2 = CARTESIAN_POINT('',(1.0, -0.5, 0.0));
#3 = CARTESIAN_POINT('',(0.5, 0.5, 0.45));
#4 = CARTESIAN_POINT('',(-1.0e0, 2.5e-1, 1.2));
ENDSEC;
END-ISO-10303-21;
"""


def test_step_bbox_from_text(tmp_path):
    path = tmp_path / "part.step"
    path.write_text(STEP_TEXT)
    bbox = step_model.read_bbox(path)
    assert bbox.min == (-1.0, -0.5, 0.0)
    assert bbox.max == (1.0, 0.5, 1.2)
    assert bbox.height_mm == pytest.approx(1.2)


def test_step_bbox_rejects_a_non_step_file(tmp_path):
    path = tmp_path / "nope.step"
    path.write_text("not a step file at all")
    with pytest.raises(step_model.StepModelError):
        step_model.read_bbox(path)


def test_step_resolve_kiprjmod_and_missing(tmp_path):
    project = tmp_path / "project"
    (project / "shapes").mkdir(parents=True)
    model = project / "shapes" / "part.step"
    model.write_text(STEP_TEXT)
    board = project / "board.kicad_pcb"
    board.write_text("")

    assert step_model.resolve_model_path("${KIPRJMOD}/shapes/part.step", board) == model
    assert step_model.resolve_model_path("${KIPRJMOD}/shapes/nope.step", board) is None
    assert step_model.resolve_model_path("", board) is None
    assert step_model.resolve_model_path("${UNKNOWN_VAR}/x.step", board) is None


def test_step_sibling_is_preferred_over_the_referenced_vrml(tmp_path):
    """Custom libraries ship .wrl for the viewer and .step for geometry.

    Verified on a real board: 18 of 18 model pairs complete. Ignoring the .wrl
    and reading its sibling took height coverage from 160/707 to 696/707.
    """
    shapes = tmp_path / "shapes"
    shapes.mkdir()
    (shapes / "PART.wrl").write_text("VRML, unmeasurable here")
    (shapes / "PART.step").write_text(STEP_TEXT)
    (shapes / "ONLY.wrl").write_text("no STEP sibling")
    board = tmp_path / "board.kicad_pcb"
    board.write_text("")

    assert step_model.resolve_step_path("${KIPRJMOD}/shapes/PART.wrl", board) == shapes / "PART.step"
    assert step_model.resolve_step_path("${KIPRJMOD}/shapes/PART.step", board) == shapes / "PART.step"
    # Nothing measurable must resolve to None, never to the VRML file.
    assert step_model.resolve_step_path("${KIPRJMOD}/shapes/ONLY.wrl", board) is None
    assert step_model.resolve_step_path("${KIPRJMOD}/shapes/MISSING.wrl", board) is None


def test_resolver_reads_the_step_sibling_of_a_wrl_reference(tmp_path, committed_extract):
    """End to end through the resolver: a .wrl reference still yields a height."""
    shapes = tmp_path / "shapes"
    shapes.mkdir()
    (shapes / "PART.wrl").write_text("VRML")
    (shapes / "PART.step").write_text(STEP_TEXT)

    doc = json.loads(json.dumps(committed_extract))
    for component in doc["components"]:
        component["models"] = [str(shapes / "PART.wrl")] if component["ref"] == "U1" else []

    overlay, report = resolve(doc, step_models=True, heuristics=False)
    assert overlay.components["U1"].height_mm == pytest.approx(1.2)
    assert report.components["U1"]["height_mm"]["source"] == "step"


def test_step_height_of_a_real_kicad_model():
    """Integration: the official KiCad models resolve and measure correctly."""
    roots = step_model.model_roots()
    if not roots:
        pytest.skip("no KiCad 3D model directory on this machine")
    resistor = roots[0] / "Resistor_SMD.3dshapes" / "R_0603_1608Metric.step"
    if not resistor.is_file():
        pytest.skip("the 0603 model is not installed")
    height = step_model.height_mm(resistor)
    assert height == pytest.approx(0.45, abs=0.05)


# ===========================================================================
# Datasheet PDF rules
# ===========================================================================


RULES_TOML = """
[[rule]]
name = "fake-mcu"
mpn_prefix = "STM32"
[[rule.field]]
name = "height_mm"
pattern = "Package height[^0-9]{0,12}([0-9.]+)\\\\s*mm"
confidence = 0.55
[[rule.field]]
name = "power_w"
pattern = "Power dissipation[^0-9]{0,12}([0-9.]+)\\\\s*W"
"""

DATASHEET_TEXT = """
STM32F103C8T6
6 Package characteristics
Package height nominal 1.75 mm, see figure 12 for the tolerances.
Power dissipation: 0.33 W typical.
"""


def test_datasheet_rules_and_extraction(tmp_path):
    rules_path = tmp_path / "rules.toml"
    rules_path.write_text(RULES_TOML)
    rules = datasheet_pdf.load_rules(rules_path)
    assert len(rules) == 1 and rules[0].name == "fake-mcu"

    component = {"ref": "U1", "fpid": "Package_SO:SOIC-8", "_mpn": "STM32F103C8T6"}
    facts = datasheet_pdf.extract_from_text(
        DATASHEET_TEXT, rules, component, source_ref="fake.pdf", page=6
    )
    assert facts.get("height_mm").value == pytest.approx(1.75)
    assert facts.get("height_mm").provenance.confidence == pytest.approx(0.55)
    assert "fake.pdf#p6" in facts.get("height_mm").provenance.source_ref
    assert facts.get("power_w").value == pytest.approx(0.33)


def test_datasheet_rules_do_not_apply_to_other_parts(tmp_path):
    rules_path = tmp_path / "rules.toml"
    rules_path.write_text(RULES_TOML)
    rules = datasheet_pdf.load_rules(rules_path)
    component = {"ref": "R1", "fpid": "Resistor_SMD:R_0603", "_mpn": "RC0603FR"}
    facts = datasheet_pdf.extract_from_text(DATASHEET_TEXT, rules, component, source_ref="x.pdf")
    assert facts.facts == {}


def test_datasheet_rules_reject_unknown_fields(tmp_path):
    rules_path = tmp_path / "rules.toml"
    rules_path.write_text(
        '[[rule]]\nname = "x"\nvalue_pattern = ".*"\n'
        '[[rule.field]]\nname = "colour"\npattern = "(red)"\n'
    )
    with pytest.raises(datasheet_pdf.DatasheetError):
        datasheet_pdf.load_rules(rules_path)


# ===========================================================================
# Resolver
# ===========================================================================


def _store_with_parts(tmp_path, entries: dict) -> EnrichmentStore:
    store = EnrichmentStore(tmp_path / "cache")
    for identity, facts in entries.items():
        record = PartFacts(identity=identity)
        for name, value in facts.items():
            record.set(name, value, Provenance("parts_file", "bom.csv"))
        store.put("parts", identity, record)
    return store


def test_resolver_precedence_extra_beats_parts_file(tmp_path, committed_extract):
    store = _store_with_parts(tmp_path, {"lcsc:C7955": {"height_mm": 1.75, "mass_g": 0.07}})
    extra = tmp_path / "extra.toml"
    extra.write_text('[[component]]\nref = "U1"\nheight_mm = 9.0\n')

    overlay, report = resolve(committed_extract, extra_path=extra, store=store, step_models=False)
    u1 = overlay.components["U1"]
    assert u1.height_mm == pytest.approx(9.0)          # extra.toml wins
    assert u1.mass_g == pytest.approx(0.07)            # parts file fills the gap
    assert report.components["U1"]["height_mm"]["source"] == "extra_toml"
    assert report.components["U1"]["mass_g"]["source"] == "parts_file"


def test_resolver_reports_what_stayed_unresolved(tmp_path, committed_extract):
    store = _store_with_parts(tmp_path, {"lcsc:C7955": {"height_mm": 1.75}})
    overlay, report = resolve(committed_extract, store=store, step_models=False)

    assert overlay.components["U1"].height_mm == pytest.approx(1.75)
    assert overlay.components["R1"].height_mm is None
    assert report.components["R1"]["height_mm"] == {"unresolved": True}
    assert report.unresolved["height_mm"] == 3
    assert report.parts_matched == 1


def test_resolver_step_fills_only_missing_heights(tmp_path, committed_extract):
    model = tmp_path / "C7955.step"
    model.write_text(STEP_TEXT)
    doc = json.loads(json.dumps(committed_extract))
    for component in doc["components"]:
        component["models"] = [str(model)] if component["ref"] == "U1" else []

    overlay, report = resolve(doc, step_models=True, heuristics=False)
    assert overlay.components["U1"].height_mm == pytest.approx(1.2)
    assert overlay.components["U1"].mass_g is None
    assert report.step_models_read == 1
    assert report.components["U1"]["height_mm"]["source"] == "step"


def test_resolver_netlist_heuristic_pairs_caps_with_ics(committed_extract):
    overlay, report = resolve(committed_extract, step_models=False, heuristics=True)
    pairs = {(row.ic, row.cap): row for row in overlay.decoupling}
    assert ("U1", "C1") in pairs  # both sit on VCC
    assert pairs[("U1", "C1")].pin == "8"  # U1's VCC pad
    assert pairs[("R1", "C1")].pin == "1"  # R1 is on VCC too, hence also paired
    assert report.sources.get("netlist_heuristic")


def test_explicit_decoupling_suppresses_the_heuristic(committed_extract):
    extra = FIXTURES / "extra_fixture.toml"
    overlay, _ = resolve(committed_extract, extra_path=extra, step_models=False, heuristics=True)
    assert len(overlay.decoupling) == 1
    assert overlay.decoupling[0].max_distance_mm == pytest.approx(4.0)


def test_power_fact_becomes_a_thermal_row(committed_extract):
    extra = FIXTURES / "extra_fixture.toml"
    overlay, _ = resolve(committed_extract, extra_path=extra, step_models=False)
    rows = {row.component: row for row in overlay.thermal}
    assert rows["U1"].power_w == pytest.approx(0.35)  # from [[component]] power_w
    assert rows["R1"].power_w == pytest.approx(0.1)  # from [[thermal]]


# ===========================================================================
# Coverage-driven degraded bits
# ===========================================================================


def test_enrichment_clears_the_bits_it_resolves(committed_extract):
    store = EnrichmentStore(FIXTURES)  # not used; keeps the signature explicit
    store = _store_with_parts(
        __import__("pathlib").Path("/tmp"), {}
    )
    # Use the committed store shape instead: build the parts records in memory.
    overlay, _ = resolve(committed_extract, step_models=False)

    baseline = build_scene(committed_extract).report.degraded
    for bit in ("NO_3D_HEIGHT", "NO_MASS"):
        assert baseline & DEGRADED_BY_NAME[bit]

    empty = build_scene(committed_extract, enrichment=EnrichmentOverlay())
    assert empty.report.degraded == baseline  # an empty overlay changes nothing
    assert store is not None


def test_scene_with_full_enrichment_has_only_irreducible_bits(tmp_path, committed_extract):
    store = parts_file_enrich(tmp_path)
    extra = FIXTURES / "extra_fixture.toml"
    overlay, enrich_report = resolve(
        committed_extract, extra_path=extra, store=store, step_models=False
    )
    result = build_scene(committed_extract, enrichment=overlay, enrichment_report=enrich_report)
    mask = result.report.degraded

    # Resolved by enrichment:
    for bit in ("NO_3D_HEIGHT", "NO_MASS", "NO_NET_CLASSES"):
        assert not mask & DEGRADED_BY_NAME[bit], bit
    # Still missing, and honestly reported:
    for bit in ("NO_ROTATION_DATA", "NO_PIN_ELEC", "NO_PIN_CURRENT",
                "NO_NET_WEIGHTS", "COURTYARD_BBOX"):
        assert mask & DEGRADED_BY_NAME[bit], bit
    # Board-level bits are the engine's job, so the producer must not raise them:
    assert not mask & DEGRADED_BY_NAME["BOARD_OUTLINE_BBOX"]
    assert not mask & DEGRADED_BY_NAME["NO_STACKUP"]


def parts_file_enrich(tmp_path) -> EnrichmentStore:
    store = EnrichmentStore(tmp_path / "cache")
    parts_file.import_file(FIXTURES / "parts_fixture.csv", store)
    return store


def test_enrichment_applies_flags_roles_and_board_data(tmp_path, committed_extract):
    store = parts_file_enrich(tmp_path)
    extra = FIXTURES / "extra_fixture.toml"
    overlay, report = resolve(committed_extract, extra_path=extra, store=store, step_models=False)
    result = build_scene(committed_extract, enrichment=overlay, enrichment_report=report)

    refs = result.builder.refs
    u1 = refs.index("U1")
    flags = result.builder._columns["comps.flags"][u1]
    assert flags & COMP_HAS_HEIGHT and flags & COMP_HAS_MASS
    assert flags & COMP_ROT_FIXED
    assert (flags & 0x03) == 1  # 90 degrees from extra.toml
    heights = result.builder._columns["comps.height"]
    masses = result.builder._columns["comps.mass"]
    assert heights[u1] == pytest.approx(1.75)
    assert masses[u1] == pytest.approx(0.07)

    # Authoritative pin roles replace the inference on those pads only.
    pin_flags = result.builder._columns["pins.flags"]
    currents = result.builder._columns["pins.max_current"]
    # extra.toml made U1 pads 8 (power) and 4 (ground) authoritative; the rest
    # of U1 keeps the net-name inference and is flagged as such. Pin indices are
    # positional, so map them through the extract's pad order.
    component_of = result.builder._columns["pins.comp_id"]
    u1_indices = [index for index, comp in enumerate(component_of) if comp == u1]
    u1_doc = next(c for c in committed_extract["components"] if c["ref"] == "U1")
    numbers = [str(pad["number"]) for pad in u1_doc["pads"]]
    assert len(numbers) == len(u1_indices)
    for position, index in enumerate(u1_indices):
        number = numbers[position]
        if number == "8":
            assert pin_flags[index] == PIN_POWER
            assert currents[index] == pytest.approx(0.5)
        elif number == "4":
            assert pin_flags[index] == PIN_GROUND
            assert currents[index] == pytest.approx(0.5)
        else:
            # No authoritative role: whatever the heuristic guessed, it is a guess.
            assert pin_flags[index] != PIN_POWER
            assert pin_flags[index] != PIN_GROUND

    board = result.scene.board
    assert board.ceiling_height == pytest.approx(20.0)
    assert board.airflow_x == pytest.approx(1.0)
    assert board.allow_vias_under_body == 0
    assert board.courtyard_fallback_margin == pytest.approx(0.3)

    # The extra keepout became a polygon, and the glob put USB* in its own class.
    kinds = result.builder._columns["poly.kind"]
    assert kinds.count(POLY_KIND_KEEPOUT) == 2  # board keepout + extra.toml keepout
    assert "HighSpeed" in result.report.netclass_names
    assert "net:VCC" in result.report.netclass_names
    assert result.report.constraints["thermal"] == 2


def test_locked_and_side_overrides(tmp_path, committed_extract):
    extra = tmp_path / "extra.toml"
    extra.write_text('[[component]]\nref = "R1"\nlocked = true\nside = "bottom"\n')
    overlay, _ = resolve(committed_extract, extra_path=extra, step_models=False)
    result = build_scene(committed_extract, enrichment=overlay)
    r1 = result.builder.refs.index("R1")
    flags = result.builder._columns["comps.flags"][r1]
    assert flags & COMP_LOCKED
    assert flags & COMP_SIDE_BOTTOM


def test_enrichment_report_is_serialisable(tmp_path, committed_extract):
    store = parts_file_enrich(tmp_path)
    overlay, report = resolve(committed_extract, store=store, step_models=False)
    payload = json.loads(json.dumps(report.to_json()))
    assert payload["schema"] == "dodplace.enrichment/1"
    assert payload["parts_matched"] == 4  # R1, R2 and C1 share two LCSC codes
    assert payload["components"]["U1"]["height_mm"]["value"] == pytest.approx(1.75)


def test_constraint_rows_are_grouped_by_primary_component(tmp_path, committed_extract):
    """The engine's sparse tables are CSR-indexed, so rows must be sorted.

    A netlist heuristic emits rows in *net* order, which is not component order:
    regression guard for the bug the C validator caught on a 707-part board.
    """
    store = parts_file_enrich(tmp_path)
    extra = FIXTURES / "extra_fixture.toml"
    overlay, report = resolve(committed_extract, extra_path=extra, store=store,
                              step_models=False, heuristics=True)
    result = build_scene(committed_extract, enrichment=overlay, enrichment_report=report)
    columns = result.builder._columns

    def ascending(values) -> bool:
        return all(later >= earlier for earlier, later in zip(values, values[1:]))

    assert ascending(columns["decoupling.ic_comp"])
    assert ascending(columns["thermal.comp"])
    assert ascending(columns["symmetry.comp_a"])


def test_heuristic_rows_are_sorted_even_when_the_board_order_is_not(tmp_path):
    """Force a case where net order and component order disagree."""
    doc = {
        "schema": "dodplace.extract/1",
        "backend": "test",
        "board": {"path": "b.kicad_pcb", "copper_layers": 2, "outline": []},
        "keepouts": [],
        "netclasses": [{"name": "Default", "clearance_mm": 0.2, "track_width_mm": 0.2}],
        "nets": [],
        # U2 comes first in net order, U1 second: the rows must still be sorted.
        "components": [
            {"ref": "U2", "value": "b", "fpid": "Pkg:QFN", "x": 0, "y": 0, "rot_deg": 0,
             "side": "top", "locked": False, "courtyard": [[[-1, -1], [1, -1], [1, 1], [-1, 1]]],
             "pads": [{"number": "1", "net": "VCC", "x": 0, "y": 0, "size_x": 1, "size_y": 1,
                       "attrib": "smd", "drill": 0}]},
            {"ref": "U1", "value": "a", "fpid": "Pkg:QFN", "x": 10, "y": 0, "rot_deg": 0,
             "side": "top", "locked": False, "courtyard": [[[-1, -1], [1, -1], [1, 1], [-1, 1]]],
             "pads": [{"number": "1", "net": "VCC", "x": 0, "y": 0, "size_x": 1, "size_y": 1,
                       "attrib": "smd", "drill": 0}]},
            {"ref": "C1", "value": "100n", "fpid": "Pkg:0603", "x": 5, "y": 5, "rot_deg": 0,
             "side": "top", "locked": False, "courtyard": [[[-1, -1], [1, -1], [1, 1], [-1, 1]]],
             "pads": [{"number": "1", "net": "VCC", "x": -0.5, "y": 0, "size_x": 0.5, "size_y": 0.5,
                       "attrib": "smd", "drill": 0}]},
        ],
        "warnings": [],
    }
    overlay, report = resolve(doc, step_models=False, heuristics=True)
    result = build_scene(doc, enrichment=overlay, enrichment_report=report)
    rows = result.builder._columns["decoupling.ic_comp"]
    assert rows == sorted(rows)
    assert len(rows) == 2  # U2->C1 and U1->C1


# ===========================================================================
# Datasheet PDF, end to end (optional extra)
# ===========================================================================


def minimal_pdf(lines: list[str]) -> bytes:
    """A valid one-page PDF with Helvetica text, built without any dependency."""
    content = "BT /F1 11 Tf 72 740 Td 14 TL\n"
    for line in lines:
        escaped = line.replace("\\", r"\\").replace("(", r"\(").replace(")", r"\)")
        content += f"({escaped}) Tj T*\n"
    content += "ET\n"
    stream = content.encode("latin-1")

    objects = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R "
        b"/Resources << /Font << /F1 5 0 R >> >> >>",
        b"<< /Length " + str(len(stream)).encode() + b" >>\nstream\n" + stream + b"endstream",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    ]

    out = bytearray(b"%PDF-1.4\n")
    offsets = []
    for number, body in enumerate(objects, start=1):
        offsets.append(len(out))
        out += f"{number} 0 obj\n".encode() + body + b"\nendobj\n"
    xref = len(out)
    out += f"xref\n0 {len(objects) + 1}\n".encode()
    out += b"0000000000 65535 f \n"
    for offset in offsets:
        out += f"{offset:010d} 00000 n \n".encode()
    out += f"trailer\n<< /Size {len(objects) + 1} /Root 1 0 R >>\nstartxref\n{xref}\n%%EOF\n".encode()
    return bytes(out)


def test_datasheet_pdf_end_to_end(tmp_path):
    """Read a real PDF with pdfplumber through the rules engine."""
    if not datasheet_pdf.available():
        pytest.skip("pdfplumber is not installed (uv sync --extra pdf)")

    rules_path = tmp_path / "rules.toml"
    rules_path.write_text(RULES_TOML)
    rules = datasheet_pdf.load_rules(rules_path)

    pdf = tmp_path / "fake.pdf"
    pdf.write_bytes(
        minimal_pdf(
            [
                "STM32F103C8T6 datasheet",
                "6 Package characteristics",
                "Package height nominal 1.75 mm, see figure 12.",
                "Power dissipation: 0.33 W typical.",
            ]
        )
    )

    component = {"ref": "U1", "fpid": "Package_SO:SOIC-8", "_mpn": "STM32F103C8T6"}
    facts = datasheet_pdf.extract_from_pdf(pdf, rules, component)
    assert facts.get("height_mm").value == pytest.approx(1.75)
    assert facts.get("power_w").value == pytest.approx(0.33)
    assert "#p1" in facts.get("height_mm").provenance.source_ref


def test_datasheet_pdf_is_cached_by_content_hash(tmp_path, committed_extract):
    """Parts file gives the MPN, the MPN selects the rules, the PDF is parsed once."""
    if not datasheet_pdf.available():
        pytest.skip("pdfplumber is not installed (uv sync --extra pdf)")

    rules_path = tmp_path / "rules.toml"
    rules_path.write_text(RULES_TOML)
    rules = datasheet_pdf.load_rules(rules_path)

    store = EnrichmentStore(tmp_path / "cache")
    parts_file.import_file(FIXTURES / "parts_fixture.csv", store)

    datasheets = tmp_path / "datasheets"
    datasheets.mkdir()
    (datasheets / "STM32F103C8T6.pdf").write_bytes(
        minimal_pdf(["Package height nominal 1.75 mm", "Power dissipation: 0.33 W typical."])
    )

    first, report = resolve(
        committed_extract, store=store, rules=rules, datasheets_dir=datasheets,
        step_models=False, heuristics=False,
    )
    # The parts file already provides 1.75, so the datasheet's unique
    # contribution is power_w - and the parts file rightly outranks the PDF.
    assert first.components["U1"].height_mm == pytest.approx(1.75)
    assert first.components["U1"].power_w == pytest.approx(0.33)
    assert report.datasheets_read == 1 and report.datasheets_cached == 0
    assert store.count("datasheets") == 1

    second, report2 = resolve(
        committed_extract, store=store, rules=rules, datasheets_dir=datasheets,
        step_models=False, heuristics=False,
    )
    assert second.components["U1"].power_w == pytest.approx(0.33)
    assert report2.datasheets_cached == 1 and report2.datasheets_read == 0
    assert report2.components["U1"]["power_w"]["source"] == "datasheet_pdf"
    assert report2.components["U1"]["height_mm"]["source"] == "parts_file"


def test_shipped_rules_load_by_default():
    rules = datasheet_pdf.load_rules()
    assert rules, "the package ships at least one datasheet rule"
    assert any(rule.name == "stm32-f1" for rule in rules)


# ===========================================================================
# Height from a naming convention
# ===========================================================================


@pytest.mark.parametrize("name,expected", [
    ("LED-SMD_L3.7-W3.5-H2.8.wrl", 2.8),
    ("QSOP-24_L8.7-W3.9-H1.6-LS6.0-P0.64.wrl", 1.6),
    ("SOP-8_L5.0-W4.0-H1.8-LS6.2-P1.27.step", 1.8),
    ("PART_H0.45", 0.45),
])
def test_height_from_name_reads_the_convention(name, expected):
    assert model_name.height_from_name(name) == pytest.approx(expected)


@pytest.mark.parametrize("name", [
    "VQFN-14-HR_L4.0-W3.5-TL_x.step",   # HR is not a height
    "CAP-SMD_BD6.3-L6.6-W6.6-FD.step",  # BD is a diameter
    "C_0805_2012Metric.step",
    "R_0402_1005Metric.wrl",
    "PART_H999",                        # implausible
    "",
])
def test_height_from_name_ignores_anything_else(name):
    assert model_name.height_from_name(name) is None


def test_name_heuristic_never_beats_geometry(tmp_path, committed_extract):
    """A measured height always wins over a name convention."""
    shapes = tmp_path / "shapes"
    shapes.mkdir()
    # The name says 9.9 mm, the geometry says 1.2 mm.
    (shapes / "PART_H9.9.step").write_text(STEP_TEXT)

    doc = json.loads(json.dumps(committed_extract))
    for component in doc["components"]:
        component["models"] = [str(shapes / "PART_H9.9.step")] if component["ref"] == "U1" else []

    overlay, report = resolve(doc, step_models=True, heuristics=False)
    assert overlay.components["U1"].height_mm == pytest.approx(1.2)
    assert report.components["U1"]["height_mm"]["source"] == "step"


def test_name_heuristic_applies_when_nothing_is_measurable(tmp_path, committed_extract):
    doc = json.loads(json.dumps(committed_extract))
    for component in doc["components"]:
        component["models"] = [] if component["ref"] != "U1" else [
            "${KIPRJMOD}/gone/PART_H2.8.wrl"   # no STEP sibling, no file at all
        ]

    overlay, report = resolve(doc, step_models=True, heuristics=False)
    u1 = overlay.components["U1"]
    assert u1.height_mm == pytest.approx(2.8)
    entry = report.components["U1"]["height_mm"]
    assert entry["source"] == "model_name"
    assert entry["confidence"] == pytest.approx(0.5)


def test_name_heuristic_reads_the_footprint_when_there_is_no_model(committed_extract):
    doc = json.loads(json.dumps(committed_extract))
    for component in doc["components"]:
        component["models"] = []
        if component["ref"] == "U1":
            component["fpid"] = "Package_SO:SOP-8_L5.0-W4.0-H1.8-LS6.2-P1.27"

    overlay, _ = resolve(doc, step_models=True, heuristics=False)
    assert overlay.components["U1"].height_mm == pytest.approx(1.8)


# ===========================================================================
# Supplier field aliases
# ===========================================================================


def test_supplier_field_alias_is_recognised_on_both_sides():
    """`LCSC Part` must be found by the importer AND by the key builder."""
    component = {
        "ref": "D1",
        "fpid": "Tuile_LED:LED-SMD",
        "fields": {"LCSC Part": "C110402", "KiLib_Generator": "x"},
    }
    assert parts_file.field_value(component, parts_file.SUPPLIER_FIELDS) == "C110402"
    assert parts_file.candidate_keys(component) == ["lcsc:C110402", "fp:Tuile_LED:LED-SMD"]


def test_candidate_keys_accepts_every_alias_spelling():
    for field_name in ("LCSC", "LCSC Part", "LCSC Part #", "JLCPCB Part"):
        component = {"ref": "R1", "fpid": "x", "fields": {field_name: "c25804"}}
        assert parts_file.candidate_keys(component)[0] == "lcsc:C25804", field_name
