/*
 * ratsnest.h - the connectivity tree the crossing count reads.
 *
 * ratsnest_build fills the solver's segment tables (one MST per net, see
 * ratsnest.c); count_crossings counts the pairs of segments of different nets
 * that intersect. Both are rebuilt from scratch on every evaluation, so the
 * tables are only valid until the placement moves.
 */
#ifndef PLACE_RATSNEST_H
#define PLACE_RATSNEST_H

#include "solver_internal.h"

void     ratsnest_build(solver_t *s);
uint32_t count_crossings(solver_t *s);

#endif /* PLACE_RATSNEST_H */
