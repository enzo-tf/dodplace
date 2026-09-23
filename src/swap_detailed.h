/*
 * swap_detailed.h - the annealer's discrete follow-up, in one place.
 */
#ifndef PLACE_SWAP_DETAILED_H
#define PLACE_SWAP_DETAILED_H

#include "solver_internal.h"

/* Global assignment of the big buckets, local windows on what is left, and the
 * better of the two chains kept. In place, deterministic, no allocation that
 * outlives the call. */
void swap_detailed_stage(solver_t *s);

#endif /* PLACE_SWAP_DETAILED_H */
