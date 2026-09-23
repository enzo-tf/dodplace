/*
 * density_grid.c - see density_grid.h.
 */
#include "density_grid.h"

#include <math.h>

/* The board's bounding box, padded by half a bin so every part is inside. */
static void grid_frame(const solver_t *s, density_grid_t *g, uint32_t nx, uint32_t ny)
{
    coord_t span_x = s->board_max_x - s->board_min_x;
    coord_t span_y = s->board_max_y - s->board_min_y;
    if (!(span_x > 0.0f)) {
        span_x = 1.0f;
    }
    if (!(span_y > 0.0f)) {
        span_y = 1.0f;
    }
    g->nx = nx;
    g->ny = ny;
    g->cell_x = span_x / (coord_t)nx;
    g->cell_y = span_y / (coord_t)ny;
    g->origin_x = s->board_min_x;
    g->origin_y = s->board_min_y;
}

bool density_grid_init(solver_t *s, density_grid_t *g, uint32_t nx, uint32_t ny)
{
    const size_t bins = (size_t)nx * (size_t)ny;
    const size_t line = (nx > ny) ? nx : ny;
    g->rho = SOLVER_ALLOC(s, coord_t, bins);
    g->phi = SOLVER_ALLOC(s, coord_t, bins);
    g->ex = SOLVER_ALLOC(s, coord_t, bins);
    g->ey = SOLVER_ALLOC(s, coord_t, bins);
    g->line = SOLVER_ALLOC(s, coord_t, 2u * line);
    if (g->rho == nullptr || g->phi == nullptr || g->ex == nullptr || g->ey == nullptr ||
        g->line == nullptr) {
        return false;
    }
    if (!dct_axis_init(&g->axis_x, s, nx) || !dct_axis_init(&g->axis_y, s, ny)) {
        return false;
    }
    grid_frame(s, g, nx, ny);
    for (size_t i = 0u; i < bins; ++i) {
        g->rho[i] = 0.0f;
        g->phi[i] = 0.0f;
        g->ex[i] = 0.0f;
        g->ey[i] = 0.0f;
    }
    return true;
}

/* One bin index along an axis, clamped: a part that hangs over the edge of the
 * board charges the bin it hangs over rather than vanishing from the model. */
static uint32_t bin_index(coord_t value, coord_t origin, coord_t cell, uint32_t n)
{
    const coord_t f = (value - origin) / cell;
    if (f <= 0.0f) {
        return 0u;
    }
    const coord_t last = (coord_t)(n - 1u);
    if (f >= last) {
        return n - 1u;
    }
    return (uint32_t)f;
}

void density_rasterise(const solver_t *s, density_grid_t *g)
{
    const size_t bins = (size_t)g->nx * (size_t)g->ny;
    for (size_t i = 0u; i < bins; ++i) {
        g->rho[i] = 0.0f;
    }
    const coord_t bin_area = g->cell_x * g->cell_y;
    for (uint32_t i = 0u; i < s->ncomp; ++i) {
        const coord_t lo_x = s->x[i] - s->half_w[i];
        const coord_t hi_x = s->x[i] + s->half_w[i];
        const coord_t lo_y = s->y[i] - s->half_h[i];
        const coord_t hi_y = s->y[i] + s->half_h[i];
        const uint32_t x0 = bin_index(lo_x, g->origin_x, g->cell_x, g->nx);
        const uint32_t x1 = bin_index(hi_x, g->origin_x, g->cell_x, g->nx);
        const uint32_t y0 = bin_index(lo_y, g->origin_y, g->cell_y, g->ny);
        const uint32_t y1 = bin_index(hi_y, g->origin_y, g->cell_y, g->ny);
        for (uint32_t by = y0; by <= y1; ++by) {
            const coord_t b_lo = g->origin_y + (coord_t)by * g->cell_y;
            const coord_t b_hi = b_lo + g->cell_y;
            const coord_t oy = fminf(hi_y, b_hi) - fmaxf(lo_y, b_lo);
            if (oy <= 0.0f) {
                continue;
            }
            for (uint32_t bx = x0; bx <= x1; ++bx) {
                const coord_t a_lo = g->origin_x + (coord_t)bx * g->cell_x;
                const coord_t a_hi = a_lo + g->cell_x;
                const coord_t ox = fminf(hi_x, a_hi) - fmaxf(lo_x, a_lo);
                if (ox <= 0.0f) {
                    continue;
                }
                g->rho[(size_t)by * (size_t)g->nx + (size_t)bx] += (ox * oy) / bin_area;
            }
        }
    }
}

/* Rows then columns: the 2-D transform is separable because the operator is. */
static void transform_rows(density_grid_t *g, coord_t *grid, bool forward)
{
    for (uint32_t y = 0u; y < g->ny; ++y) {
        coord_t *row = &grid[(size_t)y * (size_t)g->nx];
        if (forward) {
            dct_axis_forward(&g->axis_x, row, g->line);
        } else {
            dct_axis_inverse(&g->axis_x, row, g->line);
        }
        for (uint32_t x = 0u; x < g->nx; ++x) {
            row[x] = g->line[x];
        }
    }
}

static void transform_columns(density_grid_t *g, coord_t *grid, bool forward)
{
    coord_t *in = g->line;
    coord_t *out = g->line + g->ny;
    for (uint32_t x = 0u; x < g->nx; ++x) {
        for (uint32_t y = 0u; y < g->ny; ++y) {
            in[y] = grid[(size_t)y * (size_t)g->nx + (size_t)x];
        }
        if (forward) {
            dct_axis_forward(&g->axis_y, in, out);
        } else {
            dct_axis_inverse(&g->axis_y, in, out);
        }
        for (uint32_t y = 0u; y < g->ny; ++y) {
            grid[(size_t)y * (size_t)g->nx + (size_t)x] = out[y];
        }
    }
}

void density_transform_2d(density_grid_t *g, coord_t *grid, bool forward)
{
    transform_rows(g, grid, forward);
    transform_columns(g, grid, forward);
}
