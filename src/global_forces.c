/*
 * global_forces.c - see global_forces.h.
 */
#include "global_forces.h"

#include <math.h>

void global_reset_forces(solver_t *s, coord_t *fx, coord_t *fy, uint8_t *touched)
{
    for (uint32_t i = 0u; i < s->ncomp; ++i) {
        fx[i] = 0.0f;
        fy[i] = 0.0f;
        touched[i] = 0u;
    }
}

void global_attraction(solver_t *s, coord_t *fx, coord_t *fy, uint8_t *touched)
{
    const netlist_csr_t *nets = &s->ctx->nets;
    const pins_soa_t *pins = &s->ctx->pins;
    const uint32_t npin = s->npin;
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
}

void global_centring(solver_t *s, coord_t *fx, coord_t *fy, const uint8_t *touched)
{
    const coord_t centre_x = (s->board_min_x + s->board_max_x) * 0.5f;
    const coord_t centre_y = (s->board_min_y + s->board_max_y) * 0.5f;
    for (uint32_t i = 0u; i < s->ncomp; ++i) {
        if (touched[i] == 0u && (s->ctx->comps.flags[i] & COMP_LOCKED) == 0u) {
            fx[i] += 0.02f * (centre_x - s->x[i]);
            fy[i] += 0.02f * (centre_y - s->y[i]);
        }
    }
}

void global_cluster_forces(solver_t *s, coord_t *fx, coord_t *fy)
{
    for (uint32_t cl = 0u; cl < s->nclusters; ++cl) {
        const uint32_t master = s->cluster_master[cl];
        for (uint32_t k = s->cluster_first[cl];
             k < s->cluster_first[cl] + s->cluster_count[cl]; ++k) {
            const uint32_t slave = s->member_comp[k];
            fx[master] += fx[slave];
            fy[master] += fy[slave];
        }
    }
}

coord_t global_max_force(solver_t *s, const coord_t *fx, const coord_t *fy)
{
    coord_t max_f = 0.0f;
    for (uint32_t i = 0u; i < s->ncomp; ++i) {
        if (!solver_component_is_movable(s, i)) {
            continue;
        }
        const coord_t f = sqrtf(fx[i] * fx[i] + fy[i] * fy[i]);
        max_f = (f > max_f) ? f : max_f;
    }
    return max_f;
}

void global_confine(solver_t *s)
{
    for (uint32_t i = 0u; i < s->ncomp; ++i) {
        if (!solver_component_is_movable(s, i)) {
            continue;
        }
        const uint32_t cl = s->cluster_of[i];
        const uint32_t target = (cl == SOLVER_NO_INDEX) ? i : s->cluster_master[cl];
        coord_t px = s->x[target];
        coord_t py = s->y[target];
        const coord_t lo_x = s->board_min_x + s->half_w[target];
        const coord_t hi_x = s->board_max_x - s->half_w[target];
        const coord_t lo_y = s->board_min_y + s->half_h[target];
        const coord_t hi_y = s->board_max_y - s->half_h[target];
        px = (px < lo_x) ? lo_x : px;
        px = (px > hi_x) ? hi_x : px;
        py = (py < lo_y) ? lo_y : py;
        py = (py > hi_y) ? hi_y : py;
        if (cl != SOLVER_NO_INDEX) {
            solver_apply_cluster(s, cl, px, py);
        } else {
            s->x[target] = px;
            s->y[target] = py;
        }
    }
}
