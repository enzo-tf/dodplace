"""Name-based electrical inference.

This is the *producer* side of the pin-role fallback. The C engine has its own
heuristic for hand-built scenes, keyed on pin names; a PCB has no pin names, so
here the signal is the **net name** instead. Both raise DEGRADED_NO_PIN_ELEC,
and neither is authoritative: an ``extra.toml`` or a datasheet pinout overrides
them in phase 3.
"""

from __future__ import annotations

from ..ir.spec import PIN_CLOCK, PIN_DIFF_N, PIN_DIFF_P, PIN_GROUND, PIN_INFERRED, PIN_POWER

_GROUND = ("gnd", "vss", "agnd", "dgnd", "earth")
_CLOCK = ("clk", "clock", "xtal", "osc")
_RAIL_PREFIXES = ("vcc", "vdd", "vbus", "vbat", "vin", "vcore", "vddio", "vccint", "vccio", "vref")


def is_ground(name: str) -> bool:
    lowered = name.lower()
    return any(mark in lowered for mark in _GROUND)


def is_power(name: str) -> bool:
    lowered = name.lower()
    if lowered.startswith(_RAIL_PREFIXES):
        return True
    if lowered.startswith("+") and len(lowered) > 1 and lowered[1].isdigit():
        return True  # +3V3, +5V
    if lowered[0:1].isdigit() and "v" in lowered:
        return True  # 3V3, 1V8, 5V
    return False


def is_clock(name: str) -> bool:
    lowered = name.lower()
    return any(mark in lowered for mark in _CLOCK)


def infer_pin_flags(net_name: str | None) -> int:
    """Return PIN_* flags for a net name, or 0 for a plain signal."""
    if not net_name:
        return 0
    flags = 0
    if is_ground(net_name):
        flags |= PIN_GROUND
    elif is_clock(net_name):
        flags |= PIN_CLOCK
    elif is_power(net_name):
        flags |= PIN_POWER
    elif diff_pair_polarity(net_name) is not None:
        flags |= PIN_DIFF_P if diff_pair_polarity(net_name) > 0 else PIN_DIFF_N
    return flags | (PIN_INFERRED if flags else 0)


#: Suffix pairs recognised as differential polarity markers.
_DIFF_SUFFIXES = (("_p", "_n"), ("-p", "-n"), ("+", "-"))


def diff_pair_polarity(net_name: str) -> int | None:
    """+1 for the positive member, -1 for the negative one, None otherwise."""
    lowered = net_name.lower()
    for positive, negative in _DIFF_SUFFIXES:
        if lowered.endswith(positive):
            return 1
        if lowered.endswith(negative):
            return -1
    return None


def find_diff_pairs(net_names: list[str]) -> list[tuple[str, str]]:
    """Match ``*_P`` / ``*_N`` style names into (positive, negative) pairs."""
    by_prefix: dict[str, dict[int, str]] = {}
    for name in net_names:
        polarity = diff_pair_polarity(name)
        if polarity is None:
            continue
        prefix = name[:-1].lower()
        by_prefix.setdefault(prefix, {})[polarity] = name
    pairs = []
    for prefix in sorted(by_prefix):
        members = by_prefix[prefix]
        if 1 in members and -1 in members:
            pairs.append((members[1], members[-1]))
    return pairs
