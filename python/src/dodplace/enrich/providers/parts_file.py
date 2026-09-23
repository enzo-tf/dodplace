"""Board parts file: the join between the board and real part data.

Two formats are accepted, and both are *user supplied files* - nothing is
downloaded implicitly:

* **CSV** - the shape a JLCPCB/LCSC BOM export has: one row per line item with
  a reference list, a value, a footprint and a supplier code.
* **TOML** - the same records with dimensions, currents and thermal data, for
  the fields a BOM export cannot carry.

Records land in the store under a namespaced identity, so lookups never need a
secondary index:

    lcsc:C25804            from an ``LCSC`` footprint field or a BOM column
    mpn:RC0603FR-0710KL    from an ``MPN`` field or a BOM column
    fp:<library>:<name>    footprint fallback, for coarse package data
"""

from __future__ import annotations

import csv
import tomllib
from dataclasses import dataclass, field
from pathlib import Path

from ..model import PIN_ROLE_NAMES, Fact, PartFacts, Provenance
from ..store import EnrichmentStore

SOURCE = "parts_file"

#: Normalised header aliases -> canonical field name.
CSV_ALIASES = {
    "lcsc": "lcsc",
    "lcscpart": "lcsc",
    "lcscpartnumber": "lcsc",
    "lcscno": "lcsc",
    "jlcpcbpart": "lcsc",
    "jlcpcb": "lcsc",
    "mpn": "mpn",
    "manufacturerpartnumber": "mpn",
    "mfrpart": "mpn",
    "partnumber": "mpn",
    "manufacturer": "manufacturer",
    "mfr": "manufacturer",
    "comment": "value",
    "value": "value",
    "description": "value",
    "designator": "designators",
    "designators": "designators",
    "refs": "designators",
    "reference": "designators",
    "footprint": "footprint",
    "package": "footprint",
    "height": "height_mm",
    "heightmm": "height_mm",
    "mass": "mass_g",
    "massg": "mass_g",
    "weight": "mass_g",
    "power": "power_w",
    "powerw": "power_w",
    "rtheta": "r_theta_ja",
    "rthetaja": "r_theta_ja",
    "datasheet": "datasheet_url",
    "datasheeturl": "datasheet_url",
}

#: Footprint field names that carry a supplier part number. Shared by the
#: importer, the skeleton generator and `candidate_keys`, so a field recognised
#: on one side is recognised on the other.
SUPPLIER_FIELDS = ("lcsc part", "lcsc part #", "lcsc", "jlcpcb part", "jlcpcb")

#: Field names that carry a manufacturer part number.
MPN_FIELDS = ("mpn", "manufacturer part number", "manufacturer pn", "mfr part",
              "part number")

#: Which canonical fields become scalar facts.
SCALAR_COLUMNS = ("height_mm", "mass_g", "power_w", "r_theta_ja", "mpn",
                  "manufacturer", "package", "datasheet_url")


class PartsFileError(ValueError):
    """The parts file cannot be read."""


#: Names auto-discovered next to a board, in order. `enrich skeleton` writes the
#: first one.
PARTS_FILE_NAMES = ("dodplace-parts.csv", "dodplace-parts.toml")


def discover(board_path: str | Path | None) -> Path | None:
    """A parts file sitting next to the board, if there is one."""
    if not board_path:
        return None
    directory = Path(board_path).resolve().parent
    for name in PARTS_FILE_NAMES:
        candidate = directory / name
        if candidate.is_file():
            return candidate
    return None


@dataclass
class ImportReport:
    source: str
    records: int = 0
    identities: list[str] = field(default_factory=list)
    skipped: list[str] = field(default_factory=list)

    def summary(self) -> str:
        text = f"{self.records} record(s) from {self.source}"
        if self.skipped:
            text += f", {len(self.skipped)} row(s) skipped"
        return text


def _normalise(header: str) -> str:
    return "".join(ch for ch in header.lower() if ch.isalnum())


def field_value(component: dict, names: tuple[str, ...]) -> str:
    """First non-empty footprint field whose name matches one of ``names``.

    Matching is case-insensitive and ignores surrounding spaces, because the
    same datum is spelled ``LCSC``, ``LCSC Part`` or ``LCSC Part #`` depending on
    who wrote the library.
    """
    for field_name, value in component.get("fields", {}).items():
        if field_name.strip().lower() in names and str(value).strip():
            return str(value).strip()
    return ""


def _identity(lcsc: str | None, mpn: str | None, footprint: str | None) -> str | None:
    if lcsc:
        code = lcsc.strip().upper()
        if code and not code.startswith("C"):
            code = "C" + code
        if code:
            return f"lcsc:{code}"
    if mpn:
        return f"mpn:{mpn.strip()}"
    if footprint:
        return f"fp:{footprint.strip()}"
    return None


def candidate_keys(component: dict) -> list[str]:
    """Store keys to try for a component, strongest first.

    Uses the same field aliases as the importer: a footprint carrying
    ``LCSC Part`` (with a space) must match a record imported from a BOM whose
    column is ``LCSC Part #``.
    """
    keys: list[str] = []
    identity = _identity(
        field_value(component, SUPPLIER_FIELDS),
        field_value(component, MPN_FIELDS),
        None,
    )
    if identity:
        keys.append(identity)
    if component.get("fpid"):
        keys.append(f"fp:{component['fpid']}")
    return keys


def import_file(path: str | Path, store: EnrichmentStore, *, confidence: float = 1.0) -> ImportReport:
    """Import a CSV or TOML parts file into the store."""
    path = Path(path)
    if not path.is_file():
        raise PartsFileError(f"parts file not found: {path}")
    if path.suffix.lower() == ".toml":
        return import_toml(path, store, confidence=confidence)
    if path.suffix.lower() in (".csv", ".tsv", ".txt"):
        return import_csv(path, store, confidence=confidence)
    raise PartsFileError(f"unsupported parts file extension {path.suffix!r} (use .csv or .toml)")


def import_csv(
    path: Path, store: EnrichmentStore, *, confidence: float = 1.0
) -> ImportReport:
    report = ImportReport(source=path.name)
    text = path.read_text()

    # `#` lines are comments, wherever they are: the generated skeleton starts
    # with a few, and a hand-edited parts file will grow its own.
    lines = [
        line for line in text.splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ]
    if not lines:
        raise PartsFileError(f"{path.name}: no data rows")
    sample = "\n".join(lines[:20])
    try:
        dialect = csv.Sniffer().sniff(sample, delimiters=",;\t")
    except csv.Error:
        dialect = csv.excel
    reader = csv.DictReader(lines, dialect=dialect)
    fieldnames = reader.fieldnames or []
    rows = list(reader)
    if not rows:
        raise PartsFileError(f"{path.name}: no data rows")
    if not fieldnames:
        raise PartsFileError(f"{path.name}: no header row")

    columns = {_normalise(name or ""): name for name in fieldnames}
    unknown = [name for name in columns if name not in CSV_ALIASES]
    for name in unknown:
        report.skipped.append(f"ignored column {name!r}")
    mapping = {
        canonical: columns[normalised]
        for normalised, canonical in CSV_ALIASES.items()
        if normalised in columns
    }
    if not {"lcsc", "mpn", "footprint"} & set(mapping):
        raise PartsFileError(f"{path.name}: no LCSC, MPN or footprint column")

    provenance = Provenance(source=SOURCE, source_ref=path.name, confidence=confidence)
    for row in rows:
        values = {canonical: (row.get(column) or "").strip() for canonical, column in mapping.items()}
        identity = _identity(values.get("lcsc"), values.get("mpn"), values.get("footprint"))
        if not identity:
            reference = values.get("designators") or "?"
            report.skipped.append(f"row without part identity ({reference})")
            continue
        record = PartFacts(identity=identity)
        for name in SCALAR_COLUMNS:
            value = values.get(name)
            if not value:
                continue
            if name in ("height_mm", "mass_g", "power_w", "r_theta_ja"):
                try:
                    record.set(name, float(value), provenance)
                except ValueError:
                    report.skipped.append(f"{identity}: {name}={value!r} is not a number")
            else:
                record.set(name, value, provenance)
        store.merge("parts", identity, record)
        report.records += 1
        report.identities.append(identity)
    return report


def import_toml(
    path: Path, store: EnrichmentStore, *, confidence: float = 1.0
) -> ImportReport:
    report = ImportReport(source=path.name)
    try:
        document = tomllib.loads(path.read_text())
    except tomllib.TOMLDecodeError as exc:
        raise PartsFileError(f"{path.name}: {exc}") from exc

    parts = document.get("parts", document)
    if not isinstance(parts, dict):
        raise PartsFileError(f"{path.name}: [parts] must be a table")
    provenance = Provenance(source=SOURCE, source_ref=path.name, confidence=confidence)

    for key, table in parts.items():
        if not isinstance(table, dict):
            report.skipped.append(f"{key}: not a table")
            continue
        identity = key if ":" in str(key) else _identity(table.get("lcsc"), key, None)
        if identity is None:
            report.skipped.append(f"{key}: no identity")
            continue
        record = PartFacts(identity=identity)
        for name in SCALAR_COLUMNS:
            if name not in table:
                continue
            value = table[name]
            if name in ("height_mm", "mass_g", "power_w", "r_theta_ja"):
                try:
                    value = float(value)
                except (TypeError, ValueError):
                    report.skipped.append(f"{identity}: {name}={value!r} is not a number")
                    continue
            record.set(name, value, provenance)

        roles = table.get("pin_roles", {})
        if roles:
            if not isinstance(roles, dict):
                report.skipped.append(f"{identity}: pin_roles must be a table")
            else:
                for pin, role in roles.items():
                    name = str(role).lower()
                    if name not in PIN_ROLE_NAMES:
                        report.skipped.append(f"{identity}: unknown pin role {role!r}")
                        continue
                    record.pin_roles[str(pin)] = Fact(PIN_ROLE_NAMES[name], provenance)
        currents = table.get("pin_currents", {})
        if currents:
            if not isinstance(currents, dict):
                report.skipped.append(f"{identity}: pin_currents must be a table")
            else:
                for pin, current in currents.items():
                    try:
                        record.pin_currents[str(pin)] = Fact(float(current), provenance)
                    except (TypeError, ValueError):
                        report.skipped.append(f"{identity}: current {current!r} is not a number")
        store.merge("parts", identity, record)
        report.records += 1
        report.identities.append(identity)
    return report
