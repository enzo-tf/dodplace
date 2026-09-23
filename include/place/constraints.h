/*
 * constraints.h - sparse, flat constraint tables (table-driven polymorphism).
 *
 * Rationale
 * ---------
 * A 2-pin decoupling capacitor and a 200-pin BGA are both `components_soa_t`
 * rows: they cost the same, and neither pays for constraints it does not have.
 * All the "rich" behaviour lives in separate sparse tables keyed by component
 * id, so the solver's hot loop only ever touches dense arrays, and the rare
 * constraints are reached through a per-component CSR index (`first_by_comp`,
 * size num_comps + 1) built once during ingestion - an O(1) lookup with no
 * tag byte, no vtable and no per-component cost when a table is empty.
 *
 * Rows of every table must be pushed in ascending order of their primary
 * component key; `placer_context_finalize` verifies this and builds the CSR
 * index by counting.
 */
#ifndef PLACE_CONSTRAINTS_H
#define PLACE_CONSTRAINTS_H

#include "place/types.h"

#include <stdint.h>

/* ------------------------------------------------------------------------- */
/* Polygon pool: exact courtyards, board outline, keepouts                    */
/* ------------------------------------------------------------------------- */

typedef uint8_t poly_kind_t;

enum : poly_kind_t {
    POLY_KIND_COURTYARD     = 0,
    POLY_KIND_BOARD_OUTLINE = 1,
    POLY_KIND_KEEPOUT       = 2
};

static_assert(sizeof(poly_kind_t) == 1, "poly_kind_t must stay 1 byte");

/*
 * All polygon vertices of the scene live in two flat arrays; polygon `p`
 * owns [poly_offsets[p], poly_offsets[p+1]). Vertices are mm, in the
 * footprint frame for courtyards, in board coordinates otherwise.
 */
typedef struct {
    coord_t     *vx;              /* num_vertices */
    coord_t     *vy;              /* num_vertices */
    uint32_t    *poly_offsets;    /* num_polys + 1 */
    poly_kind_t *poly_kind;       /* num_polys */
    uint32_t    *poly_layer_mask; /* keepouts: copper layer bitmask; 0 = all */
    uint32_t     num_polys;
    uint32_t     num_vertices;
} polygon_pool_t;

/* ------------------------------------------------------------------------- */
/* Block 6 - hard constraints                                                 */
/* ------------------------------------------------------------------------- */

/*
 * Decoupling: pull `cap_comp` next to `ic_comp` (and optionally to a specific
 * `ic_pin` such as VDD). Distances are stored squared so the solver never
 * pays a sqrt in the inner loop. Indexed by `ic_comp`.
 */
typedef struct {
    place_id_t *ic_comp;      /* primary key */
    place_id_t *cap_comp;
    place_id_t *ic_pin;       /* PLACE_ID_NONE = any power pin */
    coord_t    *max_dist_sq;  /* mm^2 */
    coord_t    *weight;       /* pull strength, > 0 */
    uint32_t   *first_by_comp;/* num_comps + 1, or nullptr when empty */
    uint32_t    count;
    uint32_t    capacity;
} constraint_decoupling_t;

/*
 * Symmetry / mirror relation between two components. Indexed by `comp_a`.
 */
typedef uint8_t sym_axis_t;

enum : sym_axis_t {
    SYM_AXIS_X = 0, /* mirror about a vertical axis  (same Y) */
    SYM_AXIS_Y = 1, /* mirror about a horizontal axis (same X) */
    SYM_FIXED = 2   /* relative pose must be preserved exactly */
};

static_assert(sizeof(sym_axis_t) == 1, "sym_axis_t must stay 1 byte");

typedef struct {
    place_id_t *comp_a;       /* primary key */
    place_id_t *comp_b;
    sym_axis_t *axis;
    coord_t    *weight;
    uint32_t   *first_by_comp;/* num_comps + 1, or nullptr when empty */
    uint32_t    count;
    uint32_t    capacity;
} constraint_symmetry_t;

/* ------------------------------------------------------------------------- */
/* Block 9 - thermal                                                          */
/* ------------------------------------------------------------------------- */

/*
 * Thermal envelope of a dissipating component. `r_theta_ja` == 0 means the
 * junction model is unknown, in which case the solver only enforces the
 * exclusion radius. Indexed by `comp`.
 */
typedef struct {
    place_id_t *comp;             /* primary key */
    coord_t    *power_w;          /* dissipated power, W */
    coord_t    *exclusion_radius; /* keep-out radius around the part, mm */
    coord_t    *r_theta_ja;       /* K/W; 0 = unknown */
    uint32_t   *first_by_comp;    /* num_comps + 1, or nullptr when empty */
    uint32_t    count;
    uint32_t    capacity;
} constraint_thermal_t;

/* ------------------------------------------------------------------------- */
/* Block 3 - differential pairs                                               */
/* ------------------------------------------------------------------------- */

typedef struct {
    place_id_t *net_p;
    place_id_t *net_n;
    coord_t    *max_skew_mm;  /* 0 = length matching not constrained */
    uint32_t    count;
    uint32_t    capacity;
} diffpair_table_t;

/* ------------------------------------------------------------------------- */
/* Aggregate                                                                  */
/* ------------------------------------------------------------------------- */

enum : uint8_t {
    CT_PRESENT_DECOUPLING = 0x01u,
    CT_PRESENT_THERMAL    = 0x02u,
    CT_PRESENT_SYMMETRY   = 0x04u,
    CT_PRESENT_DIFFPAIRS  = 0x08u,
    CT_PRESENT_POLYGONS   = 0x10u
};

typedef struct {
    constraint_decoupling_t decoupling;
    constraint_thermal_t    thermal;
    constraint_symmetry_t   symmetry;
    diffpair_table_t        diffpairs;
    polygon_pool_t          polygons;
    uint8_t                 present; /* CT_PRESENT_* bits */
} constraints_table_t;

/* Returns the polygon id of the first polygon of `kind`, or PLACE_ID_NONE. */
static inline place_id_t constraints_find_polygon(const polygon_pool_t *pool, poly_kind_t kind)
{
    if (pool == nullptr) {
        return PLACE_ID_NONE;
    }
    for (uint32_t p = 0; p < pool->num_polys; ++p) {
        if (pool->poly_kind[p] == kind) {
            return p;
        }
    }
    return PLACE_ID_NONE;
}

#endif /* PLACE_CONSTRAINTS_H */
