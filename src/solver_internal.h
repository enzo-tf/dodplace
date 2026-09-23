/*
 * solver_internal.h - state shared by the four solver stages.
 *
 * Nothing here is public: the stages exchange data through this struct, which
 * lives in the scratch arena and is thrown away when the solve ends.
 */
#ifndef PLACE_SOLVER_INTERNAL_H
#define PLACE_SOLVER_INTERNAL_H

#include "place/solver.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>

#define SOLVER_NO_INDEX UINT32_MAX

/* ------------------------------------------------------------------------- */
/* A uniform grid over the board, rebuilt from scratch on every evaluation.   */
/* Both the overlap count and the ratsnest crossing count need "which items    */
/* are near which other items", and a two-pass counting sort is the cheapest  */
/* way to get it without a hash table.                                        */
/* ------------------------------------------------------------------------- */

typedef struct {
    uint32_t cols;
    uint32_t rows;
    coord_t  cell;
    coord_t  origin_x;
    coord_t  origin_y;
    uint32_t *start;   /* cols*rows + 1 */
    uint32_t *cursor;  /* cols*rows, scatter position */
    uint32_t *items;
    uint32_t item_budget;      /* capacity of `items`, in registrations */
    uint32_t max_per_cell;     /* registrations accepted per cell */   /* entry indices, grouped by cell */
    uint32_t *home;    /* entry -> the cell holding its centre */
    /* The cell range each entry covers. Two entries share several cells, so a
     * pair is counted only in the first cell of the intersection of their
     * ranges - exactly once, and without a visited-set. */
    uint32_t *cx0, *cy0, *cx1, *cy1;
} solver_grid_t;

/* ------------------------------------------------------------------------- */

/* Stride of the per-orientation pin tables: rot_dx[orient * PIN_STRIDE(s) + pin]. */
#define PIN_STRIDE(s) ((size_t)(s)->npin)

typedef struct {
    const placer_context_t *ctx;
    uint32_t npin;
    uint32_t ncomp;
    const solver_options_t *opt;

    /* --- working placement (the scene itself is never modified) ---------- */
    coord_t *x;
    coord_t *y;
    uint8_t *orient;      /* 0..3 */
    uint8_t *side;        /* 0 top, 1 bottom */

    /* Pin offsets pre-rotated for the four orientations, so evaluating a pin
     * position is one addition: rot_dx[o * npin + pin]. */
    coord_t *rot_dx;
    coord_t *rot_dy;

    /* --- rigid super-nodes from stage 1 --------------------------------- */
    uint32_t *cluster_of;      /* component -> cluster, or SOLVER_NO_INDEX */
    uint32_t *member_comp;     /* flat list of clustered components */
    uint8_t *member_dorient;   /* satellite orientation relative to its master */
    coord_t  *member_dx;       /* offset from the master, master frame */
    coord_t  *member_dy;
    uint32_t *cluster_first;   /* cluster -> first member */
    uint32_t *cluster_count;
    uint32_t *cluster_master;
    uint32_t  nclusters;

    /* The copper of each part as one box, in the part's own frame. The
     * courtyard box is not enough to decide whether a pair needs the copper
     * test: a footprint's pads can overhang it, and then two parts whose
     * courtyards are far apart still have metal that touches. */
    coord_t *copper_cx;
    coord_t *copper_cy;
    coord_t *copper_hw;
    coord_t *copper_hh;

    /* Nets that are rails rather than traces: ground and power, and anything
     * wider than max_net_degree. Built once at init. */
    uint8_t *net_plane;

    /* --- declared heat sources, above the power threshold ------------------ */
    uint32_t *hot;
    uint32_t  nhot;

    /* Through-hole pads: a plated hole carries copper on both faces, so a part
     * that has one collides with parts on the other side too. Derived from the
     * pin flags at init; `pth_known` is false when the scene carries none and
     * the board should have some, in which case the side filter is not applied
     * at all rather than silently allowing cross-side overlaps. */
    uint8_t *has_pth;
    bool     pth_known;

    /* The designer's placement, kept as an immutable anchor, and how far a
     * part may leave it. The coarse position of a schematic capture carries
     * the signal flow; letting parts drift freely destroys it, which is what
     * pushed r10's wirelength from 31.7 m to 41.4 m. */
    coord_t *anchor_x;
    coord_t *anchor_y;
    coord_t  slip_x;
    coord_t  slip_y;
    /* A part that arrives already overlapping something cannot be held to its
     * lane: the designer's position is the reference only when it is legal. It
     * gets a wider box, everything else keeps the strict one. */
    uint8_t *anchor_soft;

    /* Parts taller than the enclosure: no slot on the board can take them, so
     * they keep their place and stay obstacles for everything else. */
    uint8_t  *frozen;
    uint32_t  nfrozen;

    /* --- movable set ------------------------------------------------------ */
    uint32_t *movable;         /* components the annealing may move */
    uint32_t  nmovable;

    /* --- geometry cache -------------------------------------------------- */
    coord_t board_min_x, board_min_y, board_max_x, board_max_y;
    /* The outline itself, margin excluded. The margin keeps parts off the edge
     * because a router needs room there - a preference, not a rule. The hard
     * bound is the outline, and the last-resort slot search is allowed to use
     * it: a part 0.4 mm from the edge is a compromise, a part on top of a
     * frozen neighbour is a broken board. */
    coord_t edge_min_x, edge_min_y, edge_max_x, edge_max_y;
    coord_t *half_w;           /* effective half extents, orientation applied */
    coord_t *half_h;

    /* --- scratch for the cost function ----------------------------------- */
    arena_t *scratch;
    solver_grid_t comp_grid;   /* courtyard overlaps */
    solver_grid_t seg_grid;    /* ratsnest crossings */
    coord_t *seg_x1, *seg_y1, *seg_x2, *seg_y2;
    uint32_t *seg_net;
    uint32_t *seg_count_start; /* per net: first segment */
    uint8_t  *net_movable;     /* net touches at least one movable component */
    uint32_t  nsegments;

    /* --- snapshot of the best layout found by the annealing --------------- */
    coord_t *best_x, *best_y;
    uint8_t *best_orient;
    coord_t  best_score;
    bool     have_best;

    /* Ruin-and-recreate: parts temporarily lifted out of the way while a
     * pocket is rebuilt. Zero in normal operation. */
    uint8_t *ruin_mask;

    /* --- misc ------------------------------------------------------------- */
    uint32_t rng;
    bool     profile;              /* DODPLACE_SOLVER_PROFILE */
    uint32_t profile_calls;
    uint64_t profile_hpwl_ns;
    uint64_t profile_ratsnest_ns;
    uint64_t profile_crossings_ns;
    uint64_t profile_overlaps_ns;
    solver_stats_t stats;
    bool     ok;
} solver_t;

/*
 * Every stage allocates from the scratch arena and sets `ok` to false on
 * failure, so a stage can bail out at any point and the entry point cleans up.
 */
static inline void *solver_alloc_block(solver_t *s, size_t bytes)
{
    void *p = arena_alloc0(s->scratch, bytes, PLACE_CACHELINE);
    if (p == nullptr) {
        s->ok = false;
    }
    return p;
}

#define SOLVER_ALLOC(s, T, n) ((T *)solver_alloc_block((s), sizeof(T) * (size_t)(n)))

/* --- helpers shared by the stages (solver.c) ----------------------------- */

/* A pair of axis-aligned boxes is clear when they are apart on either axis. */
static inline bool boxes_overlap(coord_t ax, coord_t ay, coord_t ahw, coord_t ahh, coord_t bx,
                                 coord_t by, coord_t bhw, coord_t bhh, coord_t gap)
{
    return (fabsf(ax - bx) < (ahw + bhw + gap)) && (fabsf(ay - by) < (ahh + bhh + gap));
}

/* Two parts on opposite sides share no copper and no courtyard: KiCad checks a
 * side at a time, and comparing them in 2D is what manufactured 74 phantom
 * collisions on r10 - the solver then fled conflicts that do not exist. */
static inline bool same_side(const solver_t *s, uint32_t i, uint32_t j)
{
    return s->side[i] == s->side[j];
}

/* A pair of boxes given as extents relative to their centres. */
static inline bool extents_overlap(coord_t a_lo_x, coord_t a_lo_y, coord_t a_hi_x,
                                   coord_t a_hi_y, coord_t b_lo_x, coord_t b_lo_y,
                                   coord_t b_hi_x, coord_t b_hi_y, coord_t gap)
{
    return (a_lo_x - b_hi_x < gap) && (b_lo_x - a_hi_x < gap) && (a_lo_y - b_hi_y < gap) &&
           (b_lo_y - a_hi_y < gap);
}

void solver_union_extent(const solver_t *s, uint32_t comp, coord_t *lo_x, coord_t *lo_y,
                         coord_t *hi_x, coord_t *hi_y);

coord_t solver_hpwl(const solver_t *s);
void    solver_rebuild_extents(solver_t *s);
bool    solver_rotation_fits(const solver_t *s, uint32_t comp, uint8_t orient);
bool    solver_within_anchor(const solver_t *s, uint32_t comp, coord_t x, coord_t y);
void    solver_clamp_to_anchor(solver_t *s, uint32_t comp);
void    solver_set_position(solver_t *s, uint32_t comp, coord_t px, coord_t py);
void    solver_apply_cluster(solver_t *s, uint32_t cluster, coord_t mx, coord_t my);
void    solver_sync_cluster_members(solver_t *s);
uint32_t solver_component_cluster(const solver_t *s, uint32_t comp);
bool    solver_component_is_movable(const solver_t *s, uint32_t comp);
coord_t solver_rand_unit(solver_t *s);   /* uniform in [0, 1) */
uint32_t solver_rand_below(solver_t *s, uint32_t bound);

/* --- stages -------------------------------------------------------------- */
void solver_cluster(solver_t *s);
void solver_global(solver_t *s);
void solver_refine(solver_t *s);
void solver_legalize(solver_t *s);

/* --- cost (solver.c) ----------------------------------------------------- */

typedef struct {
    coord_t  hpwl;
    uint32_t crossings;
    uint32_t overlaps;
    coord_t  overlap_depth; /* mm of penetration summed over the overlapping pairs */
    coord_t  displacement;  /* mm the movable parts sit away from the input */
    coord_t  thermal;
    coord_t  decoupling;    /* mm^2 capacitors sit beyond their pin */
    coord_t  diffpair;      /* mm^2 of length mismatch between paired nets */       /* mm^2 of thermal exclusion the layout violates */
    coord_t  score;
} solver_cost_t;

/* Rebuilds both grids and counts. This is the hot path of stage 3. */
solver_cost_t solver_evaluate(solver_t *s);

/*
 * Courtyard pairs that overlap (within the courtyard clearance), written as
 * index pairs into `out_i`/`out_j`. Returns the number found, capped at `max`.
 * Shared by the overlap cost and by legalisation, so both see the same pairs.
 */
uint32_t solver_collect_overlap_pairs(solver_t *s, uint32_t *out_i, uint32_t *out_j,
                                      uint32_t max);

/* Same, with an explicit gap between courtyard edges (0 = true overlap). */
uint32_t solver_collect_conflicts(solver_t *s, coord_t gap, uint32_t *out_i, uint32_t *out_j,
                                  uint32_t max);
/* Counts conflicts without materialising them. */
uint32_t solver_count_conflicts(solver_t *s, coord_t gap);
/* Same, but only pairs where at least one side may still move. */
uint32_t solver_count_locked_conflicts(solver_t *s, coord_t gap);
coord_t  solver_overlap_depth(solver_t *s, coord_t gap);

/* The one pass behind every overlap number: pairs of courtyard boxes that
 * collide, split into the solver's own (at least one side movable) and the
 * designer's (both locked), with the summed penetration depth on request. */
uint32_t count_conflicts(solver_t *s, coord_t gap, uint32_t *locked_pairs, coord_t *depth_out);
coord_t  solver_displacement(solver_t *s);
coord_t  solver_thermal_cost(const solver_t *s);
coord_t  solver_decoupling_cost(const solver_t *s);
coord_t  solver_diffpair_cost(const solver_t *s);
uint8_t  solver_torque_orientation(const solver_t *s, uint32_t comp);

/* Clamps a component (or the cluster it belongs to) inside the board box. */
void solver_clamp_to_board(solver_t *s, uint32_t comp);
/* Moves a component by (dx, dy); a clustered satellite moves its whole cluster. */
void solver_nudge(solver_t *s, uint32_t comp, coord_t dx, coord_t dy);

#endif /* PLACE_SOLVER_INTERNAL_H */
