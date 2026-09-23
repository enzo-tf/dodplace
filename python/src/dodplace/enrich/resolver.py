"""Resolve the enrichment overlay from every available source.

The chain, strongest first (see ``model.SOURCE_RANK``)::

    extra.toml  >  parts file  >  board  >  netlist heuristic  >  datasheet  >  STEP

Sources are *additive*: each one fills what the previous ones left empty, so a
number is never overwritten silently and the report can always name the winner
and say what stayed unresolved. Whatever stays unresolved is left absent on
purpose - the engine then applies its documented fallback and raises the matching
DEGRADED_* bit, which keeps a single implementation of every fallback.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from ..ir.spec import PIN_GROUND, PIN_POWER
from .model import Fact, PartFacts, Provenance
from .overlay import (
    ComponentEnrichment,
    DecouplingRow,
    EnrichmentOverlay,
    ThermalRow,
)
from .providers import datasheet_pdf, extra_toml, model_name, parts_file, step_model
from .store import EnrichmentStore

#: Fields the report tracks per component.
TRACKED_FIELDS = ("height_mm", "mass_g", "orientation_deg", "power_w", "r_theta_ja")

#: Facts that map onto a ComponentEnrichment attribute as a plain float.
_NUMERIC_FIELDS = ("height_mm", "mass_g", "power_w", "r_theta_ja", "exclusion_radius_mm")


@dataclass
class Resolution:
    value: Any
    source: str
    source_ref: str = ""
    confidence: float = 1.0

    def to_json(self) -> dict:
        payload = {
            "value": self.value,
            "source": self.source,
            "confidence": round(self.confidence, 3),
        }
        if self.source_ref:
            payload["source_ref"] = self.source_ref
        return payload


@dataclass
class ResolveReport:
    """Where every resolved field came from, and what stayed unresolved."""

    components: dict[str, dict[str, dict]] = field(default_factory=dict)
    unresolved: dict[str, int] = field(default_factory=dict)
    sources: dict[str, int] = field(default_factory=dict)
    warnings: list[str] = field(default_factory=list)
    parts_matched: int = 0
    datasheets_read: int = 0
    datasheets_cached: int = 0
    step_models_read: int = 0
    names_read: int = 0

    def record(self, ref: str, name: str, resolution: Resolution | None) -> None:
        entry = self.components.setdefault(ref, {})
        if resolution is None:
            entry[name] = {"unresolved": True}
            self.unresolved[name] = self.unresolved.get(name, 0) + 1
            return
        entry[name] = resolution.to_json()
        self.sources[resolution.source] = self.sources.get(resolution.source, 0) + 1

    def to_json(self) -> dict:
        return {
            "schema": "dodplace.enrichment/1",
            "components": self.components,
            "unresolved": self.unresolved,
            "sources": self.sources,
            "parts_matched": self.parts_matched,
            "datasheets_read": self.datasheets_read,
            "datasheets_cached": self.datasheets_cached,
            "step_models_read": self.step_models_read,
            "names_read": self.names_read,
            "warnings": self.warnings,
        }


def _keep_strongest(candidates: dict[str, Fact], name: str, fact: Fact) -> None:
    """Keep the highest-precedence fact per field, whatever order sources run in."""
    existing = candidates.get(name)
    if existing is None or fact.provenance.rank < existing.provenance.rank:
        candidates[name] = fact


def _is_capacitor(component: dict) -> bool:
    ref = str(component.get("ref", ""))
    return ref[:1].upper() == "C" and ref[1:2].isdigit() or "Capacitor" in str(
        component.get("fpid", "")
    )


def _net_is_power(name: str) -> bool:
    from ..kicad.naming import is_ground, is_power

    return is_power(name) and not is_ground(name)


def decoupling_from_netlist(doc: dict, *, max_distance_mm: float = 5.0) -> list[DecouplingRow]:
    """Pair every decoupling capacitor with the ICs sharing its power net.

    This is the engine-input side of the matrix's documented fallback
    ("rapprochement heuristique par netlist"): no datasheet needed, only the
    footprint reference convention and the net names.
    """
    components = doc["components"]
    by_net: dict[str, list[tuple[int, dict]]] = {}
    for index, component in enumerate(components):
        for pad in component.get("pads", []):
            net = pad.get("net")
            if net:
                by_net.setdefault(net, []).append((index, pad))

    rows: list[DecouplingRow] = []
    seen: set[tuple[str, str]] = set()
    for net, members in sorted(by_net.items()):
        if not _net_is_power(net):
            continue
        caps = [(index, pad) for index, pad in members if _is_capacitor(components[index])]
        ics = [(index, pad) for index, pad in members if not _is_capacitor(components[index])]
        for ic_index, ic_pad in ics:
            for cap_index, _cap_pad in caps:
                if ic_index == cap_index:
                    continue
                key = (components[ic_index]["ref"], components[cap_index]["ref"])
                if key in seen:
                    continue
                seen.add(key)
                rows.append(
                    DecouplingRow(
                        ic=key[0],
                        cap=key[1],
                        pin=str(ic_pad.get("number", "")) or None,
                        max_distance_mm=max_distance_mm,
                        weight=1.0,
                    )
                )
    return rows


def _find_datasheet(directory: Path, candidates: list[str]) -> Path | None:
    if not directory.is_dir():
        return None
    for candidate in candidates:
        if not candidate:
            continue
        direct = directory / f"{candidate}.pdf"
        if direct.is_file():
            return direct
        for path in directory.glob("*.pdf"):
            if path.stem.lower() == candidate.lower():
                return path
    return None


def resolve(
    doc: dict,
    *,
    extra_path: str | Path | None = None,
    store: EnrichmentStore | None = None,
    rules: list[datasheet_pdf.Rule] | None = None,
    datasheets_dir: str | Path | None = None,
    board_path: str | Path | None = None,
    step_models: bool = True,
    heuristics: bool = True,
) -> tuple[EnrichmentOverlay, ResolveReport]:
    """Build the overlay by walking the precedence chain once per component."""
    report = ResolveReport()
    #: Identical STEP problems repeat once per component; warn once.
    _warned: set[str] = set()
    extra = extra_toml.parse(extra_path) if extra_path else extra_toml.ExtraConfig()
    overlay = extra.overlay
    # Glob selectors live in the file only: resolve them against this board so
    # everything downstream sees concrete per-reference entries.
    extra.materialize(doc["components"])
    overlay.merge_warnings(report.warnings)

    if step_models:
        roots = step_model.model_roots()
        if not roots:
            overlay.warnings.append(
                "no KiCad 3D model directory found: STEP heights are unavailable"
            )
        report.warnings.extend(overlay.warnings[-1:])

    if board_path is None:
        board_path = doc.get("board", {}).get("path", "")
    datasheets_dir = Path(datasheets_dir) if datasheets_dir else None
    pdf_ready = datasheets_dir is not None and datasheet_pdf.available() and rules

    for component in doc["components"]:
        ref = component["ref"]
        entry = overlay.component(ref)
        # extra.toml is applied when the entry is created: remember what it
        # supplied so the report attributes those fields to it, not to whichever
        # lower-precedence source happens to carry the same field.
        from_extra = {
            name: getattr(entry, name) for name in TRACKED_FIELDS if getattr(entry, name) is not None
        }
        candidates: dict[str, Fact] = {}
        pin_roles: dict[str, Fact] = {}
        pin_currents: dict[str, Fact] = {}

        # --- parts file / store -------------------------------------------
        mpn = manufacturer = ""
        if store is not None:
            keys = parts_file.candidate_keys(component)
            hit = store.get_first("parts", keys) if keys else None
            if hit is not None:
                key, record = hit
                report.parts_matched += 1
                for name, fact in record.facts.items():
                    _keep_strongest(candidates, name, fact)
                pin_roles.update(record.pin_roles)
                pin_currents.update(record.pin_currents)
                fact = record.get("mpn")
                mpn = str(fact.value) if fact else ""
                fact = record.get("manufacturer")
                manufacturer = str(fact.value) if fact else ""
                _record_pin_resolutions(report, ref, record)

        match_info = {**component, "_mpn": mpn, "_manufacturer": manufacturer}

        # --- datasheet PDF -------------------------------------------------
        if pdf_ready and len(candidates) < len(TRACKED_FIELDS) + 3:
            datasheet = _find_datasheet(
                datasheets_dir,
                [mpn, _field_text(component, "LCSC"), component.get("value", ""), ref],
            )
            if datasheet is not None:
                facts, from_cache = _datasheet_facts(datasheet, rules, match_info, store)
                if facts is not None and facts.facts:
                    if from_cache:
                        report.datasheets_cached += 1
                    else:
                        report.datasheets_read += 1
                    for name, fact in facts.facts.items():
                        _keep_strongest(candidates, name, fact)

        # --- STEP model ----------------------------------------------------
        if step_models and "height_mm" not in candidates:
            for model in component.get("models", []):
                # Prefers a STEP sibling over a referenced VRML model; returns
                # None when only unmeasurable geometry is available.
                path = step_model.resolve_step_path(model, board_path)
                if path is None:
                    continue
                try:
                    height = step_model.height_mm(path)
                except step_model.StepModelError as exc:
                    message = str(exc)
                    if message not in _warned:
                        _warned.add(message)
                        report.warnings.append(message)
                    continue
                if height > 0.0:
                    report.step_models_read += 1
                    _keep_strongest(
                        candidates, "height_mm", Fact(height, Provenance("step", path.name, 0.9))
                    )
                break

        # --- dimensions encoded in a name -----------------------------------
        # Last resort, and only for height: no geometry was readable, so a
        # convention beats "unknown" as long as the report says so.
        if step_models and "height_mm" not in candidates:
            fact = model_name.height_fact(component)
            if fact is not None:
                report.names_read += 1
                _keep_strongest(candidates, "height_mm", fact)

        # --- apply, strongest source first ---------------------------------
        for name, fact in candidates.items():
            _apply(entry, name, fact)
        for pad, fact in pin_roles.items():
            entry.pin_roles.setdefault(pad, _role_flag(str(fact.value)))
        for pad, fact in pin_currents.items():
            entry.pin_currents.setdefault(pad, float(fact.value))

        for name in TRACKED_FIELDS:
            report.record(ref, name, _resolution_of(entry, name, from_extra, candidates))

    # --- thermal rows from resolved per-component data --------------------
    for ref, entry in overlay.components.items():
        if entry.power_w is None:
            continue
        if any(row.component == ref for row in overlay.thermal):
            continue  # an explicit [[thermal]] row wins
        overlay.thermal.append(
            ThermalRow(
                component=ref,
                power_w=entry.power_w,
                radius_mm=entry.exclusion_radius_mm or 3.0,
                r_theta_ja=entry.r_theta_ja or 0.0,
            )
        )

    # --- netlist heuristic, only where the user said nothing --------------
    if heuristics and not overlay.decoupling:
        rows = decoupling_from_netlist(doc, max_distance_mm=5.0)
        overlay.decoupling = rows
        if rows:
            report.sources["netlist_heuristic"] = (
                report.sources.get("netlist_heuristic", 0) + len(rows)
            )

    overlay.merge_warnings(report.warnings)
    return overlay, report


def _field_text(component: dict, field_name: str) -> str:
    for name, value in component.get("fields", {}).items():
        if name.lower() == field_name.lower():
            return str(value)
    return ""


def _role_flag(name: str) -> int:
    from ..ir.spec import PIN_CLOCK, PIN_DIFF_N, PIN_DIFF_P, PIN_GROUND, PIN_POWER

    return {
        "PIN_POWER": PIN_POWER,
        "PIN_GROUND": PIN_GROUND,
        "PIN_DIFF_P": PIN_DIFF_P,
        "PIN_DIFF_N": PIN_DIFF_N,
        "PIN_CLOCK": PIN_CLOCK,
    }.get(name, 0)


def _datasheet_facts(
    path: Path,
    rules: list[datasheet_pdf.Rule],
    component: dict,
    store: EnrichmentStore | None,
) -> tuple[PartFacts | None, bool]:
    """Read a datasheet, caching the extracted facts by content hash.

    Returns the facts and whether they came from the cache. A content hash means
    a re-run, or another project with the same document, parses nothing.
    """
    from .store import content_key, now_iso

    try:
        payload = path.read_bytes()
    except OSError:
        return None, False
    digest = content_key(payload)
    if store is not None:
        cached = store.get("datasheets", digest)
        if cached is not None:
            return cached, True
    try:
        facts = datasheet_pdf.extract_from_pdf(path, rules, component)
    except datasheet_pdf.DatasheetError:
        return None, False
    if store is not None and facts.facts:
        for fact in facts.facts.values():
            if not fact.provenance.fetched_at:
                fact.provenance.fetched_at = now_iso()
        store.put("datasheets", digest, facts)
    return facts, False


def _record_pin_resolutions(report: ResolveReport, ref: str, record: PartFacts) -> None:
    for pad, fact in record.pin_roles.items():
        report.sources[fact.provenance.source] = report.sources.get(fact.provenance.source, 0) + 1
        report.components.setdefault(ref, {})[f"pin_role.{pad}"] = Resolution(
            fact.value, fact.provenance.source, fact.provenance.source_ref, fact.provenance.confidence
        ).to_json()
    for pad, fact in record.pin_currents.items():
        report.sources[fact.provenance.source] = report.sources.get(fact.provenance.source, 0) + 1
        report.components.setdefault(ref, {})[f"pin_current.{pad}"] = Resolution(
            fact.value, fact.provenance.source, fact.provenance.source_ref, fact.provenance.confidence
        ).to_json()


def _apply(entry: ComponentEnrichment, name: str, fact: Fact) -> None:
    """Write a resolved fact into the overlay, never overwriting a stronger one.

    ``entry`` already holds the extra.toml values (rank 0), so anything set
    here only fills a gap - which is exactly what the chain means.
    """
    if name == "mpn":
        if entry.part_number is None:
            entry.part_number = str(fact.value)
        return
    if name not in _NUMERIC_FIELDS:
        return  # manufacturer / package / datasheet_url are reporting only
    if getattr(entry, name, None) is not None:
        return
    try:
        setattr(entry, name, float(fact.value))
    except (TypeError, ValueError):
        return  # a non-numeric value in a numeric field is a bad record, not data


def _resolution_of(
    entry: ComponentEnrichment,
    name: str,
    from_extra: dict[str, Any],
    candidates: dict[str, Fact],
) -> Resolution | None:
    """Which source won this field, for the report."""
    value = getattr(entry, name, None)
    if value is None:
        return None
    if name in from_extra:
        return Resolution(value, "extra_toml", "", 1.0)
    fact = candidates.get(name)
    if fact is not None:
        return Resolution(
            value,
            fact.provenance.source,
            fact.provenance.source_ref,
            fact.provenance.confidence,
        )
    return Resolution(value, "board", "", 1.0)
