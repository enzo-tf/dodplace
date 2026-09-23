/*
 * solver_analytic.c - see solver_analytic.h.
 */
#include "solver_analytic.h"
#include "density_grid.h"
#include "global_forces.h"

#include <math.h>
#include <string.h>

/* The look-ahead point: y = x + beta * (x - x_previous). */
static void analytic_look_ahead(solver_t *s, const coord_t *mx, const coord_t *my,
                                coord_t beta)
{
    for (uint32_t i = 0u; i < s->ncomp; ++i) {
        if (!solver_component_is_movable(s, i)) {
            continue;
        }
        s->x[i] += beta * mx[i];
        s->y[i] += beta * my[i];
    }
    solver_sync_cluster_members(s);
}

/*
 * The electric force on each part: its area times the field, scaled so that the
 * mean density force matches the mean net pull (times the caller's weight).
 * Both terms end up with the same units, which is what makes the weighting mean
 * something across boards of different sizes and densities.
 */
static void analytic_density_forces(solver_t *s, const density_grid_t *grid, coord_t *fx,
                                    coord_t *fy, coord_t *dx, coord_t *dy, coord_t weight)
{
    coord_t sum_att = 0.0f;
    coord_t sum_den = 0.0f;
    uint32_t count = 0u;
    for (uint32_t i = 0u; i < s->ncomp; ++i) {
        dx[i] = 0.0f;
        dy[i] = 0.0f;
        if (!solver_component_is_movable(s, i)) {
            continue;
        }
        coord_t ex = 0.0f;
        coord_t ey = 0.0f;
        density_force_at(grid, s->x[i], s->y[i], &ex, &ey);
        const coord_t charge = s->half_w[i] * s->half_h[i];
        dx[i] = ex * charge;
        dy[i] = ey * charge;
        sum_den += sqrtf(dx[i] * dx[i] + dy[i] * dy[i]);
        sum_att += sqrtf(fx[i] * fx[i] + fy[i] * fy[i]);
        count += 1u;
    }
    if (count == 0u) {
        return;
    }
    const coord_t mean_att = sum_att / (coord_t)count;
    const coord_t mean_den = sum_den / (coord_t)count;
    const coord_t reference = (mean_att > 0.0f) ? mean_att : 1.0f;
    const coord_t scale =
        (mean_den > 0.0f) ? weight * reference / mean_den : 0.0f;
    for (uint32_t i = 0u; i < s->ncomp; ++i) {
        fx[i] += scale * dx[i];
        fy[i] += scale * dy[i];
    }
}

bool solver_global_analytic(solver_t *s)
{
    const uint32_t ncomp = s->ncomp;
    const uint32_t iters = s->opt->global_iterations;
    if (iters == 0u || ncomp == 0u) {
        return s->ok;
    }

    coord_t *fx = SOLVER_ALLOC(s, coord_t, ncomp);
    coord_t *fy = SOLVER_ALLOC(s, coord_t, ncomp);
    coord_t *mx = SOLVER_ALLOC(s, coord_t, ncomp);
    coord_t *my = SOLVER_ALLOC(s, coord_t, ncomp);
    coord_t *dx = SOLVER_ALLOC(s, coord_t, ncomp);
    coord_t *dy = SOLVER_ALLOC(s, coord_t, ncomp);
    uint8_t *touched = SOLVER_ALLOC(s, uint8_t, ncomp);
    density_grid_t grid;
    const uint32_t bins = (s->opt->density_bins >= 8u) ? s->opt->density_bins : 8u;
    if (fx == nullptr || fy == nullptr || mx == nullptr || my == nullptr || dx == nullptr ||
        dy == nullptr || touched == nullptr || !density_grid_init(s, &grid, bins, bins)) {
        return false;
    }
    memset(mx, 0, (size_t)ncomp * sizeof(coord_t));
    memset(my, 0, (size_t)ncomp * sizeof(coord_t));

    coord_t beta = s->opt->momentum;
    if (beta < 0.0f) {
        beta = 0.0f;
    }
    if (beta > 0.95f) {
        beta = 0.95f;
    }
    const coord_t board_w = s->board_max_x - s->board_min_x;
    const coord_t board_h = s->board_max_y - s->board_min_y;
    const coord_t max_move = 0.02f * sqrtf(board_w * board_w + board_h * board_h);

    for (uint32_t iter = 0u; iter < iters; ++iter) {
        analytic_look_ahead(s, mx, my, beta);
        global_reset_forces(s, fx, fy, touched);
        global_attraction(s, fx, fy, touched);
        global_centring(s, fx, fy, touched);
        density_rasterise(s, &grid);
        density_solve(&grid);
        analytic_density_forces(s, &grid, fx, fy, dx, dy, s->opt->w_density);
        global_cluster_forces(s, fx, fy);

        const coord_t max_f = global_max_force(s, fx, fy);
        if (max_f <= 0.0f) {
            break;
        }
        const coord_t cooling = 1.0f - (coord_t)iter / (coord_t)iters;
        const coord_t step = (max_move * (0.25f + 0.75f * cooling)) / max_f;
        for (uint32_t i = 0u; i < ncomp; ++i) {
            if (!solver_component_is_movable(s, i)) {
                continue;
            }
            const coord_t step_x = fx[i] * step;
            const coord_t step_y = fy[i] * step;
            mx[i] = beta * mx[i] + step_x;
            my[i] = beta * my[i] + step_y;
            s->x[i] += step_x;
            s->y[i] += step_y;
        }
        solver_sync_cluster_members(s);
        global_confine(s);
    }
    return s->ok;
}
