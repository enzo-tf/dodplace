/*
 * solver_state.h - building the solver's working tables from a scene.
 *
 * solver_state_init allocates everything from the scratch arena in one go and
 * returns false, leaving `ok` set, if the arena is too small; the caller
 * reports the need and unwinds. Nothing here touches the scene.
 */
#ifndef PLACE_SOLVER_STATE_H
#define PLACE_SOLVER_STATE_H

#include "solver_internal.h"

/* The median pitch of one axis, used to size the anchor box. */
coord_t solver_axis_pitch(coord_t *scratch, const coord_t *values, uint32_t count);

bool solver_state_init(solver_t *s, const placer_context_t *ctx, const solver_options_t *opt);

#endif /* PLACE_SOLVER_STATE_H */
