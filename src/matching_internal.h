/*
 * matching_internal.h - what the matching modules share.
 *
 * The assignment is built in three steps, one module each:
 *   matching_plan.c    which passives take part, and what they aim at
 *   matching_slots.c   the free slots around a target, and their cost
 *   matching_auction.c the assignment itself and the commit
 *
 * Invariants:
 *   - a row's slots are all proved clear for that passive's own footprint by
 *     raster_mask_box_clear and then by the exact clearance test;
 *   - a cost is the marginal wirelength of the move with every other pin
 *     frozen (matching_slots.c), so the auction's optimum is a real gain;
 *   - rows are independent: the auction needs the cost of a pair to depend on
 *     that pair alone, and the frozen-neighbour model gives exactly that.
 */
#ifndef PLACE_MATCHING_INTERNAL_H
#define PLACE_MATCHING_INTERNAL_H

#include "matching.h"
#include "raster_mask.h"

#define MATCH_MAX_SLOTS 48u   /* candidates kept per passive */
#define MATCH_MAX_PARTS 256u  /* passives handled in one run */
#define MATCH_LATTICE 0.5f    /* slot grid, mm */
#define MATCH_EPS 0.01f       /* a bid must beat the price by this much */
#define MATCH_MAX_ITER 64u

typedef struct {
    uint32_t comp;
    uint32_t count;
    coord_t  tx, ty;              /* the target the rule names */
    coord_t  cx[MATCH_MAX_SLOTS];
    coord_t  cy[MATCH_MAX_SLOTS];
    coord_t  cost[MATCH_MAX_SLOTS];
} match_row_t;

/* One row per movable passive of the decoupling table. Returns the count. */
uint32_t matching_build_rows(solver_t *s, const raster_mask_t *mask, match_row_t *rows,
                             uint32_t max_rows);

/* The free slots of one row, filled in place. Returns how many were kept. */
uint32_t matching_collect_slots(const solver_t *s, const raster_mask_t *mask, uint32_t comp,
                                coord_t tx, coord_t ty, coord_t reach, match_row_t *row);

#endif /* PLACE_MATCHING_INTERNAL_H */
