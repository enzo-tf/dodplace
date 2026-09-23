/*
 * solver_best.c - see solver_best.h.
 */
#include "solver_best.h"

#include <string.h>

/* A scene with no component never reaches the solver, but the arena refuses a
 * zero-byte block, so the tables are always at least one element long. */
static size_t slots(const solver_t *s)
{
    return (s->ncomp > 0u) ? (size_t)s->ncomp : 1u;
}

bool solver_best_init(solver_t *s, solver_best_t *best)
{
    best->x = SOLVER_ALLOC(s, coord_t, slots(s));
    best->y = SOLVER_ALLOC(s, coord_t, slots(s));
    best->orient = SOLVER_ALLOC(s, uint8_t, slots(s));
    best->have = false;
    memset(&best->cost, 0, sizeof best->cost);
    return s->ok && best->x != nullptr && best->y != nullptr && best->orient != nullptr;
}

void solver_best_take(solver_t *s, solver_best_t *best)
{
    memcpy(best->x, s->x, slots(s) * sizeof(coord_t));
    memcpy(best->y, s->y, slots(s) * sizeof(coord_t));
    memcpy(best->orient, s->orient, slots(s) * sizeof(uint8_t));
    best->cost = solver_evaluate(s);
    best->have = true;
}

bool solver_best_adopt(solver_t *s, const solver_best_t *best)
{
    if (!best->have) {
        return false;
    }
    const solver_cost_t current = solver_evaluate(s);
    if (current.score <= best->cost.score) {
        return false;
    }
    solver_best_restore(s, best);
    return true;
}

void solver_best_restore(solver_t *s, const solver_best_t *best)
{
    if (!best->have) {
        return;
    }
    memcpy(s->x, best->x, slots(s) * sizeof(coord_t));
    memcpy(s->y, best->y, slots(s) * sizeof(coord_t));
    memcpy(s->orient, best->orient, slots(s) * sizeof(uint8_t));
    solver_rebuild_extents(s);
}
