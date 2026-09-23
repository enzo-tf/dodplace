/*
 * matching_plan.c - which passives take part, and what each one aims at.
 */
#include "matching_internal.h"

#include <math.h>

/* Where the rule says the passive should end up: the IC pin, or the IC. */
static bool decoupling_target(const solver_t *s, uint32_t k, coord_t *out_x, coord_t *out_y)
{
    const constraint_decoupling_t *d = &s->ctx->constraints.decoupling;
    const place_id_t ic = d->ic_comp[k];
    if (ic == PLACE_ID_NONE) {
        return false;
    }
    const place_id_t pin = d->ic_pin[k];
    if (pin != PLACE_ID_NONE && s->ctx->pins.comp_id[pin] == ic) {
        const uint32_t npin = s->ctx->pins.count;
        *out_x = s->x[ic] + s->rot_dx[s->orient[ic] * npin + pin];
        *out_y = s->y[ic] + s->rot_dy[s->orient[ic] * npin + pin];
        return true;
    }
    *out_x = s->x[ic];
    *out_y = s->y[ic];
    return true;
}

uint32_t matching_build_rows(solver_t *s, const raster_mask_t *mask, match_row_t *rows,
                             uint32_t max_rows)
{
    const constraint_decoupling_t *d = &s->ctx->constraints.decoupling;
    uint32_t nrows = 0u;
    for (uint32_t k = 0u; k < d->count && nrows < max_rows; ++k) {
        const uint32_t comp = d->cap_comp[k];
        if (comp == PLACE_ID_NONE || !solver_component_is_movable(s, comp)) {
            continue;
        }
        coord_t tx = 0.0f;
        coord_t ty = 0.0f;
        if (!decoupling_target(s, k, &tx, &ty)) {
            continue;
        }
        coord_t reach = 5.0f;
        if (d->max_dist_sq[k] > 0.0f) {
            reach = sqrtf(d->max_dist_sq[k]);
        }
        if (matching_collect_slots(s, mask, comp, tx, ty, reach, &rows[nrows]) > 0u) {
            nrows += 1u;
        }
    }
    return nrows;
}
