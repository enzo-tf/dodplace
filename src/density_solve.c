/*
 * density_solve.c - the Poisson solve and the field it produces.
 *
 *     div(grad phi) = -rho,  no-flux at the half-bin boundary
 *
 * The DCT-II basis diagonalises that operator, so the solve is a transform, a
 * division by the eigenvalue, and the inverse transform. The constant mode has
 * eigenvalue zero - a uniform density exerts no force - and is dropped, which
 * also fixes the arbitrary additive constant of the potential.
 */
#include "density_grid.h"

#include <math.h>

#ifndef DCT_PI
#define DCT_PI 3.14159265358979323846f
#endif

static coord_t axis_eigen(uint32_t u, uint32_t n, coord_t cell)
{
    const coord_t angle = DCT_PI * (coord_t)u / (coord_t)n;
    return (2.0f * cosf(angle) - 2.0f) / (cell * cell);
}

void density_solve(density_grid_t *g)
{
    const size_t bins = (size_t)g->nx * (size_t)g->ny;
    for (size_t i = 0u; i < bins; ++i) {
        g->phi[i] = g->rho[i];
    }
    density_transform_2d(g, g->phi, true);
    for (uint32_t v = 0u; v < g->ny; ++v) {
        const coord_t lv = axis_eigen(v, g->ny, g->cell_y);
        for (uint32_t u = 0u; u < g->nx; ++u) {
            const coord_t lambda = axis_eigen(u, g->nx, g->cell_x) + lv;
            coord_t value = 0.0f;
            if (lambda < 0.0f) {
                /* phi_hat = -rho_hat / lambda, and lambda < 0, so the sign flips:
                 * a dense bin becomes a potential *peak* and the field runs off
                 * it, downhill, towards the room. */
                value = -g->phi[(size_t)v * (size_t)g->nx + (size_t)u] / lambda;
            }
            g->phi[(size_t)v * (size_t)g->nx + (size_t)u] = value;
        }
    }
    density_transform_2d(g, g->phi, false);

    /* The force on a part is q * E with E = -grad phi; here only the field is
     * built, and the caller weighs it by the part's own area. */
    for (uint32_t v = 0u; v < g->ny; ++v) {
        for (uint32_t u = 0u; u < g->nx; ++u) {
            const uint32_t xm = (u == 0u) ? 0u : (u - 1u);
            const uint32_t xp = (u + 1u < g->nx) ? (u + 1u) : (g->nx - 1u);
            const uint32_t ym = (v == 0u) ? 0u : (v - 1u);
            const uint32_t yp = (v + 1u < g->ny) ? (v + 1u) : (g->ny - 1u);
            const size_t row = (size_t)v * (size_t)g->nx;
            const coord_t dx = (coord_t)(xp - xm) * g->cell_x;
            const coord_t dy = (coord_t)(yp - ym) * g->cell_y;
            const coord_t gx = (g->phi[row + xp] - g->phi[row + xm]) / dx;
            const coord_t gy =
                (g->phi[(size_t)yp * (size_t)g->nx + (size_t)u] -
                 g->phi[(size_t)ym * (size_t)g->nx + (size_t)u]) / dy;
            const size_t bin = row + u;
            g->ex[bin] = -gx;
            g->ey[bin] = -gy;
        }
    }
}

/* Fractional bin coordinate of a point: bins are centred on origin + (i+.5)h. */
static coord_t axis_coord(coord_t value, coord_t origin, coord_t cell, uint32_t n)
{
    const coord_t f = (value - origin) / cell - 0.5f;
    const coord_t last = (coord_t)(n - 1u);
    if (f <= 0.0f) {
        return 0.0f;
    }
    if (f >= last) {
        return last;
    }
    return f;
}

static coord_t sample(const density_grid_t *g, const coord_t *grid, coord_t fx, coord_t fy)
{
    const uint32_t x0 = (uint32_t)fx;
    const uint32_t y0 = (uint32_t)fy;
    const uint32_t x1 = (x0 + 1u < g->nx) ? (x0 + 1u) : x0;
    const uint32_t y1 = (y0 + 1u < g->ny) ? (y0 + 1u) : y0;
    const coord_t tx = fx - (coord_t)x0;
    const coord_t ty = fy - (coord_t)y0;
    const coord_t a = grid[(size_t)y0 * (size_t)g->nx + x0];
    const coord_t b = grid[(size_t)y0 * (size_t)g->nx + x1];
    const coord_t c = grid[(size_t)y1 * (size_t)g->nx + x0];
    const coord_t d = grid[(size_t)y1 * (size_t)g->nx + x1];
    const coord_t top = a + (b - a) * tx;
    const coord_t bot = c + (d - c) * tx;
    return top + (bot - top) * ty;
}

void density_force_at(const density_grid_t *g, coord_t x, coord_t y, coord_t *fx, coord_t *fy)
{
    const coord_t bx = axis_coord(x, g->origin_x, g->cell_x, g->nx);
    const coord_t by = axis_coord(y, g->origin_y, g->cell_y, g->ny);
    *fx = sample(g, g->ex, bx, by);
    *fy = sample(g, g->ey, bx, by);
}
