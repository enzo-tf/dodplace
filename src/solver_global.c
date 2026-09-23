/*
 * Stage 2 - global placement by force-directed relaxation.
 *
 * Two forces, both read straight off the flat arrays:
 *
 *   attraction   the classic star model: every pin is pulled toward the
 *                centroid of its net, weighted by the net's weight. No
 *                Steiner point is materialised, the centroid is recomputed
 *                from the pins every iteration.
 *
 *   repulsion    volumetric, and that is the part that makes PCB placement
 *                different from chip placement: parts differ in size by two
 *                orders of magnitude, so the repulsion uses an equivalent
 *                occupancy radius r = sqrt(halfW^2 + halfH^2) and falls off as
 *                1/d^2. Two large ICs push hard from far away; a 0402 can slide
 *                right up against its neighbour.
 *
 * Mechanical parts (connectors, mounting holes) are locked: they are the
 * Dirichlet boundary conditions the whole layout settles against. Clusters move
 * as rigid bodies, so a converter's inductor and output capacitor travel with
 * its IC.
 *
 * The repulsion here is the pairwise model; `--global-model analytic` swaps it
 * for the ePlace density field (solver_analytic.c), which is the same idea
 * without the O(n^2) sum and with a global view of where the room is. Both
 * share the attraction, the centring, the rigid bodies and the confinement.
 */
#include "solver_internal.h"
#include "global_forces.h"
#include "solver_analytic.h"

#include <math.h>

void solver_global(solver_t *s)
{
    const uint32_t ncomp = s->ncomp;
    const uint32_t iters = s->opt->global_iterations;
    if (iters == 0u || ncomp == 0u) {
        return;
    }

    if (s->opt->global_model == GLOBAL_MODEL_ANALYTIC) {
        (void)solver_global_analytic(s);
        return;
    }

    coord_t *fx = SOLVER_ALLOC(s, coord_t, ncomp);
    coord_t *fy = SOLVER_ALLOC(s, coord_t, ncomp);
    uint8_t *touched = SOLVER_ALLOC(s, uint8_t, ncomp);
    if (!s->ok) {
        return;
    }

    /* Repulsion scale: derived from the mean occupancy radius so the two
     * forces balance around a part's own size, whatever the board scale is. */
    coord_t mean_r = 0.0f;
    for (uint32_t i = 0u; i < ncomp; ++i) {
        mean_r += sqrtf(s->half_w[i] * s->half_w[i] + s->half_h[i] * s->half_h[i]);
    }
    mean_r = (ncomp > 0u) ? mean_r / (coord_t)ncomp : 1.0f;
    const coord_t gamma = (s->opt->repulsion > 0.0f)
                              ? s->opt->repulsion
                              : s->opt->attraction * mean_r * 4.0f;

    const coord_t board_w = s->board_max_x - s->board_min_x;
    const coord_t board_h = s->board_max_y - s->board_min_y;
    const coord_t max_move = 0.02f * sqrtf(board_w * board_w + board_h * board_h);

    for (uint32_t iter = 0u; iter < iters; ++iter) {
        global_reset_forces(s, fx, fy, touched);
        global_attraction(s, fx, fy, touched);
        global_centring(s, fx, fy, touched);

        /* --- repulsion: every pair, with the occupancy radius ----------- */
        for (uint32_t i = 0u; i < ncomp; ++i) {
            const coord_t ri = s->half_w[i] * s->half_w[i] + s->half_h[i] * s->half_h[i];
            for (uint32_t j = i + 1u; j < ncomp; ++j) {
                coord_t dx = s->x[i] - s->x[j];
                coord_t dy = s->y[i] - s->y[j];
                coord_t d2 = dx * dx + dy * dy;
                if (d2 < 1e-6f) {
                    /* exactly coincident: break the tie deterministically */
                    dx = 0.01f * (coord_t)((i % 7u) + 1u);
                    dy = 0.01f * (coord_t)((j % 5u) + 1u);
                    d2 = dx * dx + dy * dy;
                }
                const coord_t rj = s->half_w[j] * s->half_w[j] + s->half_h[j] * s->half_h[j];
                /* (r_i + r_j)^2 with r = sqrt(halfW^2 + halfH^2) */
                const coord_t rr = ri + rj + 2.0f * sqrtf(ri * rj);
                const coord_t mag = gamma * rr / d2;
                const coord_t inv_d = 1.0f / sqrtf(d2);
                const coord_t ux = dx * inv_d;
                const coord_t uy = dy * inv_d;
                fx[i] += mag * ux;
                fy[i] += mag * uy;
                fx[j] -= mag * ux;
                fy[j] -= mag * uy;
            }
        }

        global_cluster_forces(s, fx, fy);

        /* --- integrate, scaled so the largest force moves max_move ------ */
        const coord_t max_f = global_max_force(s, fx, fy);
        if (max_f <= 0.0f) {
            break;
        }
        const coord_t cooling = 1.0f - (coord_t)iter / (coord_t)iters;
        const coord_t scale = (max_move * (0.25f + 0.75f * cooling)) / max_f;

        for (uint32_t i = 0u; i < ncomp; ++i) {
            if (!solver_component_is_movable(s, i)) {
                continue;
            }
            const coord_t nx = s->x[i] + fx[i] * scale;
            const coord_t ny = s->y[i] + fy[i] * scale;
            const uint32_t cl = s->cluster_of[i];
            if (cl != SOLVER_NO_INDEX) {
                solver_apply_cluster(s, cl, nx, ny);
            } else {
                s->x[i] = nx;
                s->y[i] = ny;
            }
        }

        global_confine(s);
    }
}
