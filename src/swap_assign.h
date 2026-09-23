/*
 * swap_assign.h - the whole bucket at once.
 *
 * The window pass permutes five parts along a rail; this one permutes the whole
 * bucket by solving the assignment exactly. It costs a matrix and a Hungarian
 * solve, and it is the same kind of move: every pose in the bucket is already
 * occupied by a twin, so whatever the bijection is, the set of rectangles on
 * the board does not change. The DRC cannot tell that anything happened.
 *
 * The matrix is the star model - the distance from each pin to the centroid of
 * the *other* pins on its net - because the true HPWL is a sum of bounding
 * boxes and is not separable over parts. That is an approximation, which is why
 * the result is scored with the engine's own cost before it is kept, and why
 * the window pass still runs afterwards on the true wirelength.
 */
#ifndef PLACE_SWAP_ASSIGN_H
#define PLACE_SWAP_ASSIGN_H

#include "solver_internal.h"

/* Solves and applies the best assignment of every bucket with at least
 * `min_bucket` members (0 disables). Returns whether anything was kept. */
bool swap_assign_pass(solver_t *s, uint32_t min_bucket);

#endif /* PLACE_SWAP_ASSIGN_H */
