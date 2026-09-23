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
 */
#include "solver_internal.h"

#include <math.h>

static coord_t clamp_coord(coord_t v, coord_t lo, coord_t hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

void solver_global(solver_t *s)
{
    const netlist_csr_t *nets = &s->ctx->nets;
    const pins_soa_t *pins = &s->ctx->pins;
    const components_soa_t *comps = &s->ctx->comps;
    const uint32_t npin = s->npin;
    const uint32_t ncomp = s->ncomp;
    const uint32_t iters = s->opt->global_iterations;
    if (iters == 0u || ncomp == 0u) {
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
    const coord_t centre_x = (s->board_min_x + s->board_max_x) * 0.5f;
    const coord_t centre_y = (s->board_min_y + s->board_max_y) * 0.5f;
    const coord_t max_move = 0.02f * sqrtf(board_w * board_w + board_h * board_h);

    for (uint32_t iter = 0u; iter < iters; ++iter) {
        for (uint32_t i = 0u; i < ncomp; ++i) {
            fx[i] = 0.0f;
            fy[i] = 0.0f;
            touched[i] = 0u;
        }

        /* --- attraction: pin -> net centroid ---------------------------- */
        for (uint32_t n = 0u; n < nets->num_nets; ++n) {
            const uint32_t begin = nets->net_offsets[n];
            const uint32_t end = nets->net_offsets[n + 1u];
            if (end - begin < 2u) {
                continue;
            }
            coord_t cx = 0.0f;
            coord_t cy = 0.0f;
            for (uint32_t e = begin; e < end; ++e) {
                const uint32_t pin = nets->net_to_pins[e];
                const uint32_t comp = pins->comp_id[pin];
                const uint32_t o = s->orient[comp];
                cx += s->x[comp] + s->rot_dx[o * npin + pin];
                cy += s->y[comp] + s->rot_dy[o * npin + pin];
            }
            const coord_t inv = 1.0f / (coord_t)(end - begin);
            cx *= inv;
            cy *= inv;

            const coord_t k = s->opt->attraction * s->opt->net_weight_scale * nets->weights[n];
            for (uint32_t e = begin; e < end; ++e) {
                const uint32_t pin = nets->net_to_pins[e];
                const uint32_t comp = pins->comp_id[pin];
                const uint32_t o = s->orient[comp];
                const coord_t px = s->x[comp] + s->rot_dx[o * npin + pin];
                const coord_t py = s->y[comp] + s->rot_dy[o * npin + pin];
                fx[comp] += k * (cx - px);
                fy[comp] += k * (cy - py);
                touched[comp] = 1u;
            }
        }

        /* --- weak centring for parts the netlist does not reach --------- */
        for (uint32_t i = 0u; i < ncomp; ++i) {
            if (touched[i] == 0u && (comps->flags[i] & COMP_LOCKED) == 0u) {
                fx[i] += 0.02f * (centre_x - s->x[i]);
                fy[i] += 0.02f * (centre_y - s->y[i]);
            }
        }

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

        /* --- clusters take the sum of their members' forces -------------- */
        for (uint32_t cl = 0u; cl < s->nclusters; ++cl) {
            const uint32_t master = s->cluster_master[cl];
            for (uint32_t k = s->cluster_first[cl];
                 k < s->cluster_first[cl] + s->cluster_count[cl]; ++k) {
                const uint32_t slave = s->member_comp[k];
                fx[master] += fx[slave];
                fy[master] += fy[slave];
            }
        }

        /* --- integrate, scaled so the largest force moves max_move ------ */
        coord_t max_f = 0.0f;
        for (uint32_t i = 0u; i < ncomp; ++i) {
            if (!solver_component_is_movable(s, i)) {
                continue;
            }
            const coord_t f = sqrtf(fx[i] * fx[i] + fy[i] * fy[i]);
            max_f = (f > max_f) ? f : max_f;
        }
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

        /* --- confinement -------------------------------------------------- */
        for (uint32_t i = 0u; i < ncomp; ++i) {
            if (!solver_component_is_movable(s, i)) {
                continue;
            }
            const uint32_t cl = s->cluster_of[i];
            const uint32_t target = (cl == SOLVER_NO_INDEX) ? i : s->cluster_master[cl];
            const coord_t px = clamp_coord(s->x[target], s->board_min_x + s->half_w[target],
                                           s->board_max_x - s->half_w[target]);
            const coord_t py = clamp_coord(s->y[target], s->board_min_y + s->half_h[target],
                                           s->board_max_y - s->half_h[target]);
            if (cl != SOLVER_NO_INDEX) {
                solver_apply_cluster(s, cl, px, py);
            } else {
                s->x[target] = px;
                s->y[target] = py;
            }
        }
    }
}
