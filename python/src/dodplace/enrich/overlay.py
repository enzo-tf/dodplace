"""The resolved enrichment handed to the IR builder.

The overlay is the *only* thing ``to_scene`` sees: providers do not touch the
scene themselves, so the conversion stays a pure function of
(extract, overlay) and every override is visible in one dataclass.
"""

from __future__ import annotations

from dataclasses import dataclass, field


@dataclass
class ComponentEnrichment:
    """Resolved optional data for one component, all fields optional."""

    ref: str
    height_mm: float | None = None
    mass_g: float | None = None
    orientation_deg: float | None = None
    rot_fixed: bool | None = None
    locked: bool | None = None
    side: str | None = None
    pin_roles: dict[str, int] = field(default_factory=dict)  # pad number -> PIN_* flags
    pin_currents: dict[str, float] = field(default_factory=dict)  # pad number -> A
    power_w: float | None = None
    r_theta_ja: float | None = None
    exclusion_radius_mm: float | None = None
    part_number: str | None = None  # reporting only

    def has_geometry(self) -> bool:
        return self.height_mm is not None and self.mass_g is not None


@dataclass
class ThermalRow:
    component: str
    power_w: float
    radius_mm: float
    r_theta_ja: float = 0.0


@dataclass
class DecouplingRow:
    ic: str
    cap: str
    pin: str | None = None
    max_distance_mm: float = 5.0
    weight: float = 1.0


@dataclass
class SymmetryRow:
    a: str
    b: str
    axis: str = "x"
    weight: float = 1.0


@dataclass
class DiffPairRow:
    p: str
    n: str
    max_skew_mm: float = 0.0


@dataclass
class KeepoutRow:
    polygon: list[list[float]]
    layer_mask: int = 0


@dataclass
class BoardEnrichment:
    """Resolved optional board data."""

    allowed_sides: int | None = None
    ceiling_height_mm: float | None = None
    airflow: tuple[float, float] | None = None
    via_under_body_allowed: bool | None = None
    courtyard_margin_mm: float | None = None
    net_clearance: dict[str, float] = field(default_factory=dict)  # glob -> mm
    net_class_of: dict[str, str] = field(default_factory=dict)  # glob -> class
    class_clearance: dict[str, float] = field(default_factory=dict)  # class -> mm
    keepouts: list[KeepoutRow] = field(default_factory=list)


@dataclass
class EnrichmentOverlay:
    components: dict[str, ComponentEnrichment] = field(default_factory=dict)
    board: BoardEnrichment = field(default_factory=BoardEnrichment)
    thermal: list[ThermalRow] = field(default_factory=list)
    decoupling: list[DecouplingRow] = field(default_factory=list)
    symmetry: list[SymmetryRow] = field(default_factory=list)
    diffpairs: list[DiffPairRow] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)

    def component(self, ref: str) -> ComponentEnrichment:
        """Existing entry for ``ref``, created on first use."""
        entry = self.components.get(ref)
        if entry is None:
            entry = ComponentEnrichment(ref=ref)
            self.components[ref] = entry
        return entry

    def is_empty(self) -> bool:
        return not (
            self.components
            or self.thermal
            or self.decoupling
            or self.symmetry
            or self.diffpairs
            or self.board.keepouts
            or self.board.net_clearance
            or self.board.net_class_of
            or self.board.class_clearance
        )

    def merge_warnings(self, warnings: list[str]) -> None:
        self.warnings.extend(warnings)
