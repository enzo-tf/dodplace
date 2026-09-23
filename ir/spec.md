# dodplace IR — normative format specification

Version 1.0 — little-endian only — every payload starts on a 64-byte boundary.

Two files make up the interchange between the Python front end and the C engine:

| File | Magic | Direction | Content |
|---|---|---|---|
| `scene.bin` | `DODP` | Python → C | the whole scene, as flat SoA arrays |
| `placement.bin` | `DODR` | C → Python | one record per component |

For a map of every artifact the project writes (and what is deliberately
left out of these files), see [`../docs/data-map.md`](../docs/data-map.md).

The C reference implementation is `src/io.c`; the golden fixture
`ir/fixtures/scene_minimal.bin` is the byte-level contract. Regenerate it with:

```sh
cmake --build --preset gcc-debug
./build/gcc-debug/tests/test_scene_io --emit-fixture ir/fixtures/scene_minimal.bin
ctest --preset gcc-debug
```

## 1. General rules

1. All integers and floats are **little-endian**; `float` is IEEE-754 binary32.
2. Section payloads start at a file offset that is a multiple of **64 bytes**.
   Gaps between sections are zero-filled.
3. Unknown section kinds are **skipped** (forward compatibility). A section the
   declared counts require must be present, with the exact `count` and
   `elem_size` the reader expects.
4. A duplicate section kind is an error.
5. Counts are declared in the header *before* any payload; a reader may size its
   buffers from them and must never trust a payload to be self-describing.

## 2. `scene.bin` header — 128 bytes

| Offset | Size | Type | Field |
|---:|---:|---|---|
| 0 | 4 | char[4] | magic `"DODP"` |
| 4 | 2 | u16 | `version_major` (currently **1**) |
| 6 | 2 | u16 | `version_minor` (currently 0) |
| 8 | 4 | u32 | `flags`, bit 0 = little-endian marker (must be set) |
| 12 | 4 | u32 | `header_size` (must be 128) |
| 16 | 4 | u32 | `section_count` (1..256) |
| 20 | 4 | u32 | reserved (0) |
| 24 | 40 | u32[10] | `counts[]`, see §3 |
| 64 | 4 | u32 | `num_net_classes` (0..255) |
| 68 | 52 | u32[13] | reserved (0) |
| 120 | 8 | u64 | reserved (0) |

## 3. `counts[]` order

| Index | Field | Meaning |
|---:|---|---|
| 0 | `num_comps` | components |
| 1 | `num_pins` | pins |
| 2 | `num_nets` | nets |
| 3 | `num_net_entries` | connected pins (≤ `num_pins`) |
| 4 | `num_polygons` | courtyard + outline + keepout polygons |
| 5 | `num_vertices` | summed over all polygons |
| 6 | `num_decoupling` | decoupling constraint rows |
| 7 | `num_thermal` | thermal constraint rows |
| 8 | `num_symmetry` | symmetry constraint rows |
| 9 | `num_diffpairs` | differential pair rows |

## 4. Section table — 32 bytes per entry, starting at offset 128

| Offset | Size | Type | Field |
|---:|---:|---|---|
| 0 | 4 | u32 | `kind` (§5) |
| 4 | 4 | u32 | `offset` from the start of the file, multiple of 64 |
| 8 | 4 | u32 | `count` in elements |
| 12 | 4 | u32 | `elem_size` in bytes |
| 16 | 8 | u64 | reserved (0) |
| 24 | 8 | u64 | reserved (0) |

The first payload starts at `align64(128 + 32 * section_count)`.

## 5. Section kinds

`elem_size` is fixed by the kind; it is listed for completeness.

| Kind | Name | Type | Count |
|---:|---|---|---|
| 1 | `COMP_X` | f32 | `num_comps` |
| 2 | `COMP_Y` | f32 | `num_comps` |
| 3 | `COMP_HALF_W` | f32 | `num_comps` |
| 4 | `COMP_HALF_H` | f32 | `num_comps` |
| 5 | `COMP_HEIGHT` | f32 | `num_comps` |
| 6 | `COMP_MASS` | f32 | `num_comps` |
| 7 | `COMP_FLAGS` | u8 | `num_comps` |
| 8 | `COMP_PIN_COUNT` | u16 | `num_comps` |
| 9 | `COMP_POLYGON_ID` | u32 | `num_comps` (`0xFFFFFFFF` = none) |
| 10 | `PIN_OFFSET_X` | f32 | `num_pins` |
| 11 | `PIN_OFFSET_Y` | f32 | `num_pins` |
| 12 | `PIN_COMP_ID` | u32 | `num_pins` |
| 13 | `PIN_NET_ID` | u32 | `num_pins` (`0xFFFFFFFF` = unconnected) |
| 14 | `PIN_FLAGS` | u8 | `num_pins` |
| 15 | `PIN_MAX_CURRENT` | f32 | `num_pins` |
| 16 | `NET_WEIGHT` | f32 | `num_nets` |
| 17 | `NET_CLASS` | u8 | `num_nets` |
| 18 | `POLY_VX` | f32 | `num_vertices` |
| 19 | `POLY_VY` | f32 | `num_vertices` |
| 20 | `POLY_OFFSETS` | u32 | `num_polygons + 1` |
| 21 | `POLY_KIND` | u8 | `num_polygons` |
| 22 | `POLY_LAYER_MASK` | u32 | `num_polygons` |
| 23 | `DECOUPLING_IC` | u32 | `num_decoupling` |
| 24 | `DECOUPLING_CAP` | u32 | `num_decoupling` |
| 25 | `DECOUPLING_PIN` | u32 | `num_decoupling` |
| 26 | `DECOUPLING_MAX_DIST_SQ` | f32 | `num_decoupling` |
| 27 | `DECOUPLING_WEIGHT` | f32 | `num_decoupling` |
| 28 | `THERMAL_COMP` | u32 | `num_thermal` |
| 29 | `THERMAL_POWER` | f32 | `num_thermal` |
| 30 | `THERMAL_RADIUS` | f32 | `num_thermal` |
| 31 | `THERMAL_R_THETA` | f32 | `num_thermal` |
| 32 | `SYMMETRY_A` | u32 | `num_symmetry` |
| 33 | `SYMMETRY_B` | u32 | `num_symmetry` |
| 34 | `SYMMETRY_AXIS` | u8 | `num_symmetry` (0=x, 1=y, 2=fixed) |
| 35 | `SYMMETRY_WEIGHT` | f32 | `num_symmetry` |
| 36 | `DIFFPAIR_P` | u32 | `num_diffpairs` |
| 37 | `DIFFPAIR_N` | u32 | `num_diffpairs` |
| 38 | `DIFFPAIR_SKEW` | f32 | `num_diffpairs` |
| 39 | `CLEARANCE_MATRIX` | f32 | `num_net_classes²` (row-major) |
| 40 | `BOARD_CONFIG` | struct | 1 (96 bytes, §7) |
| 41 | `COMP_KIND` | u8 | `num_comps` |
| 900 | `JSON_TAIL` | u8 | byte length; free-form JSON, **ignored by the C reader** |

Sections with a count of zero are omitted by the writer and not required by the
reader.

### Pin flags (`PIN_FLAGS`, kind 14)

| Bit | Value | Meaning |
|---:|---:|---|
| 0 | `0x01` | power rail |
| 1 | `0x02` | ground |
| 2, 3 | `0x04`, `0x08` | differential pair P / N |
| 4 | `0x10` | clock |
| 5 | `0x20` | plated through hole: copper on both faces |
| 6 | `0x40` | non-plated hole: no copper, but the drill and its mask opening cross every layer |
| 7 | `0x80` | the electrical role came from name inference, not from authoritative data |

Bit 6 is additive and was added after the first release. A consumer that ignores
it treats a mounting hole as a one-sided part and can put copper on top of it
from the far side, which KiCad reports as `solder_mask_bridge`,
`hole_clearance` and `copper_edge_clearance` in the same breath.

**Kind numbers are stable identifiers and are never renumbered.** `COMP_KIND` is
41 although it sits with the other component sections in this table, because it
was added after the first release: a new field continues the series rather than
shifting every kind below it, so files written by an older producer keep
loading. The emission order follows the table above; the numbering does not.

`COMP_KIND` is the only semantic datum in the scene: the engine has no names, and
the clustering stage needs to tell an inductor from a decoupling capacitor. The
producer derives it from the reference designator prefix (`U` IC, `C` capacitor,
`L` inductor, `Y` crystal...), the one role convention available without a
datasheet.

## 6. What is NOT stored — and why

These are recomputed by `placer_context_finalize()` from the raw arrays, so the
file cannot disagree with the in-memory model:

| Derived data | Recomputed from |
|---|---|
| `comps.first_pin[]` | prefix sum of `COMP_PIN_COUNT` |
| `nets.net_offsets[]`, `nets.net_to_pins[]` | counting sort over `PIN_NET_ID` |
| `decoupling/thermal/symmetry.first_by_comp[]` | counting pass over the primary key column |
| courtyard AABB (when absent) | pad bbox + margin, with origin rebase |
| board outline (when absent) | bounding box of all courtyards |
| board-level degraded bits | presence tests on stackup / matrix / keepouts / … |

A consumer may therefore omit `COMP_HALF_W`/`COMP_HALF_H` (leave 0) and let the
engine derive them — that is what raises `DEGRADED_COURTYARD_BBOX`.

Two conventions follow from this and are worth stating explicitly:

* **Polygon rings are closed implicitly**: the last vertex joins the first. A
  repeated closing vertex is tolerated but not required.
* **A producer with richer geometry should derive the courtyard itself.** The
  KiCad front end has pad *extents*, which the engine's last-resort rule (pad
  *centres*) cannot know, so it emits the derived rectangle as a real
  `POLY_KIND_COURTYARD` polygon and sets `DEGRADED_COURTYARD_BBOX` in
  `degraded_mask`. The engine then treats it with its normal polygon path
  (AABB derivation plus origin rebase), so no fallback is implemented twice:
  the producer replaces it, it does not duplicate it. The engine's own rule
  remains for hand-built scenes that carry only pin offsets.

## 7. `BOARD_CONFIG` payload — 96 bytes

| Offset | Type | Field |
|---:|---|---|
| 0 | f32 | `grid_origin_x` |
| 4 | f32 | `grid_origin_y` |
| 8 | f32 | `grid_fine` (> 0, default 0.1) |
| 12 | f32 | `grid_coarse` (> 0, default 0.5) |
| 16 | f32 | `courtyard_fallback_margin` (> 0, default 0.25) |
| 20 | f32 | `global_clearance` (> 0, default 0.2) |
| 24 | f32 | `courtyard_clearance` (≥ 0, default 0.2) |
| 28 | f32 | `min_track_width` (> 0) |
| 32 | f32 | `total_thickness` (0 = unknown) |
| 36 | f32 | `ceiling_height` (0 = no z test) |
| 40 | f32 | `airflow_x` (0,0 = isotropic) |
| 44 | f32 | `airflow_y` |
| 48 | u32 | `allowed_sides` (`1` top, `2` bottom, `3` both) |
| 52 | u32 | `copper_layers` (0 = unknown) |
| 56 | u32 | `has_stackup` (0/1) |
| 60 | u32 | `allow_vias_under_body` (0/1) |
| 64 | u32 | `disable_mask` (`OPT_DISABLE_*`) |
| 68 | u32 | `degraded_mask` (`DEGRADED_*` subset, see §8) |
| 72 | u32[6] | reserved (0) |

`allowed_sides` is a 4-byte field to keep the struct free of implicit padding.
The C header `place/io.h` asserts the layout with `static_assert`.

## 8. `degraded_mask` — carrying the producer's resolution report

The bulk-load path bypasses `placer_add_*()`, so the degradation bits the
incremental path would have raised from the *data itself* cannot be re-derived
after the fact: once `COMP_HEIGHT` is 0, a reader cannot tell "height unknown"
from "height genuinely zero". The producer therefore reports which optional
data it could not resolve:

- The writer sets `degraded_mask` from its resolver report (Python) or from the
  context it just finalized (C).
- The reader restores the mask, then `finalize()` **ORs in** the bits it derives
  from the loaded data (outline, keepouts, stackup, clearance matrix, ceiling,
  airflow, via restriction, length bounds, segregation). `mark_degraded()` is
  idempotent, and `warnings` is rebuilt as `popcount(degraded_mask)` so the
  invariant `warnings == popcount(degraded)` holds across the round trip.
- Bits outside `DEGRADED_ALL_MASK` are rejected.

### Who raises which bit

| Raised by the **producer** (data-derived, unknowable after the fact) | Raised by the **engine** (derived from the loaded data) |
|---|---|
| `NO_3D_HEIGHT`, `NO_MASS` | `BOARD_OUTLINE_BBOX` (no outline polygon given) |
| `NO_ROTATION_DATA` | `NO_KEEPOUTS` (no keepout polygon) |
| `NO_PIN_ELEC`, `NO_PIN_CURRENT` | `NO_STACKUP` (no layer count) |
| `NO_NET_WEIGHTS`, `NO_NET_CLASSES` | `NO_CLEARANCE_MATRIX` (no matrix) |
| `NO_DIFFPAIRS` | `NO_CEILING`, `NO_AIRFLOW`, `NO_VIA_RESTRICTION` |
| `COURTYARD_BBOX` | `NO_DECOUPLING`, `NO_THERMAL`, `NO_SYMMETRY` |
| | `NO_MAX_LENGTH`, `NO_SEGREGATION` |

A producer never needs to guess the right column: it knows which data it
supplied. The engine re-derives the right column from what it received, and
`mark_degraded()` is idempotent, so an overlap is harmless.

## 9. `placement.bin`

Header — 32 bytes:

| Offset | Size | Type | Field |
|---:|---:|---|---|
| 0 | 4 | char[4] | magic `"DODR"` |
| 4 | 2 | u16 | `version_major` (1) |
| 6 | 2 | u16 | `version_minor` (0) |
| 8 | 4 | u32 | `flags` (bit 0 = little-endian) |
| 12 | 4 | u32 | `count` (must equal `num_comps`) |
| 16 | 4 | u32 | `entry_size` (12) |
| 20 | 12 | u32[3] | reserved (0) |

Entry — 12 bytes, one per component, in component-id order:

| Offset | Type | Field |
|---:|---|---|
| 0 | f32 | `x` (courtyard centre, mm) |
| 4 | f32 | `y` |
| 8 | u8 | `orient` 0..3 → 0/90/180/270 degrees CCW |
| 9 | u8 | `side` 0 = top, 1 = bottom |
| 10 | u8 | `flags`: bit 0 locked, bit 1 moved, bit 2 unplaced |
| 11 | u8 | reserved (0) |

## 10. Reader validation rules (all fatal)

1. magic, `version_major`, `header_size`, little-endian flag, section count.
2. every section: 64-byte aligned offset, non-zero `elem_size`, payload fully
   inside the file (overflow-checked), no duplicate kind.
3. `BOARD_CONFIG` present, exactly one element, 96 bytes, values in range,
   `degraded_mask` ⊆ `DEGRADED_ALL_MASK`.
4. every section required by `counts[]` present with the exact `count` and
   `elem_size`.
5. after loading: `adopt()` + `finalize()` + `validate()` must all succeed
   (pin grouping, CSR bounds, id ranges, courtyard positivity, …).

On any failure the loader returns `false`, reports why, and leaves **no
half-built context behind** (the caller may destroy it safely).
