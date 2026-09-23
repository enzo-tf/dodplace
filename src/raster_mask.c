/*
 * raster_mask.c - see raster_mask.h for the indexing contract.
 */
#include "raster_mask.h"

#include <math.h>
#include <string.h>

bool raster_mask_init(raster_mask_t *m, const solver_t *s, coord_t cell)
{
    if (m == nullptr || s == nullptr || cell <= 0.0f) {
        return false;
    }
    m->cell = cell;
    m->ceiling = s->ctx->stackup.ceiling_height; /* 0 = no enclosure declared */
    m->origin_x = s->edge_min_x - cell;
    m->origin_y = s->edge_min_y - cell;
    const coord_t span_x = s->edge_max_x - m->origin_x;
    const coord_t span_y = s->edge_max_y - m->origin_y;
    m->cols = (uint32_t)(span_x / cell) + 2u;
    m->rows = (uint32_t)(span_y / cell) + 2u;
    m->words = ((size_t)m->cols * (size_t)m->rows + 63u) / 64u;
    m->bits = SOLVER_ALLOC((solver_t *)s, uint64_t, m->words);
    if (m->bits == nullptr) {
        return false;
    }
    memset(m->bits, 0, m->words * sizeof(uint64_t));
    return true;
}

/* The first and last cell whose centre lies inside [lo, hi] on one axis. */
static bool cell_span(const raster_mask_t *m, coord_t lo, coord_t hi, coord_t origin,
                      int32_t *first, int32_t *last)
{
    const coord_t f0 = (lo - origin) / m->cell - 0.5f;
    const coord_t f1 = (hi - origin) / m->cell - 0.5f;
    *first = (int32_t)ceilf(f0);
    *last = (int32_t)floorf(f1);
    return *first <= *last;
}

void raster_mask_stamp_box(raster_mask_t *m, coord_t lo_x, coord_t lo_y, coord_t hi_x,
                           coord_t hi_y)
{
    int32_t cx0 = 0;
    int32_t cx1 = 0;
    int32_t cy0 = 0;
    int32_t cy1 = 0;
    if (!cell_span(m, lo_x, hi_x, m->origin_x, &cx0, &cx1) ||
        !cell_span(m, lo_y, hi_y, m->origin_y, &cy0, &cy1)) {
        return;
    }
    if (cx0 < 0) {
        cx0 = 0;
    }
    if (cy0 < 0) {
        cy0 = 0;
    }
    if (cx1 >= (int32_t)m->cols) {
        cx1 = (int32_t)m->cols - 1;
    }
    if (cy1 >= (int32_t)m->rows) {
        cy1 = (int32_t)m->rows - 1;
    }
    for (int32_t cy = cy0; cy <= cy1; ++cy) {
        for (int32_t cx = cx0; cx <= cx1; ++cx) {
            const size_t bit = (size_t)cy * m->cols + (size_t)cx;
            m->bits[bit >> 6] |= 1ull << (bit & 63u);
        }
    }
}

bool raster_mask_box_clear(const raster_mask_t *m, coord_t height, coord_t lo_x, coord_t lo_y,
                           coord_t hi_x, coord_t hi_y)
{
    if (m->ceiling > 0.0f && height > m->ceiling) {
        return false; /* taller than the enclosure: no slot on the board fits */
    }
    int32_t cx0 = 0;
    int32_t cx1 = 0;
    int32_t cy0 = 0;
    int32_t cy1 = 0;
    if (!cell_span(m, lo_x, hi_x, m->origin_x, &cx0, &cx1) ||
        !cell_span(m, lo_y, hi_y, m->origin_y, &cy0, &cy1)) {
        return false; /* thinner than a cell: the exact test decides */
    }
    if (cx0 < 0 || cy0 < 0 || cx1 >= (int32_t)m->cols || cy1 >= (int32_t)m->rows) {
        return false; /* outside the modelled area: never a proof */
    }
    for (int32_t cy = cy0; cy <= cy1; ++cy) {
        for (int32_t cx = cx0; cx <= cx1; ++cx) {
            const size_t bit = (size_t)cy * m->cols + (size_t)cx;
            if ((m->bits[bit >> 6] & (1ull << (bit & 63u))) != 0ull) {
                return false;
            }
        }
    }
    return true;
}

void raster_mask_build_obstacles(raster_mask_t *m, const solver_t *s, coord_t gap)
{
    if (m == nullptr || s == nullptr || m->bits == nullptr) {
        return;
    }
    memset(m->bits, 0, m->words * sizeof(uint64_t));

    /* Everything off the board is blocked, so a candidate near the edge is
     * rejected by the mask and not only by the caller's bounds check. */
    raster_mask_stamp_box(m, m->origin_x - m->cell, m->origin_y - m->cell, s->edge_min_x,
                          s->edge_max_y + m->cell);
    raster_mask_stamp_box(m, s->edge_max_x, m->origin_y - m->cell, m->origin_x + (coord_t)m->cols * m->cell,
                          s->edge_max_y + m->cell);
    raster_mask_stamp_box(m, m->origin_x - m->cell, m->origin_y - m->cell,
                          s->edge_max_x + m->cell, s->edge_min_y);
    raster_mask_stamp_box(m, m->origin_x - m->cell, s->edge_max_y, s->edge_max_x + m->cell,
                          m->origin_y + (coord_t)m->rows * m->cell);

    const coord_t half = gap * 0.5f;
    for (uint32_t i = 0u; i < s->ncomp; ++i) {
        /* Only the immovable parts are obstacles: a movable one will move. */
        if (solver_component_is_movable(s, i)) {
            continue;
        }
        coord_t lo_x = 0.0f;
        coord_t lo_y = 0.0f;
        coord_t hi_x = 0.0f;
        coord_t hi_y = 0.0f;
        solver_union_extent(s, i, &lo_x, &lo_y, &hi_x, &hi_y);
        raster_mask_stamp_box(m, s->x[i] + lo_x - half, s->y[i] + lo_y - half,
                              s->x[i] + hi_x + half, s->y[i] + hi_y + half);
    }

    const polygon_pool_t *pool = &s->ctx->constraints.polygons;
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
        raster_mask_stamp_box(m, lo_x - half, lo_y - half, hi_x + half, hi_y + half);
    }
}
