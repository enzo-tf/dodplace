/*
 * solver.h - the placement engine.
 *
 * Four stages, in the order a professional placer runs them:
 *
 *   1. semantic clustering   patterns in the netlist hypergraph are condensed
 *                            into rigid super-nodes, so a switching converter
 *                            moves as a block instead of being scattered
 *   2. global placement      force-directed relaxation: star-model attraction
 *                            along the nets, volumetric repulsion between
 *                            courtyards, mechanical parts as anchors
 *   3. untangling            simulated annealing on the discrete choices
 *                            (position swaps and 90-degree rotations) against a
 *                            cost that mixes wirelength, ratsnest crossings
 *                            and overlaps - because two layouts with the same
 *                            HPWL can have opposite routability
 *   4. legalisation          exact geometric separation by minimum translation
 *                            vector, plus confinement to the board outline
 *
 * Everything works on flat arrays in the scratch arena: no allocation, no
 * pointer chasing, no dependency on the scene being mutable.
 */
#ifndef PLACE_SOLVER_H
#define PLACE_SOLVER_H

#include "place/context.h"
#include "place/io.h"

#include <stdbool.h>
#include <stdint.h>

/* How the global stage pushes parts apart. Explicit values: they are written
 * into reports and compared by the harness. */
typedef enum : uint8_t {
    GLOBAL_MODEL_REPULSION = 0, /* pairwise 1/d^2 sum, O(n^2) per iteration */
    GLOBAL_MODEL_ANALYTIC  = 1  /* ePlace density Poisson solve on a bin grid */
} global_model_t;

typedef struct {

    coord_t attraction;           /* net pull strength (1.0) */
    coord_t repulsion;            /* 0 = derived from the board area */
    coord_t net_weight_scale;     /* multiplies the per-net weight (1.0) */

    /* --- the global model ----------------------------------------------- */
    /* How parts are pushed apart. `repulsion` is the original pairwise
     * 1/d^2 sum, O(n^2) per iteration; `analytic` solves the ePlace density
     * Poisson equation on a bin grid and costs a transform instead. */
    global_model_t global_model;
    coord_t w_density;            /* density force, relative to the net pull (1.0) */
    coord_t momentum;            /* Nesterov look-ahead, 0 = plain descent (0.9) */
    uint32_t density_bins;       /* bins per axis; power of two is enough (64) */
    /* --- effort --------------------------------------------------------- */
    uint32_t global_iterations;   /* force-directed relaxation steps (400) */
    uint32_t refine_moves;
    coord_t  anneal_t_start_ratio; /* 0.01: start temperature, as a share of the score */
    coord_t  anneal_t_end_ratio;   /* 1e-3: end temperature, as a share of the start */        /* annealing moves (12000) */
    uint32_t legalize_passes;
    uint32_t legalize_rounds;   /* slot-search rounds after the pushes settle */     /* push-out passes (80) */

    /* --- cost weights --------------------------------------------------- */
    coord_t w_hpwl;               /* 1.0  */
    coord_t w_crossings;          /* 0.6  */
    coord_t w_overlaps;           /* 4.0  */
    coord_t w_overlap_depth;      /* 2.0, mm of penetration */
    coord_t w_displacement;       /* 0.5, mm moved from where the designer put it */
    coord_t w_thermal;            /* 3.0, mm^2 of thermal exclusion violated */
    uint32_t max_net_degree;      /* 15: a net wider than this is a rail */
    bool     exclude_plane_nets;  /* skip ground/power rails in HPWL and RUDY */
    coord_t w_decoupling;         /* 2.0, mm^2 a capacitor sits beyond its pin */
    coord_t w_diffpair;           /* 4.0, mm^2 of length mismatch on a pair */
    coord_t swap_prob;            /* 0.15: share of moves that are part swaps */
    uint32_t swap_max_pins;       /* 6: only parts this small can be swapped (0 = any) */
    uint32_t swap_window;         /* 5: parts per permutation window (0 = off, max 6) */
    uint32_t swap_assign_min;     /* 6: smallest bucket solved exactly (0 = off) */
    coord_t pad_clearance;
    coord_t polish_reach;         /* 0.20, mm under which the mask polish acts */        /* 0.25, mm between two parts' copper */
    coord_t max_slip_x;           /* mm a part may leave its anchor; 0 = derive */
    coord_t max_slip_y;           /* mm; the row of a regular board is ~0.12 pitch */
    coord_t anchor_soft_scale;    /* 2.5, lanes granted to a part that arrived in conflict */
    coord_t thermal_min_power;    /* 0.3 W: below this a part is not a heat source */

    /* --- discrete choices ----------------------------------------------- */
    uint32_t rotation_mask;       /* allowed orientations, bit o = o*90 degrees */
    bool     allow_side_flip;     /* flip a part to the other side (false) */
    bool     respect_rot_fixed;   /* honour COMP_ROT_FIXED (true) */

    /* --- stages --------------------------------------------------------- */
    bool enable_clustering;
    bool enable_matching;   /* discrete assignment of decoupling passives */
    bool enable_global;
    bool enable_refine;
    bool enable_legalize;

    /* --- geometry ------------------------------------------------------- */
    coord_t board_margin;         /* keep-out from the outline, mm (0.5) */
    coord_t courtyard_clearance;  /* extra gap to enforce, mm (from rules) */
    coord_t crossing_cell;        /* ratsnest crossing grid, mm (4.0) */

    uint32_t seed;                /* deterministic runs (1) */
} solver_options_t;

typedef struct {
    uint32_t clusters;            /* super-nodes built in stage 1 */
    uint32_t clustered_components;
    uint32_t movable;             /* components free to move */
    uint32_t matched;             /* passives assigned to a slot by matching */

    coord_t  hpwl_before;
    coord_t  hpwl_after;
    uint32_t crossings_before;
    uint32_t crossings_after;
    uint32_t overlaps_before;
    uint32_t overlaps_after;

    uint32_t moves_tried;
    uint32_t moves_accepted;
    uint32_t moves_walled;        /* refused: outside the part's anchor box */
    uint32_t legalize_pushes;
    uint32_t unplaced_before;     /* true overlaps the solver had to fix, in the input */
    uint32_t unplaced;            /* courtyards still truly overlapping */
    uint32_t unplaced_fixable;    /* ... of which the solver could still fix */
    uint32_t locked_overlaps;     /* true overlaps between two locked parts: not ours */
    uint32_t too_tall;
    uint32_t polished;
    uint32_t swaps_applied;            /* micro-moves made to satisfy the mask rule */            /* parts taller than the ceiling, frozen in place */
    uint32_t clearance_violations;/* pairs closer than the courtyard clearance */
    size_t   scratch_needed;      /* set when the arena was too small */
    size_t   scratch_peak;        /* high-water mark of the scratch arena */
    coord_t  seconds;             /* the whole solve */

    /* --- the same number, split by stage (a bench needs to know where the
     * time went, not just how much there was) ---------------------------- */
    coord_t  seconds_cluster;
    coord_t  seconds_incumbent;   /* legalising the input: the pose to beat */
    coord_t  seconds_global;
    coord_t  seconds_refine;
    coord_t  seconds_legalize;
    coord_t  seconds_evaluate;    /* cost calls, all stages together */
} solver_stats_t;

void solver_options_defaults(solver_options_t *opt);

/*
 * Places every component and fills `out` with one record per component, in
 * component-index order (the same order as the scene). `capacity` must be at
 * least `ctx->comps.count`. Returns false when the scene is not finalised or
 * the scratch arena cannot hold the solver state.
 */
[[nodiscard]] bool placer_solve(const placer_context_t *ctx, const solver_options_t *opt,
                                placement_entry_t *out, uint32_t capacity,
                                solver_stats_t *stats);

#endif /* PLACE_SOLVER_H */
