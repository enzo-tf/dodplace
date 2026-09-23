#!/usr/bin/env python3
"""Generate the plugin toolbar icons.

KiCad renders a plugin toolbar button only from the ``icons-light`` /
``icons-dark`` bitmaps declared in ``plugin.json``: with ``show-button: true``
and no icon, the button exists but is blank and therefore invisible.

Pure standard library (zlib + struct), no image dependency. The output is
committed under ``src/dodplace/plugin/icons/`` and shipped as package data.

    python3 tools/make_plugin_icons.py
"""

from __future__ import annotations

import struct
import zlib
from pathlib import Path

OUT_DIR = Path(__file__).resolve().parent.parent / "src" / "dodplace" / "plugin" / "icons"
SIZES = (24, 48)
SUPERSAMPLE = 4

#: Light mode draws a dark glyph, dark mode a light one.
PALETTE = {
    "light": (0x1A, 0x1A, 0x1A),
    "dark": (0xF0, 0xF0, 0xF0),
}


def write_png(path: Path, width: int, height: int, pixels: list[list[tuple[int, int, int, int]]]) -> None:
    raw = bytearray()
    for row in pixels:
        raw.append(0)  # filter type: none
        for red, green, blue, alpha in row:
            raw += bytes((red, green, blue, alpha))

    def chunk(tag: bytes, payload: bytes) -> bytes:
        return (
            struct.pack(">I", len(payload))
            + tag
            + payload
            + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)
        )

    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", header)
        + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
        + chunk(b"IEND", b"")
    )


def render(size: int, colour: tuple[int, int, int]) -> list[list[tuple[int, int, int, int]]]:
    """Analyze action: a board outline with four components being placed."""
    n = size * SUPERSAMPLE
    coverage = [[0.0] * n for _ in range(n)]

    def paint(x0: float, y0: float, x1: float, y1: float, radius: float, inside: bool) -> None:
        """Fill (or carve out) a rounded rectangle, in supersampled units."""
        for y in range(max(0, int(y0)), min(n, int(y1) + 1)):
            for x in range(max(0, int(x0)), min(n, int(x1) + 1)):
                # Distance to the rounded corner, zero inside the straight parts.
                dx = max(x0 + radius - x, 0.0, x - (x1 - radius))
                dy = max(y0 + radius - y, 0.0, y - (y1 - radius))
                if dx * dx + dy * dy <= radius * radius:
                    coverage[y][x] = 0.0 if inside else 1.0

    unit = n / 24.0  # the artwork is designed on a 24x24 grid

    # Board outline: outer rounded rect minus an inset one.
    paint(1.5 * unit, 1.5 * unit, 22.5 * unit, 22.5 * unit, 3.0 * unit, False)
    paint(3.2 * unit, 3.2 * unit, 20.8 * unit, 20.8 * unit, 1.6 * unit, True)

    # Three parts: a big one and two smaller ones, as in a placement pass.
    paint(5.5 * unit, 5.5 * unit, 13.5 * unit, 12.0 * unit, 0.8 * unit, False)
    paint(15.0 * unit, 5.5 * unit, 19.0 * unit, 9.5 * unit, 0.6 * unit, False)
    paint(5.5 * unit, 14.0 * unit, 9.5 * unit, 18.5 * unit, 0.6 * unit, False)
    paint(11.5 * unit, 14.0 * unit, 19.0 * unit, 18.5 * unit, 0.6 * unit, False)

    red, green, blue = colour
    pixels = []
    for y in range(size):
        row = []
        for x in range(size):
            total = 0.0
            for sy in range(SUPERSAMPLE):
                for sx in range(SUPERSAMPLE):
                    total += coverage[y * SUPERSAMPLE + sy][x * SUPERSAMPLE + sx]
            alpha = total / (SUPERSAMPLE * SUPERSAMPLE)
            row.append((red, green, blue, int(round(alpha * 255))))
        pixels.append(row)
    return pixels


def render_report(size: int, colour: tuple[int, int, int]) -> list[list[tuple[int, int, int, int]]]:
    """Report action: the same board, with the report lines drawn on it."""
    n = size * SUPERSAMPLE
    coverage = [[0.0] * n for _ in range(n)]

    def paint(x0: float, y0: float, x1: float, y1: float, radius: float, inside: bool) -> None:
        for y in range(max(0, int(y0)), min(n, int(y1) + 1)):
            for x in range(max(0, int(x0)), min(n, int(x1) + 1)):
                dx = max(x0 + radius - x, 0.0, x - (x1 - radius))
                dy = max(y0 + radius - y, 0.0, y - (y1 - radius))
                if dx * dx + dy * dy <= radius * radius:
                    coverage[y][x] = 0.0 if inside else 1.0

    unit = n / 24.0
    paint(1.5 * unit, 1.5 * unit, 22.5 * unit, 22.5 * unit, 3.0 * unit, False)
    paint(3.2 * unit, 3.2 * unit, 20.8 * unit, 20.8 * unit, 1.6 * unit, True)

    # Four text lines of decreasing length, like a report.
    for index, width in enumerate((11.0, 9.0, 12.0, 7.0)):
        top = (6.0 + index * 3.2) * unit
        paint(6.0 * unit, top, (6.0 + width) * unit, top + 1.4 * unit, 0.5 * unit, False)

    red, green, blue = colour
    pixels = []
    for y in range(size):
        row = []
        for x in range(size):
            total = 0.0
            for sy in range(SUPERSAMPLE):
                for sx in range(SUPERSAMPLE):
                    total += coverage[y * SUPERSAMPLE + sy][x * SUPERSAMPLE + sx]
            alpha = total / (SUPERSAMPLE * SUPERSAMPLE)
            row.append((red, green, blue, int(round(alpha * 255))))
        pixels.append(row)
    return pixels


def main() -> int:
    written = []
    for mode, colour in PALETTE.items():
        for size in SIZES:
            path = OUT_DIR / f"dodplace-{mode}-{size}.png"
            write_png(path, size, size, render(size, colour))
            written.append(path)
            report = OUT_DIR / f"dodplace-report-{mode}-{size}.png"
            write_png(report, size, size, render_report(size, colour))
            written.append(report)
    for path in written:
        print(f"wrote {path.relative_to(OUT_DIR.parent.parent.parent.parent)} "
              f"({path.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
