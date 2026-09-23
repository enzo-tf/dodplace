"""Enrichment: resolve optional part data from every available source.

Sources, strongest first: extra.toml user rules, a board parts file, the board
itself, netlist heuristics, datasheet PDFs and STEP models. Whatever stays
unresolved is deliberately left out so the C engine applies its own fallback.
"""

from .model import Fact, PartFacts, Provenance
from .overlay import ComponentEnrichment, EnrichmentOverlay
from .resolver import ResolveReport, decoupling_from_netlist, resolve
from . import template
from .store import EnrichmentStore, content_key

__all__ = [
    "ComponentEnrichment",
    "EnrichmentOverlay",
    "EnrichmentStore",
    "Fact",
    "PartFacts",
    "Provenance",
    "ResolveReport",
    "content_key",
    "decoupling_from_netlist",
    "resolve",
    "template",
]
