/*
 * legalize_keepout.c - pushing parts out of the keepout areas.
 *
 * Keepouts are polygons in the scene; their bounding boxes are what the push
 * uses, which is conservative (a part may be pushed a little further than the
 * outline strictly demands) and never leaves a part inside one.
 */
#include "solver_internal.h"
#include "legalize_keepout.h"

#include <math.h>

/* Pushes everything out of the keepout areas, using their bounding boxes. */
void push_out_of_keepouts(solver_t *s)
{
    const polygon_pool_t *pool = &s->ctx->constraints.polygons;
    const coord_t gap = s->opt->courtyard_clearance;
    for (uint32_t p = 0u; p < pool->num_polys; ++p) {
        if (pool->poly_kind[p] != POLY_KIND_KEEPOUT) {
            continue;
        }
        const uint32_t begin = pool->poly_offsets[p];
        const uint32_t end = pool->poly_offsets[p + 1u];
        if (end - begin < 3u) {
            continue;
        }
        coord_t lo_x = pool->vx[begin];
        coord_t hi_x = pool->vx[begin];
        coord_t lo_y = pool->vy[begin];
        coord_t hi_y = pool->vy[begin];
        for (uint32_t v = begin + 1u; v < end; ++v) {
            lo_x = (pool->vx[v] < lo_x) ? pool->vx[v] : lo_x;
            hi_x = (pool->vx[v] > hi_x) ? pool->vx[v] : hi_x;
            lo_y = (pool->vy[v] < lo_y) ? pool->vy[v] : lo_y;
            hi_y = (pool->vy[v] > hi_y) ? pool->vy[v] : hi_y;
        }
        for (uint32_t i = 0u; i < s->ncomp; ++i) {
            if (!solver_component_is_movable(s, i)) {
                continue;
            }
            const coord_t dx = s->x[i] - (lo_x + hi_x) * 0.5f;
            const coord_t dy = s->y[i] - (lo_y + hi_y) * 0.5f;
            const coord_t pen_x = (hi_x - lo_x) * 0.5f + s->half_w[i] + gap - fabsf(dx);
            const coord_t pen_y = (hi_y - lo_y) * 0.5f + s->half_h[i] + gap - fabsf(dy);
            if (pen_x <= 0.0f || pen_y <= 0.0f) {
                continue; /* outside this keepout */
            }
            if (pen_x < pen_y) {
                const coord_t dir = (dx >= 0.0f) ? 1.0f : -1.0f;
                solver_nudge(s, i, dir * (pen_x + 0.001f), 0.0f);
            } else {
                const coord_t dir = (dy >= 0.0f) ? 1.0f : -1.0f;
                solver_nudge(s, i, 0.0f, dir * (pen_y + 0.001f));
            }
            solver_clamp_to_board(s, i);
        }
    }
}

