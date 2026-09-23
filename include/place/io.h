/*
 * io.h - the C <-> Python interchange format (see ir/spec.md).
 *
 * scene.bin  (magic "DODP") : a versioned, section-table container whose
 *                            payloads are exactly the engine's SoA arrays.
 * placement.bin (magic "DODR"): the solver's output, one fixed-size record
 *                            per component.
 *
 * Design notes
 * ------------
 *  - Sections carry only RAW arrays. Everything the engine can derive
 *    (first_pin ranges, the net CSR, the per-component constraint indices,
 *    courtyard and outline fallbacks) is recomputed by finalize(), so the
 *    file can never disagree with the in-memory model.
 *  - Every section payload starts on a 64-byte boundary, matching the arena's
 *    cache-line alignment, so loading is a plain fread into the arena array.
 *  - The format is little-endian by definition; a file is refused on a
 *    big-endian host and a byte-order marker is validated.
 *  - Unknown section kinds are skipped (forward compatibility); a section
 *    required by the declared counts must be present with exactly the
 *    expected element count and size.
 */
#ifndef PLACE_IO_H
#define PLACE_IO_H

#include "place/context.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PLACE_SCENE_MAGIC              "DODP"
#define PLACE_RESULT_MAGIC             "DODR"
#define PLACE_SCENE_VERSION_MAJOR      1u
#define PLACE_SCENE_VERSION_MINOR      0u
#define PLACE_SCENE_HEADER_SIZE        128u
#define PLACE_SCENE_SECTION_ENTRY_SIZE 32u
#define PLACE_SCENE_MAX_SECTIONS       256u
#define PLACE_SCENE_ALIGNMENT          PLACE_CACHELINE
#define PLACE_RESULT_HEADER_SIZE       32u

/* scene_header_t.counts[] slots, in file order. */
enum : uint32_t {
    SCENE_FIELD_COMPS = 0,
    SCENE_FIELD_PINS,
    SCENE_FIELD_NETS,
    SCENE_FIELD_NET_ENTRIES,
    SCENE_FIELD_POLYGONS,
    SCENE_FIELD_VERTICES,
    SCENE_FIELD_DECOUPLING,
    SCENE_FIELD_THERMAL,
    SCENE_FIELD_SYMMETRY,
    SCENE_FIELD_DIFFPAIRS,
    SCENE_FIELD_COUNT
};

typedef enum : uint32_t {
    SCENE_SECTION_NONE = 0,

    SCENE_SECTION_COMP_X = 1,
    SCENE_SECTION_COMP_Y,
    SCENE_SECTION_COMP_HALF_W,
    SCENE_SECTION_COMP_HALF_H,
    SCENE_SECTION_COMP_HEIGHT,
    SCENE_SECTION_COMP_MASS,
    SCENE_SECTION_COMP_FLAGS,      /* u8  */
    SCENE_SECTION_COMP_PIN_COUNT,  /* u16 */
    SCENE_SECTION_COMP_POLYGON_ID, /* u32 */

    SCENE_SECTION_PIN_OFFSET_X,
    SCENE_SECTION_PIN_OFFSET_Y,
    SCENE_SECTION_PIN_COMP_ID,
    SCENE_SECTION_PIN_NET_ID,
    SCENE_SECTION_PIN_FLAGS,       /* u8  */
    SCENE_SECTION_PIN_MAX_CURRENT,

    SCENE_SECTION_NET_WEIGHT,
    SCENE_SECTION_NET_CLASS,       /* u8  */

    SCENE_SECTION_POLY_VX,
    SCENE_SECTION_POLY_VY,
    SCENE_SECTION_POLY_OFFSETS,    /* num_polys + 1 entries */
    SCENE_SECTION_POLY_KIND,       /* u8  */
    SCENE_SECTION_POLY_LAYER_MASK,

    SCENE_SECTION_DECOUPLING_IC,
    SCENE_SECTION_DECOUPLING_CAP,
    SCENE_SECTION_DECOUPLING_PIN,
    SCENE_SECTION_DECOUPLING_MAX_DIST_SQ,
    SCENE_SECTION_DECOUPLING_WEIGHT,

    SCENE_SECTION_THERMAL_COMP,
    SCENE_SECTION_THERMAL_POWER,
    SCENE_SECTION_THERMAL_RADIUS,
    SCENE_SECTION_THERMAL_R_THETA,

    SCENE_SECTION_SYMMETRY_A,
    SCENE_SECTION_SYMMETRY_B,
    SCENE_SECTION_SYMMETRY_AXIS,   /* u8 */
    SCENE_SECTION_SYMMETRY_WEIGHT,

    SCENE_SECTION_DIFFPAIR_P,
    SCENE_SECTION_DIFFPAIR_N,
    SCENE_SECTION_DIFFPAIR_SKEW,

    SCENE_SECTION_CLEARANCE_MATRIX, /* f32, num_net_classes^2 */
    SCENE_SECTION_BOARD_CONFIG,     /* one scene_board_config_t */

    /* Added after the first release; section kinds are stable identifiers and
     * are never renumbered, so this one continues the series rather than
     * shifting every kind below it. */
    SCENE_SECTION_COMP_KIND = 41,   /* u8 */

    /* The copper of each pad, as half extents in the footprint's frame. Zero
     * means the producer supplied nothing and the engine falls back to the pad
     * centre, so a scene written before these sections reads as it always did. */
    SCENE_SECTION_PIN_HALF_X = 42,  /* f32, num_pins */
    SCENE_SECTION_PIN_HALF_Y = 43,  /* f32, num_pins */

    /* The part's identity, so the solver may exchange it with another instance
     * of the same part. Explicit values, all four: implicit numbering shifted
     * the two kinds above the moment this one was inserted, and the Python
     * side has always numbered them 42/43/44. */
    SCENE_SECTION_PART_ID = 44,     /* u32, 0 = unknown */

    /* Human-readable names/metadata for reports and diffs. Ignored by C. */
    SCENE_SECTION_JSON_TAIL = 900,

    SCENE_SECTION_KIND_MAX = 1000
} scene_section_kind_t;

/*
 * Board-level scalars. Every field is 4 bytes wide so the layout has no
 * implicit padding on either side of the C/Python boundary (96 bytes).
 *
 * `degraded_mask` carries the *producer's* resolution report: the DEGRADED_*
 * bits for data no source could provide (missing height, inferred pin roles,
 * unresolved mass...). The bulk-load path bypasses placer_add_*, so without
 * this field that provenance would be lost. Bits that finalize() derives from
 * the loaded data (outline, keepouts, stackup, clearances...) are re-derived
 * on load and OR-ed in; mark_degraded() is idempotent.
 */
typedef struct {
    coord_t  grid_origin_x;
    coord_t  grid_origin_y;
    coord_t  grid_fine;
    coord_t  grid_coarse;
    coord_t  courtyard_fallback_margin;
    coord_t  global_clearance;
    coord_t  courtyard_clearance;
    coord_t  min_track_width;
    coord_t  total_thickness;
    coord_t  ceiling_height;
    coord_t  airflow_x;
    coord_t  airflow_y;
    uint32_t allowed_sides;         /* SIDE_TOP | SIDE_BOTTOM */
    uint32_t copper_layers;
    uint32_t has_stackup;
    uint32_t allow_vias_under_body;
    uint32_t disable_mask;          /* OPT_DISABLE_* */
    uint32_t degraded_mask;         /* DEGRADED_* subset */
    uint32_t reserved[6];
} scene_board_config_t;

static_assert(sizeof(scene_board_config_t) == 96, "board config layout is part of the IR");

/* ------------------------------------------------------------------------- */
/* Placement result                                                          */
/* ------------------------------------------------------------------------- */

enum : uint8_t {
    PLACEMENT_FLAG_LOCKED   = 0x01u, /* position frozen by the user */
    PLACEMENT_FLAG_MOVED    = 0x02u, /* differs from the ingested position */
    PLACEMENT_FLAG_UNPLACED = 0x04u  /* no legal position was found */
};

typedef struct {
    coord_t x;
    coord_t y;
    uint8_t orient; /* 0..3 -> 0/90/180/270 degrees */
    uint8_t side;   /* 0 = top, 1 = bottom */
    uint8_t flags;  /* PLACEMENT_FLAG_* */
    uint8_t reserved;
} placement_entry_t;

static_assert(sizeof(placement_entry_t) == 12, "placement record layout is part of the IR");

/* ------------------------------------------------------------------------- */
/* IO report                                                                 */
/* ------------------------------------------------------------------------- */

typedef struct {
    uint32_t errors;
    uint32_t warnings;
    uint32_t sections_loaded;
    uint32_t sections_skipped;
    uint32_t counts[SCENE_FIELD_COUNT];
    char     message[128];
} scene_io_report_t;

void scene_io_report_clear(scene_io_report_t *report);

/* ------------------------------------------------------------------------- */
/* API                                                                       */
/* ------------------------------------------------------------------------- */

/*
 * Loads a scene.bin into `ctx` (which must be zeroed and not yet begun):
 * validates the header and the section table, sizes the arenas from the
 * declared counts, streams the sections straight into the arena arrays, then
 * calls adopt + finalize so the result is exactly what an incremental
 * ingestion would have produced. On failure the caller must destroy `ctx`.
 */
[[nodiscard]] bool scene_load(placer_context_t *ctx, const char *path,
                              scene_io_report_t *report);

/*
 * Writes a finalised context to a scene.bin. Used for round-trip testing, for
 * regenerating the golden fixture in ir/fixtures, and to dump a scene for
 * debugging. Writes a static JSON tail so the format's ignored-section path is
 * exercised on every round trip.
 */
[[nodiscard]] bool scene_save(const placer_context_t *ctx, const char *path);

[[nodiscard]] bool placement_save(const char *path, const placement_entry_t *entries,
                                  uint32_t count);

/*
 * Reads a placement.bin into a caller-owned buffer and reports the record
 * count the file declares (even when it does not fit, so the caller can grow).
 */
[[nodiscard]] bool placement_load(const char *path, placement_entry_t *entries,
                                  uint32_t capacity, uint32_t *out_count);

#endif /* PLACE_IO_H */
