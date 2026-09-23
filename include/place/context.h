/*
 * context.h - ingestion contract and global scene container.
 *
 * Two data streams feed the engine:
 *
 *   1. MANDATORY (KiCad .kicad_pcb + netlist): geometry, pins and
 *      connectivity. A context carrying only these is immediately solvable.
 *   2. OPTIONAL (datasheet PDFs, JLCPCB/LCSC APIs, STEP models): heights,
 *      masses, electrical roles, thermal envelopes, differential pairs...
 *      Every optional datum that is absent switches its feature to a
 *      documented fallback and raises one DEGRADED_* bit, so the solver can
 *      adapt its strategy globally without per-object branching.
 *
 * Ingestion is a three-pass contract that keeps the whole scene in one
 * contiguous block:
 *
 *   pass 1  placer_context_begin()  declares exact counts -> sizes both arenas
 *   pass 2  placer_add_*()          fills pre-allocated arrays (bounds-checked)
 *   pass 3  placer_context_finalize() builds derived data, applies fallbacks,
 *                                     verifies consistency, empties scratch
 *
 * Invariant maintained by design: `warnings == popcount(degraded)` - each
 * distinct fallback category is counted exactly once.
 */
#ifndef PLACE_CONTEXT_H
#define PLACE_CONTEXT_H

#include "place/arena.h"
#include "place/constraints.h"
#include "place/types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------------- */
/* Board, rules, stackup                                                      */
/* ------------------------------------------------------------------------- */

typedef uint8_t side_mask_t;

enum : side_mask_t {
    SIDE_TOP    = 0x01u,
    SIDE_BOTTOM = 0x02u,
    SIDE_BOTH   = 0x03u
};

/* Placement grid: a fine grid for final legalisation, a coarse one for the
 * global phase. Both are mandatory solver configuration with fixed fallbacks
 * (0.1 mm / 0.5 mm), so they raise no degraded bit. */
typedef struct {
    coord_t origin_x;
    coord_t origin_y;
    coord_t step_fine;   /* default 0.1 mm */
    coord_t step_coarse; /* default 0.5 mm */
} grid_t;

/*
 * Block 7/8 - electrical rules. `class_clearance` is the optional N x N
 * net-class clearance matrix (row-major, mm); when absent the solver uses
 * `global_clearance` for every pair.
 *
 * Board-level enrichment (rules, stackup, grid) is injected by writing the
 * context fields directly between begin() and finalize(). Any pointer stored
 * there must be allocated from `ctx->scene` - the scratch arena is reset by
 * finalize() - or from caller-owned memory that outlives the context.
 */
typedef struct {
    coord_t  global_clearance;    /* default 0.2 mm */
    coord_t  courtyard_clearance; /* default 0.2 mm */
    coord_t  min_track_width;     /* default 0.2 mm */
    coord_t *class_clearance;     /* num_classes^2, or nullptr */
    uint8_t  num_classes;         /* 0 when the matrix is absent */
    bool     allow_vias_under_body; /* default true (no restriction) */
} design_rules_t;

/* Block 5 + 9 - physical stackup and mechanical envelope. */
typedef struct {
    side_mask_t allowed_sides;  /* default SIDE_BOTH */
    uint8_t     copper_layers;  /* 0 = unknown */
    bool        has_stackup;    /* copper/dielectric data present */
    coord_t     total_thickness;/* mm; 0 = unknown */
    coord_t     ceiling_height; /* mm above the board; 0 = no z test */
    coord_t     airflow_x;      /* airflow direction; (0,0) = isotropic */
    coord_t     airflow_y;
} stackup_t;

/* ------------------------------------------------------------------------- */
/* Degraded-mode reporting                                                    */
/* ------------------------------------------------------------------------- */

/*
 * One bit per optional datum/feature that fell back to its documented
 * default. A solver pass can test a whole feature class with a single mask
 * check instead of inspecting every component.
 */
enum : uint32_t {
    DEGRADED_NO_3D_HEIGHT        = 1u << 0,  /* height = 0, pure 2D */
    DEGRADED_NO_MASS             = 1u << 1,  /* mass = 0, no CoG/inertia */
    DEGRADED_NO_ROTATION_DATA    = 1u << 2,  /* nominal 0 deg, rotations free */
    DEGRADED_NO_PIN_ELEC         = 1u << 3,  /* electrical role inferred by name */
    DEGRADED_NO_PIN_CURRENT      = 1u << 4,  /* current rating unknown */
    DEGRADED_NO_NET_WEIGHTS      = 1u << 5,  /* uniform weight 1.0f */
    DEGRADED_NO_NET_CLASSES      = 1u << 6,  /* every net in the default class */
    DEGRADED_NO_DIFFPAIRS        = 1u << 7,  /* P/N treated as independent nets */
    DEGRADED_NO_KEEPOUTS         = 1u << 8,  /* no forbidden areas */
    DEGRADED_NO_STACKUP          = 1u << 9,  /* copper/dielectric ignored */
    DEGRADED_NO_CLEARANCE_MATRIX = 1u << 10, /* global clearance for all pairs */
    DEGRADED_NO_DECOUPLING       = 1u << 11, /* no IC/cap pairing data */
    DEGRADED_NO_THERMAL          = 1u << 12, /* no dissipated power */
    DEGRADED_NO_SYMMETRY         = 1u << 13, /* no symmetry relation */
    DEGRADED_NO_AIRFLOW          = 1u << 14, /* isotropic dissipation */
    DEGRADED_NO_CEILING          = 1u << 15, /* no z-interference test */
    DEGRADED_NO_VIA_RESTRICTION  = 1u << 16, /* vias allowed under any body */
    DEGRADED_COURTYARD_BBOX      = 1u << 17, /* courtyard = pad bbox + margin */
    DEGRADED_BOARD_OUTLINE_BBOX  = 1u << 18, /* outline = components bbox */
    DEGRADED_NO_MAX_LENGTH       = 1u << 19, /* HPWL only, no length bound */
    DEGRADED_NO_SEGREGATION      = 1u << 20  /* no analog/power floorplanning */
};

/* Every bit above; anything outside this mask is not a legal degradation. */
#define DEGRADED_ALL_MASK (DEGRADED_NO_3D_HEIGHT | DEGRADED_NO_MASS | \
                           DEGRADED_NO_ROTATION_DATA | DEGRADED_NO_PIN_ELEC | \
                           DEGRADED_NO_PIN_CURRENT | DEGRADED_NO_NET_WEIGHTS | \
                           DEGRADED_NO_NET_CLASSES | DEGRADED_NO_DIFFPAIRS | \
                           DEGRADED_NO_KEEPOUTS | DEGRADED_NO_STACKUP | \
                           DEGRADED_NO_CLEARANCE_MATRIX | DEGRADED_NO_DECOUPLING | \
                           DEGRADED_NO_THERMAL | DEGRADED_NO_SYMMETRY | \
                           DEGRADED_NO_AIRFLOW | DEGRADED_NO_CEILING | \
                           DEGRADED_NO_VIA_RESTRICTION | DEGRADED_COURTYARD_BBOX | \
                           DEGRADED_BOARD_OUTLINE_BBOX | DEGRADED_NO_MAX_LENGTH | \
                           DEGRADED_NO_SEGREGATION)

/* ------------------------------------------------------------------------- */
/* Ingestion: counts, options, descriptors                                    */
/* ------------------------------------------------------------------------- */

/* Exact element counts, supplied before any allocation happens. */
typedef struct {
    uint32_t num_comps;
    uint32_t num_pins;
    uint32_t num_nets;
    uint32_t num_net_entries; /* connected pins; <= num_pins */
    uint32_t num_polygons;    /* courtyards + outline + keepouts */
    uint32_t num_vertices;    /* summed over all polygons */
    uint32_t num_decoupling;
    uint32_t num_thermal;
    uint32_t num_symmetry;
    uint32_t num_diffpairs;
    uint32_t num_net_classes; /* 0 = no clearance matrix (<= 255) */
} scene_counts_t;

/* Explicit feature kills; 0 keeps every optional feature enabled. */
enum : uint32_t {
    OPT_DISABLE_DECOUPLING = 1u << 0,
    OPT_DISABLE_THERMAL    = 1u << 1,
    OPT_DISABLE_SYMMETRY   = 1u << 2,
    OPT_DISABLE_DIFFPAIRS  = 1u << 3,
    OPT_DISABLE_KEEPOUTS   = 1u << 4
};

/*
 * Solver configuration. Every zeroed scalar means "use the documented
 * default"; `placer_options_defaults` materialises them.
 */
typedef struct {
    coord_t     grid_origin_x;
    coord_t     grid_origin_y;
    coord_t     grid_fine;                /* default 0.1 mm */
    coord_t     grid_coarse;              /* default 0.5 mm */
    coord_t     courtyard_fallback_margin;/* default 0.25 mm */
    coord_t     global_clearance;         /* default 0.2 mm */
    coord_t     courtyard_clearance;      /* default 0.2 mm */
    side_mask_t allowed_sides;            /* default SIDE_BOTH */
    uint32_t    disable_mask;             /* OPT_DISABLE_* */
    size_t      scene_capacity;           /* 0 = sized from scene_counts_t */
    size_t      scratch_capacity;         /* 0 = sized from scene_counts_t */
} placer_options_t;

void placer_options_defaults(placer_options_t *opt);

/*
 * Component descriptor - the only AoS in the pipeline, alive during ingestion
 * only. Mandatory geometry: x/y and either an explicit courtyard AABB
 * (half_w/half_h > 0), or a courtyard polygon, or nothing at all (in which
 * case finalize derives pad-bbox + margin and raises DEGRADED_COURTYARD_BBOX).
 */
typedef struct {
    coord_t x;
    coord_t y;
    coord_t half_w;  /* <= 0 means "not provided" */
    coord_t half_h;
    bool    on_bottom;
    bool    locked;
    bool    rot_fixed;
    comp_kind_t kind;          /* COMP_KIND_*; 0 = unknown */
    bool    has_orientation;   /* false -> nominal 0 deg, raises NO_ROTATION */
    coord_t orientation_deg;   /* snapped to the nearest multiple of 90 */
    bool    has_height;        /* STEP/PDF enrichment */
    coord_t height;
    bool    has_mass;
    coord_t mass;
    const coord_t *polygon_xy; /* interleaved x,y courtyard; may be null */
    uint32_t polygon_n;        /* vertex count, >= 3 to be honoured */
} component_desc_t;

/*
 * Pin descriptor. `name` is ingestion-time only (used to infer the electrical
 * role when flags_valid is false) and is never stored: pin names would be the
 * largest string pool in the engine and the solver has no use for them.
 */
typedef struct {
    place_id_t  comp;      /* owning component id */
    coord_t     offset_x;  /* mm, unrotated footprint frame */
    coord_t     offset_y;
    const char *name;      /* may be null */
    bool        flags_valid;
    pin_flags_t flags;
    coord_t     max_current; /* A; 0 = unknown */
} pin_desc_t;

/* Net descriptor. weight == 0 / net_class == 0xFF mean "not provided". */
typedef struct {
    coord_t weight;
    uint8_t net_class;
} net_desc_t;

/* ------------------------------------------------------------------------- */
/* Validation report (fixed size, never allocates)                            */
/* ------------------------------------------------------------------------- */

#define PLACE_VALIDATION_MAX_MESSAGES 16
#define PLACE_VALIDATION_MESSAGE_LEN  96

typedef struct {
    uint32_t errors;
    uint32_t warnings;
    uint32_t message_count;
    char     messages[PLACE_VALIDATION_MAX_MESSAGES][PLACE_VALIDATION_MESSAGE_LEN];
} validation_report_t;

void validation_report_clear(validation_report_t *report);

/* ------------------------------------------------------------------------- */
/* Context                                                                    */
/* ------------------------------------------------------------------------- */

/* Cold, ingestion-only bookkeeping; never touched by the solver. */
typedef struct {
    scene_counts_t   counts;
    placer_options_t opt;
    place_id_t       last_pin_comp; /* enforces pin grouping by component */
    bool             has_pins;
    bool             finalized;
} placer_ingest_state_t;

typedef struct {
    /* Memory: exactly two contiguous blocks own everything below. */
    arena_t scene;
    arena_t scratch;

    /* Dense SoA - the solver's hot data. */
    components_soa_t comps;
    pins_soa_t       pins;
    netlist_csr_t    nets;

    /* Sparse polymorphic constraint tables. */
    constraints_table_t constraints;

    /* Board-level configuration. */
    design_rules_t rules;
    stackup_t      stackup;
    grid_t         grid;

    /* Degraded-mode reporting. */
    uint32_t degraded;
    uint32_t warnings;
    uint32_t errors;

    /* Ingestion state (cold). */
    placer_ingest_state_t ingest;
} placer_context_t;

/* --- lifecycle ----------------------------------------------------------- */

/*
 * Pass 1. Zeroes `ctx`, sizes and creates both arenas, and pre-allocates
 * every array of the scene. Returns false on allocation failure; `ctx` is
 * then safe to destroy.
 */
[[nodiscard]] bool placer_context_begin(placer_context_t *ctx,
                                        const scene_counts_t *counts,
                                        const placer_options_t *opt);

/* Releases both arena blocks. Safe on a zeroed context; idempotent. */
void placer_context_destroy(placer_context_t *ctx);

/* --- pass 2: fill -------------------------------------------------------- */

/*
 * All placer_add_* functions return PLACE_ID_NONE / false when the matching
 * capacity from `scene_counts_t` is exhausted or an argument is invalid;
 * `ctx->errors` is incremented and finalize will refuse to succeed.
 */
place_id_t placer_add_component(placer_context_t *ctx, const component_desc_t *desc);
place_id_t placer_add_pin(placer_context_t *ctx, const pin_desc_t *desc);
place_id_t placer_add_net(placer_context_t *ctx, const net_desc_t *desc,
                          const place_id_t *pin_ids, uint32_t pin_count);
place_id_t placer_add_polygon(placer_context_t *ctx, poly_kind_t kind,
                              const coord_t *xy, uint32_t vertex_count,
                              uint32_t layer_mask);

/*
 * Connects an already-created pin to an already-created net. Order-independent:
 * the CSR adjacency is built by counting in finalize, not by insertion.
 */
[[nodiscard]] bool placer_connect(placer_context_t *ctx, place_id_t pin, place_id_t net);

/*
 * Bulk ingestion: declares that the pre-allocated arrays have been filled
 * directly (e.g. by the scene.bin loader) and that `counts` is authoritative.
 * Everything derived - pin ranges, net CSR, constraint indices, courtyard and
 * outline fallbacks - is still produced by placer_context_finalize(), which
 * must be called next. Returns false on out-of-capacity or invalid state.
 */
[[nodiscard]] bool placer_context_adopt(placer_context_t *ctx, const scene_counts_t *counts);

/* Optional enrichment tables (blocks 3/6/9). Rows must be pushed in ascending
 * order of their primary component key. */
[[nodiscard]] bool placer_add_decoupling(placer_context_t *ctx, place_id_t ic_comp,
                                         place_id_t cap_comp, place_id_t ic_pin,
                                         coord_t max_dist_mm, coord_t weight);
[[nodiscard]] bool placer_add_thermal(placer_context_t *ctx, place_id_t comp,
                                      coord_t power_w, coord_t exclusion_radius_mm,
                                      coord_t r_theta_ja);
[[nodiscard]] bool placer_add_symmetry(placer_context_t *ctx, place_id_t comp_a,
                                       place_id_t comp_b, sym_axis_t axis,
                                       coord_t weight);
[[nodiscard]] bool placer_add_diffpair(placer_context_t *ctx, place_id_t net_p,
                                       place_id_t net_n, coord_t max_skew_mm);

/* --- pass 3: derive, default, verify ------------------------------------- */

/*
 * Builds the pin->component CSR, the net CSR, the constraint indices, applies
 * every documented fallback, marks the DEGRADED_* bits, and empties the
 * scratch arena. Returns false when mandatory data is missing or inconsistent
 * (empty netlist, ungrouped pins, constraint rows out of order, OOM...).
 */
[[nodiscard]] bool placer_context_finalize(placer_context_t *ctx);

/*
 * Structural self-check: null pointers, CSR monotonicity and bounds, id
 * ranges, geometry sanity. Never dereferences a pointer it has not proven
 * non-null, so it is safe to run on a deliberately corrupted context.
 */
[[nodiscard]] bool placer_context_validate(const placer_context_t *ctx,
                                           validation_report_t *report);

/* Byte estimate used when `placer_options_t.scene_capacity == 0`. */
[[nodiscard]] size_t placer_context_estimate_scene_bytes(const scene_counts_t *counts);
[[nodiscard]] size_t placer_context_estimate_scratch_bytes(const scene_counts_t *counts);

#endif /* PLACE_CONTEXT_H */
