/*
 * copper_test.h - the metal of a part against the metal of another.
 *
 * The courtyard rule is not enough on its own. Two parts whose courtyards are
 * apart can still have their pads meet in the gap between them - on r10, 13
 * pairs did exactly that, and no bounding box can see it, because the copper
 * that matters is outside both courtyards and faces the neighbour.
 *
 * A pad is a rectangle at (component + rotated offset), half extents rotated
 * with it; two parts conflict when a pad of one is closer to a pad of the
 * other than the copper margin.
 */
#ifndef PLACE_COPPER_TEST_H
#define PLACE_COPPER_TEST_H

#include "solver_internal.h"

/* Do the pads of the two parts, posed as given, come within `margin`? */
bool copper_overlaps(const solver_t *s, uint32_t i, coord_t xi, coord_t yi, uint32_t j,
                     coord_t xj, coord_t yj, coord_t margin);

#endif /* PLACE_COPPER_TEST_H */
