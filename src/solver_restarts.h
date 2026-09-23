/*
 * solver_restarts.h - the parallel multi-start.
 *
 * A walk is independent of every other walk: same scene, same starting pose,
 * different seed. Nothing is shared but the read-only scene, so the restarts
 * are one job per core - and the winner is picked by score with the restart
 * index as the tie-break, which keeps the result identical however many cores
 * happen to run it.
 */
#ifndef PLACE_SOLVER_RESTARTS_H
#define PLACE_SOLVER_RESTARTS_H

#include "solver_internal.h"

/* Runs `restarts` walks, `jobs` at a time, from the pose `s` holds now, and
 * leaves the best one in `s`. False when a worker could not be started at all. */
bool solver_restarts_parallel(solver_t *s, uint32_t restarts, uint32_t jobs);

#endif /* PLACE_SOLVER_RESTARTS_H */
