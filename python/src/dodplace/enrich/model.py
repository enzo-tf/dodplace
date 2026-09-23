"""Facts, provenance and the resolved overlay handed to the IR builder.

Everything the enrichment layer learns about a part is a :class:`Fact`: a value
plus where it came from and how much it can be trusted. Nothing is ever
overwritten in place - the resolver picks the strongest source and the report
records the loser too, so a surprising placement can always be explained.

Precedence is a total order over sources; ``SOURCE_RANK`` is the single place
that defines it.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any

#: Lower rank wins. User rules beat every machine-derived source.
SOURCE_RANK: dict[str, int] = {
    "extra_toml": 0,
    "parts_file": 1,
    "board": 2,
    "netlist_heuristic": 3,
    "datasheet_pdf": 4,
    "step": 5,
    "model_name": 6,  # a naming convention, below any measured geometry
    "inferred": 7,
}

#: Human-readable origin, used in reports.
SOURCE_LABEL: dict[str, str] = {
    "extra_toml": "extra.toml (user rules)",
    "parts_file": "parts file",
    "board": "KiCad board",
    "netlist_heuristic": "netlist heuristic",
    "datasheet_pdf": "datasheet PDF",
    "step": "STEP model",
    "model_name": "name convention (low confidence)",
    "inferred": "name inference",
}


@dataclass
class Provenance:
    """Where a fact came from."""

    source: str
    source_ref: str = ""
    confidence: float = 1.0
    fetched_at: str = ""

    @property
    def rank(self) -> int:
        return SOURCE_RANK.get(self.source, 99)

    def to_json(self) -> dict:
        payload = {"source": self.source, "confidence": round(self.confidence, 3)}
        if self.source_ref:
            payload["source_ref"] = self.source_ref
        if self.fetched_at:
            payload["fetched_at"] = self.fetched_at
        return payload

    @classmethod
    def from_json(cls, payload: dict) -> Provenance:
        return cls(
            source=payload["source"],
            source_ref=payload.get("source_ref", ""),
            confidence=float(payload.get("confidence", 1.0)),
            fetched_at=payload.get("fetched_at", ""),
        )


@dataclass
class Fact:
    """A value with its provenance."""

    value: Any
    provenance: Provenance

    def to_json(self) -> dict:
        return {"value": self.value, "provenance": self.provenance.to_json()}

    @classmethod
    def from_json(cls, payload: dict) -> Fact:
        return cls(payload["value"], Provenance.from_json(payload["provenance"]))


def pick(facts: list[Fact]) -> Fact | None:
    """Strongest fact of a list, or None when empty."""
    if not facts:
        return None
    return min(facts, key=lambda fact: fact.provenance.rank)


@dataclass
class PartFacts:
    """What is known about one part identity.

    ``identity`` is a namespaced key: ``lcsc:C25804``, ``mpn:RC0603FR-0710KL``
    or ``fp:Resistor_SMD:R_0603_1608Metric``.
    """

    identity: str
    facts: dict[str, Fact] = field(default_factory=dict)
    pin_roles: dict[str, Fact] = field(default_factory=dict)  # pad number -> role name
    pin_currents: dict[str, Fact] = field(default_factory=dict)  # pad number -> amps

    def set(self, name: str, value: Any, provenance: Provenance) -> None:
        self.facts[name] = Fact(value, provenance)

    def get(self, name: str) -> Fact | None:
        return self.facts.get(name)

    def to_json(self) -> dict:
        return {
            "identity": self.identity,
            "facts": {name: fact.to_json() for name, fact in sorted(self.facts.items())},
            "pin_roles": {pin: fact.to_json() for pin, fact in sorted(self.pin_roles.items())},
            "pin_currents": {
                pin: fact.to_json() for pin, fact in sorted(self.pin_currents.items())
            },
        }

    @classmethod
    def from_json(cls, payload: dict) -> PartFacts:
        record = cls(identity=payload["identity"])
        for name, fact in payload.get("facts", {}).items():
            record.facts[name] = Fact.from_json(fact)
        for pin, fact in payload.get("pin_roles", {}).items():
            record.pin_roles[pin] = Fact.from_json(fact)
        for pin, fact in payload.get("pin_currents", {}).items():
            record.pin_currents[pin] = Fact.from_json(fact)
        return record


#: Part-level fact names the resolver understands.
SCALAR_FACTS = (
    "height_mm",
    "mass_g",
    "power_w",
    "r_theta_ja",
    "exclusion_radius_mm",
    "mpn",
    "manufacturer",
    "package",
    "datasheet_url",
)

#: Pin role names accepted by ``pin_roles`` in any source.
PIN_ROLE_NAMES = {
    "power": "PIN_POWER",
    "ground": "PIN_GROUND",
    "diff_p": "PIN_DIFF_P",
    "diff_n": "PIN_DIFF_N",
    "clock": "PIN_CLOCK",
    "signal": "0",
}
