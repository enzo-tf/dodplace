/*
 * solver_bbox.c - the board rectangle the solver confines parts to.
 *
 * Two rectangles: the outline as drawn, and the same inset by the solver's
 * margin (a router needs room near the edge). The last-resort slot searches
 * work on the outline, because a part 0.4 mm from the edge is a compromise
 * while a part on top of a frozen neighbour is a broken board.
 */
#include "solver_internal.h"
#include "solver_bbox.h"

void compute_board_bbox(solver_t *s)
{
    const polygon_pool_t *pool = &s->ctx->constraints.polygons;
    const place_id_t outline = constraints_find_polygon(pool, POLY_KIND_BOARD_OUTLINE);
    if (outline != PLACE_ID_NONE) {
        const uint32_t begin = pool->poly_offsets[outline];
        const uint32_t end = pool->poly_offsets[outline + 1u];
        s->board_min_x = pool->vx[begin];
        s->board_max_x = pool->vx[begin];
        s->board_min_y = pool->vy[begin];
        s->board_max_y = pool->vy[begin];
        for (uint32_t v = begin + 1u; v < end; ++v) {
            s->board_min_x = (pool->vx[v] < s->board_min_x) ? pool->vx[v] : s->board_min_x;
            s->board_max_x = (pool->vx[v] > s->board_max_x) ? pool->vx[v] : s->board_max_x;
            s->board_min_y = (pool->vy[v] < s->board_min_y) ? pool->vy[v] : s->board_min_y;
            s->board_max_y = (pool->vy[v] > s->board_max_y) ? pool->vy[v] : s->board_max_y;
        }
    } else {
        s->board_min_x = s->ctx->comps.x[0] - s->ctx->comps.half_w[0];
        s->board_max_x = s->ctx->comps.x[0] + s->ctx->comps.half_w[0];
        s->board_min_y = s->ctx->comps.y[0] - s->ctx->comps.half_h[0];
        s->board_max_y = s->ctx->comps.y[0] + s->ctx->comps.half_h[0];
        for (uint32_t i = 1u; i < s->ncomp; ++i) {
            const coord_t lo_x = s->ctx->comps.x[i] - s->ctx->comps.half_w[i];
            const coord_t hi_x = s->ctx->comps.x[i] + s->ctx->comps.half_w[i];
            const coord_t lo_y = s->ctx->comps.y[i] - s->ctx->comps.half_h[i];
            const coord_t hi_y = s->ctx->comps.y[i] + s->ctx->comps.half_h[i];
            s->board_min_x = (lo_x < s->board_min_x) ? lo_x : s->board_min_x;
            s->board_max_x = (hi_x > s->board_max_x) ? hi_x : s->board_max_x;
            s->board_min_y = (lo_y < s->board_min_y) ? lo_y : s->board_min_y;
            s->board_max_y = (hi_y > s->board_max_y) ? hi_y : s->board_max_y;
        }
    }
    /* The outline as drawn, before the solver's own margin. */
    s->edge_min_x = s->board_min_x;
    s->edge_min_y = s->board_min_y;
    s->edge_max_x = s->board_max_x;
    s->edge_max_y = s->board_max_y;

    /* A margin keeps courtyards inside the edge, where a router needs room. */
    s->board_min_x += s->opt->board_margin;
    s->board_max_x -= s->opt->board_margin;
    s->board_min_y += s->opt->board_margin;
    s->board_max_y -= s->opt->board_margin;
}

