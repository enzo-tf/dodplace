/*
 * wa_wirelength.h - the weighted-average wirelength of one net.
 *
 * A net's length is a bounding box, and a bounding box is not differentiable:
 * it is decided by two pins and blind to every other one, so a placer cannot
 * tell "this pin is the leftmost" from "this pin is about to become the
 * leftmost". The weighted average replaces max and min by soft ones:
 *
 *     hi = sum v_i exp(v_i / g) / sum exp(v_i / g)
 *
 * which is above the maximum, converges to it as g falls, and moves smoothly
 * with every pin on the net. The pair (hi - lo) is the standard WA estimate
 * used by ePlace and DREAMPlace; here it prices the assignment matrix, so the
 * matrix and the metric the engine is judged by point the same way.
 */
#ifndef PLACE_WA_WIRELENGTH_H
#define PLACE_WA_WIRELENGTH_H

#include "solver_internal.h"

/* The WA length of `net`, with `moved_pin` posed at (px, py). Pass
 * PLACE_ID_NONE for `moved_pin` to measure the net as it stands. */
coord_t wa_net_length(const solver_t *s, uint32_t net, uint32_t moved_pin, coord_t px,
                      coord_t py, coord_t gamma);

#endif /* PLACE_WA_WIRELENGTH_H */
