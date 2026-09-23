"""Dimensions encoded in a file or footprint name.

Custom libraries often carry the package height in the name:
``LED-SMD_L3.7-W3.5-H2.8.wrl``, ``QSOP-24_L8.7-W3.9-H1.6-LS6.0-P0.64``. When no
3D geometry is available, that number is the only thing left - and it is a
*convention*, not a manufacturer datum, so this provider:

* ranks below STEP in the precedence chain (geometry always wins);
* reports a low confidence and names the file it read;
* is never used to override a value from any other source.

The point is to keep the degraded mode honest: ``-H2.8`` is better than "height
unknown" for a z-interference check, as long as the report says where it came
from.
"""

from __future__ import annotations

import re
from pathlib import Path

from ..model import Fact, Provenance

SOURCE = "model_name"

#: ``-H2.8`` / ``_H1.6`` / ``H0.45`` at a word boundary. Deliberately does not
#: match ``-HR`` or a bare ``H``: the digit is what makes it a dimension.
_HEIGHT = re.compile(r"[-_ ]H(\d+(?:[.,]\d+)?)", re.IGNORECASE)

#: A naming convention is a hint, not data.
CONFIDENCE = 0.5

#: Above this, a "height" parsed from a name is certainly a mis-parse.
MAX_PLAUSIBLE_MM = 100.0


def height_from_name(text: str) -> float | None:
    """The ``-H<value>`` dimension of a name, in mm, or None."""
    if not text:
        return None
    match = _HEIGHT.search(text)
    if not match:
        return None
    try:
        value = float(match.group(1).replace(",", "."))
    except ValueError:
        return None
    if not 0.0 < value <= MAX_PLAUSIBLE_MM:
        return None
    return value


def candidate_names(component: dict) -> list[str]:
    """Names to read, most specific first: model files, then the footprint."""
    names = []
    for model in component.get("models", []) or []:
        names.append(Path(str(model)).name)
    fpid = str(component.get("fpid", ""))
    if fpid:
        names.append(fpid.split(":")[-1])
    return names


def height_fact(component: dict) -> Fact | None:
    """A low-confidence height fact from the component's names, or None."""
    for name in candidate_names(component):
        value = height_from_name(name)
        if value is not None:
            return Fact(value, Provenance(SOURCE, name, CONFIDENCE))
    return None
