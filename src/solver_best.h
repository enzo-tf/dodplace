/*
 * solver_best.h - the incumbent.
 *
 * A stage reports that it ran, not that it helped. The force-directed
 * relaxation redraws a board the designer had already placed, and the
 * annealing walk trades real clearance for modelled wirelength; measured on
 * r10 those two stages turn a 31.7 m input into 36.7 m, and every caller sees
 * a successful run. So the pipeline legalises the input exactly as it arrived,
 * keeps that pose as the incumbent, and adopts the transformed layout only when
 * it scores better on the same cost the stages themselves minimise.
 */
#ifndef PLACE_SOLVER_BEST_H
#define PLACE_SOLVER_BEST_H

#include "solver_internal.h"

typedef struct {
    coord_t      *x;
    coord_t      *y;
    uint8_t      *orient;
    solver_cost_t cost;
    bool          have;
} solver_best_t;

/* The pose buffers come from the scratch arena, like every other table. */
bool solver_best_init(solver_t *s, solver_best_t *best);

/* Record the current pose and its cost as the incumbent. */
void solver_best_take(solver_t *s, solver_best_t *best);

/* Adopt the incumbent's pose when it beats the current one. Returns whether
 * the pose was replaced. */
bool solver_best_adopt(solver_t *s, const solver_best_t *best);

/* Put the incumbent's pose back, whatever the current score says. */
void solver_best_restore(solver_t *s, const solver_best_t *best);

#endif /* PLACE_SOLVER_BEST_H */
