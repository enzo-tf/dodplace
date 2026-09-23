/*
 * shape.h - do two components collide?
 *
 * Two rules, applied in that order:
 *
 *   1. Courtyards must not overlap - the rule KiCad's DRC checks, and the
 *      cheap test. Two parts on opposite sides of the board share no courtyard
 *      and no copper, so they are skipped, unless one of them has a plated
 *      through hole: that carries copper on both faces.
 *   2. Copper must not come within the mask margin (copper_test.c). The
 *      courtyards can be apart while the pads meet in the gap between them,
 *      and no bounding box sees that - it was 13 shorts on r10.
 */
#ifndef PLACE_SHAPE_H
#define PLACE_SHAPE_H

#include "solver_internal.h"

/* Do the two parts collide, at their current poses? */
bool shape_overlaps(const solver_t *s, uint32_t i, uint32_t j, coord_t gap);

/* Same, with part `i` posed at (xi, yi) - what a slot search needs to ask. */
bool shape_overlaps_posed(const solver_t *s, uint32_t i, coord_t xi, coord_t yi, uint32_t j,
                          coord_t gap);

#endif /* PLACE_SHAPE_H */
