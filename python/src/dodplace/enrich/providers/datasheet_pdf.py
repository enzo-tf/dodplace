"""Datasheet PDFs: dimensions, currents and thermal data from a datasheet.

Extraction is rule driven, rules live in TOML (stdlib parser, no new
dependency) and each hit records a confidence *and* the page it came from, so a
number can always be traced back to the document. A rule that misses leaves the
field unresolved: the engine's degraded mode is always preferable to a number
guessed from the wrong table.

Rules file (``python/src/dodplace/enrich/rules/*.toml``)::

    [[rule]]
    name = "STM32F1"
    mpn_prefix = "STM32F1"
    [[rule.field]]
    name = "height_mm"
    pattern = "Package(?: height| thickness)[^0-9]{0,12}([0-9.]+)\\s*mm"
    confidence = 0.6

``pdfplumber`` is an optional extra (``uv sync --extra pdf``); the text
extraction is kept separate from the PDF reading so the rule engine is testable
without it.
"""

from __future__ import annotations

import re
import tomllib
from dataclasses import dataclass, field
from pathlib import Path

from ..model import Fact, PartFacts, Provenance

SOURCE = "datasheet_pdf"
DEFAULT_RULE_DIR = Path(__file__).resolve().parent.parent / "rules"

#: Fields a rule may extract, and how the value is parsed.
FIELD_NAMES = (
    "height_mm",
    "mass_g",
    "power_w",
    "r_theta_ja",
    "exclusion_radius_mm",
    "mpn",
    "manufacturer",
    "package",
)


class DatasheetError(ValueError):
    """The rules file or document cannot be read."""


def available() -> bool:
    """True when the optional PDF reader is installed."""
    try:
        import pdfplumber  # noqa: F401
    except ImportError:
        return False
    return True


@dataclass
class FieldRule:
    name: str
    pattern: re.Pattern[str]
    confidence: float = 0.6


@dataclass
class Rule:
    name: str
    mpn_prefix: str | None = None
    fpid_prefix: str | None = None
    value_pattern: str | None = None
    manufacturer: str | None = None
    fields: list[FieldRule] = field(default_factory=list)

    def matches(self, component: dict, facts: PartFacts | None = None) -> bool:
        mpn = component.get("_mpn") or ""
        manufacturer = component.get("_manufacturer") or ""
        if facts is not None:
            fact = facts.get("mpn")
            if fact and not mpn:
                mpn = str(fact.value)
            fact = facts.get("manufacturer")
            if fact and not manufacturer:
                manufacturer = str(fact.value)
        if self.mpn_prefix and not mpn.upper().startswith(self.mpn_prefix.upper()):
            return False
        if self.manufacturer and self.manufacturer.lower() not in manufacturer.lower():
            return False
        if self.fpid_prefix and not str(component.get("fpid", "")).startswith(self.fpid_prefix):
            return False
        if self.value_pattern:
            haystack = f"{component.get('value', '')} {component.get('fpid', '')}"
            if not re.search(self.value_pattern, haystack, re.IGNORECASE):
                return False
        return bool(self.mpn_prefix or self.fpid_prefix or self.value_pattern or self.manufacturer)


def load_rules(*paths: str | Path) -> list[Rule]:
    """Load rule files; with no argument, every TOML under ``rules/``."""
    if not paths:
        paths = sorted(DEFAULT_RULE_DIR.glob("*.toml")) if DEFAULT_RULE_DIR.is_dir() else []
    rules: list[Rule] = []
    for path in paths:
        path = Path(path)
        try:
            document = tomllib.loads(path.read_text())
        except (OSError, tomllib.TOMLDecodeError) as exc:
            raise DatasheetError(f"cannot read rules {path}: {exc}") from exc
        for entry in document.get("rule", []):
            if not isinstance(entry, dict) or "name" not in entry:
                raise DatasheetError(f"{path.name}: every [[rule]] needs a name")
            fields = []
            for spec in entry.get("field", []):
                if not isinstance(spec, dict) or "name" not in spec or "pattern" not in spec:
                    raise DatasheetError(f"{path.name}: [[rule.field]] needs name and pattern")
                if spec["name"] not in FIELD_NAMES:
                    raise DatasheetError(
                        f"{path.name}: unknown field {spec['name']!r}; "
                        f"expected one of {', '.join(FIELD_NAMES)}"
                    )
                fields.append(
                    FieldRule(
                        name=spec["name"],
                        pattern=re.compile(spec["pattern"]),
                        confidence=float(spec.get("confidence", 0.6)),
                    )
                )
            if not fields:
                raise DatasheetError(f"{path.name}: rule {entry['name']!r} has no fields")
            rules.append(
                Rule(
                    name=str(entry["name"]),
                    mpn_prefix=entry.get("mpn_prefix"),
                    fpid_prefix=entry.get("fpid_prefix"),
                    value_pattern=entry.get("value_pattern"),
                    manufacturer=entry.get("manufacturer"),
                    fields=fields,
                )
            )
    return rules


def extract_from_text(
    text: str,
    rules: list[Rule],
    component: dict,
    *,
    source_ref: str,
    page: int | None = None,
) -> PartFacts:
    """Apply every matching rule to ``text`` and return the facts found.

    Pure function over text: this is what the tests exercise, with PDF reading
    kept behind :func:`extract_from_pdf`.
    """
    identity = f"fp:{component.get('fpid', component.get('ref', '?'))}"
    record = PartFacts(identity=identity)
    for rule in rules:
        if not rule.matches(component):
            continue
        for spec in rule.fields:
            match = spec.pattern.search(text)
            if not match:
                continue
            raw = match.group(1) if match.groups() else match.group(0)
            try:
                value = float(raw) if spec.name in FIELD_NAMES[:5] else raw.strip()
            except ValueError:
                continue
            if spec.name in FIELD_NAMES[:5] and value <= 0.0:
                continue  # a non-positive dimension is a parse error, not data
            reference = f"{source_ref}#p{page}" if page else source_ref
            snippet = " ".join(match.group(0).split())[:60]
            record.set(
                spec.name,
                value,
                Provenance(
                    source=SOURCE,
                    source_ref=f"{reference} ({snippet})",
                    confidence=spec.confidence,
                ),
            )
    return record


def extract_from_pdf(
    path: str | Path,
    rules: list[Rule],
    component: dict,
    *,
    max_pages: int | None = None,
) -> PartFacts:
    """Read ``path`` with pdfplumber and apply the rules page by page."""
    try:
        import pdfplumber
    except ImportError as exc:  # pragma: no cover - optional extra
        raise DatasheetError(
            "pdfplumber is not installed; run `uv sync --extra pdf`"
        ) from exc

    path = Path(path)
    if not path.is_file():
        raise DatasheetError(f"datasheet not found: {path}")
    merged = PartFacts(identity=f"fp:{component.get('fpid', component.get('ref', '?'))}")
    with pdfplumber.open(str(path)) as pdf:
        for number, page in enumerate(pdf.pages, start=1):
            if max_pages is not None and number > max_pages:
                break
            text = page.extract_text() or ""
            if not text:
                continue
            found = extract_from_text(text, rules, component, source_ref=path.name, page=number)
            for name, fact in found.facts.items():
                if name not in merged.facts:
                    merged.facts[name] = fact
            if len(merged.facts) == len(FIELD_NAMES):
                break
    return merged
