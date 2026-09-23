/*
 * assign_hungarian.h - the optimal bijection, when a permutation is the move.
 *
 * A bucket of interchangeable parts and the poses they occupy form a square
 * assignment problem: every part must take exactly one pose, every pose exactly
 * one part, and the cost of a pairing is known. Greedy and pairwise searches
 * stall on a three-cycle whose every single transposition is worse than the
 * arrangement it starts from - which is precisely the case a placer meets on a
 * rail of identical parts - and the Hungarian method solves the whole thing
 * exactly in O(n^3), which for the 48-part bucket on r10 is 110 000 operations.
 */
#ifndef PLACE_ASSIGN_HUNGARIAN_H
#define PLACE_ASSIGN_HUNGARIAN_H

#include "solver_internal.h"

/*
 * `cost` is row-major n*n: the cost of pairing row i with column j. `assign`
 * receives the column chosen for each row. All working arrays come from the
 * caller, so the solver never allocates: `u`, `v`, `p`, `way` are n+1 long and
 * `minv` is n+1 floats; `used` is n+1 flags.
 */
void assign_hungarian(const coord_t *cost, uint32_t n, uint32_t *assign, coord_t *u,
                      coord_t *v, uint32_t *p, uint32_t *way, coord_t *minv, uint8_t *used);

#endif /* PLACE_ASSIGN_HUNGARIAN_H */
