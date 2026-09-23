"""Net-name electrical inference and differential pair matching."""

from __future__ import annotations

import pytest

from dodplace.ir.spec import PIN_CLOCK, PIN_DIFF_N, PIN_DIFF_P, PIN_GROUND, PIN_INFERRED, PIN_POWER
from dodplace.kicad.naming import find_diff_pairs, infer_pin_flags

GROUND_CASES = ("GND", "gnd", "AGND", "VSS", "DGND", "GNDA")
POWER_CASES = ("VCC", "VDD", "VDDIO", "VBUS", "VBAT", "+3V3", "+5V", "3V3", "1V8", "5V", "VCORE")
CLOCK_CASES = ("CLK", "CLK_25M", "XTAL1", "OSC_IN")


@pytest.mark.parametrize("name", GROUND_CASES)
def test_ground_detection(name):
    assert infer_pin_flags(name) == (PIN_GROUND | PIN_INFERRED)


@pytest.mark.parametrize("name", POWER_CASES)
def test_power_detection(name):
    assert infer_pin_flags(name) == (PIN_POWER | PIN_INFERRED)


@pytest.mark.parametrize("name", CLOCK_CASES)
def test_clock_detection(name):
    assert infer_pin_flags(name) == (PIN_CLOCK | PIN_INFERRED)


@pytest.mark.parametrize("name", ("SIG1", "MOSI", "MISO", "RESET", "D+", "D-"))
def test_plain_signals_are_left_alone(name):
    # D+/D- end with the diff markers, so they are the one exception.
    if name in ("D+", "D-"):
        assert infer_pin_flags(name) in (
            PIN_DIFF_P | PIN_INFERRED,
            PIN_DIFF_N | PIN_INFERRED,
        )
    else:
        assert infer_pin_flags(name) == 0


def test_unconnected_pin_has_no_role():
    assert infer_pin_flags(None) == 0
    assert infer_pin_flags("") == 0


def test_ground_wins_over_power_prefix():
    """VSS must not be mistaken for a rail."""
    assert infer_pin_flags("VSS") & PIN_GROUND
    assert not infer_pin_flags("VSS") & PIN_POWER


def test_diff_pair_matching():
    names = ["USB1_P", "USB1_N", "USB2_P", "GND", "VCC", "CLK-", "CLK+"]
    pairs = find_diff_pairs(names)
    assert ("USB1_P", "USB1_N") in pairs
    assert ("CLK+", "CLK-") in pairs
    assert len(pairs) == 2


def test_unmatched_polarity_is_not_a_pair():
    assert find_diff_pairs(["USB1_P", "SOMETHING_ELSE"]) == []
