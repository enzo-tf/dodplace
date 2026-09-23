"""Content-addressed JSON store for enrichment facts.

One file per key, sharded two levels deep, written atomically (temp + rename).
No SQL, no schema migrations, nothing opaque: a record is a file you can read,
grep and delete. Disjoint keys never conflict, so a GUI plugin and a CLI run can
both write to it, and the store stays trivially portable and disposable.

Layout::

    <root>/parts/<ab>/<sha256(identity)>.json
    <root>/datasheets/<ab>/<sha256(sha)>.json
    <root>/steps/<ab>/<sha256(sha)>.json

Keys are namespaced identities (``lcsc:C25804``, ``mpn:...``, ``fp:...``) for
parts and content hashes for files. The store holds *facts*, never the source
documents: no PDF and no STEP file is ever copied into it.
"""

from __future__ import annotations

import hashlib
import json
import os
import tempfile
from collections.abc import Iterator
from datetime import datetime, timezone
from pathlib import Path

from .model import Fact, PartFacts, Provenance

KINDS = ("parts", "datasheets", "steps")


def content_key(payload: bytes | str) -> str:
    """Hash a source document; the cache key for anything fetched by content."""
    data = payload.encode() if isinstance(payload, str) else payload
    return hashlib.sha256(data).hexdigest()


def now_iso() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat()


class EnrichmentStore:
    """A directory of JSON records, one per key."""

    def __init__(self, root: str | Path):
        self.root = Path(root)

    # --- addressing -------------------------------------------------------

    def path_for(self, kind: str, key: str) -> Path:
        if kind not in KINDS:
            raise ValueError(f"unknown store kind {kind!r}; expected one of {KINDS}")
        digest = content_key(key)
        return self.root / kind / digest[:2] / f"{digest}.json"

    # --- reading ----------------------------------------------------------

    def get(self, kind: str, key: str) -> PartFacts | None:
        path = self.path_for(kind, key)
        if not path.is_file():
            return None
        try:
            payload = json.loads(path.read_text())
        except (OSError, json.JSONDecodeError):
            return None  # a corrupt record is a miss, never a crash
        record = PartFacts.from_json(payload)
        record.identity = key
        return record

    def get_first(self, kind: str, keys: list[str]) -> tuple[str, PartFacts] | None:
        """First hit among candidate keys, tried in order."""
        for key in keys:
            record = self.get(kind, key)
            if record is not None:
                return key, record
        return None

    def keys(self, kind: str) -> Iterator[str]:
        base = self.root / kind
        if not base.is_dir():
            return
        for path in sorted(base.glob("*/*.json")):
            try:
                yield json.loads(path.read_text())["identity"]
            except (OSError, json.JSONDecodeError, KeyError):
                continue

    def count(self, kind: str) -> int:
        base = self.root / kind
        return len(list(base.glob("*/*.json"))) if base.is_dir() else 0

    # --- writing ----------------------------------------------------------

    def put(self, kind: str, key: str, facts: PartFacts) -> Path:
        path = self.path_for(kind, key)
        path.parent.mkdir(parents=True, exist_ok=True)
        payload = json.dumps(facts.to_json(), indent=1, sort_keys=True)
        # Atomic: a reader either sees the previous record or the new one.
        with tempfile.NamedTemporaryFile(
            "w", dir=path.parent, prefix=".tmp-", delete=False
        ) as handle:
            handle.write(payload)
            handle.write("\n")
            temporary = Path(handle.name)
        os.replace(temporary, path)
        return path

    def invalidate(self, kind: str, key: str) -> bool:
        path = self.path_for(kind, key)
        if path.is_file():
            path.unlink()
            return True
        return False

    def merge(self, kind: str, key: str, incoming: PartFacts) -> PartFacts:
        """Merge ``incoming`` into the stored record, incoming facts winning.

        Used when several sources describe the same part (a CSV for identity, a
        TOML for dimensions): the newest write wins per field, and every field
        keeps the provenance of whoever supplied it.
        """
        existing = self.get(kind, key) or PartFacts(identity=key)
        for name, fact in incoming.facts.items():
            existing.facts[name] = fact
        for pin, fact in incoming.pin_roles.items():
            existing.pin_roles[pin] = fact
        for pin, fact in incoming.pin_currents.items():
            existing.pin_currents[pin] = fact
        self.put(kind, key, existing)
        return existing


def make_fact(value, source: str, source_ref: str = "", confidence: float = 1.0) -> Fact:
    return Fact(value, Provenance(source=source, source_ref=source_ref, confidence=confidence))
