/*
 * swap_assign.c - see swap_assign.h.
 */
#include "swap_assign.h"
#include "assign_hungarian.h"
#include "solver_best.h"
#include "wa_wirelength.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    coord_t x;
    coord_t y;
    uint8_t orient;
} assign_slot_t;

/* One pin's contribution: how far it would sit from the rest of its net. */
static coord_t pin_cost(const solver_t *s, uint32_t pin, const assign_slot_t *slot,
                        const coord_t *net_sum_x, const coord_t *net_sum_y,
                        const uint32_t *net_count, const coord_t *pin_x, const coord_t *pin_y)
{
    const netlist_csr_t *nets = &s->ctx->nets;
    const uint32_t net = s->ctx->pins.net_id[pin];
    if (net == PLACE_ID_NONE || net >= nets->num_nets || net_count[net] < 2u) {
        return 0.0f;
    }
    const uint32_t others = net_count[net] - 1u;
    const coord_t cx = (net_sum_x[net] - pin_x[pin]) / (coord_t)others;
    const coord_t cy = (net_sum_y[net] - pin_y[pin]) / (coord_t)others;
    const uint32_t o = slot->orient;
    const coord_t px = slot->x + s->rot_dx[o * (size_t)s->npin + pin];
    const coord_t py = slot->y + s->rot_dy[o * (size_t)s->npin + pin];
    const coord_t dx = px - cx;
    const coord_t dy = py - cy;
    return sqrtf(dx * dx + dy * dy) * nets->weights[net];
}

/* The cost of putting part `member[i]` in slot `j`, summed over its pins. */
static coord_t assign_cost(const solver_t *s, uint32_t member, const assign_slot_t *slot,
                           const coord_t *net_sum_x, const coord_t *net_sum_y,
                           const uint32_t *net_count, const coord_t *pin_x,
                           const coord_t *pin_y)
{
    const uint32_t begin = s->ctx->comps.first_pin[member];
    const uint32_t end = begin + (uint32_t)s->ctx->comps.pin_count[member];
    coord_t total = 0.0f;
    for (uint32_t p = begin; p < end; ++p) {
        total += pin_cost(s, p, slot, net_sum_x, net_sum_y, net_count, pin_x, pin_y);
    }
    return total;
}

/* A pin's position if its component took `slot`. */
static void pin_at(const solver_t *s, const assign_slot_t *slot, uint32_t pin, coord_t *px,
                   coord_t *py)
{
    const uint32_t o = slot->orient;
    *px = slot->x + s->rot_dx[o * (size_t)s->npin + pin];
    *py = slot->y + s->rot_dy[o * (size_t)s->npin + pin];
}

/*
 * The weighted-average cost of putting `member` in `slot`: for each of its
 * pins, how much the soft length of that pin's net would change. The marginal
 * is computed with every other pin held where it is - the standard
 * one-at-a-time linearisation, which is what makes the cost a matrix at all -
 * and the bucket's veto then checks the real thing.
 */
static coord_t assign_cost_wa(const solver_t *s, uint32_t member, const assign_slot_t *slot,
                              const coord_t *wa_now)
{
    const netlist_csr_t *nets = &s->ctx->nets;
    const pins_soa_t *pins = &s->ctx->pins;
    const uint32_t begin = s->ctx->comps.first_pin[member];
    const uint32_t end = begin + (uint32_t)s->ctx->comps.pin_count[member];
    coord_t total = 0.0f;
    for (uint32_t p = begin; p < end; ++p) {
        const uint32_t net = pins->net_id[p];
        if (net == PLACE_ID_NONE || net >= nets->num_nets) {
            continue;
        }
        coord_t px = 0.0f;
        coord_t py = 0.0f;
        pin_at(s, slot, p, &px, &py);
        const coord_t moved =
            wa_net_length(s, net, p, px, py, s->opt->wa_gamma);
        total += nets->weights[net] * (moved - wa_now[net]);
    }
    return total;
}


static void put_slot(solver_t *s, uint32_t comp, const assign_slot_t *slot)
{
    s->x[comp] = slot->x;
    s->y[comp] = slot->y;
    s->orient[comp] = slot->orient;
    const components_soa_t *c = &s->ctx->comps;
    const bool odd = (slot->orient & 1u) != 0u;
    s->half_w[comp] = odd ? c->half_h[comp] : c->half_w[comp];
    s->half_h[comp] = odd ? c->half_w[comp] : c->half_h[comp];
}

bool swap_assign_pass(solver_t *s, uint32_t min_bucket)
{
    const uint32_t npin = s->npin;
    const uint32_t nnet = s->ctx->nets.num_nets;
    if (min_bucket < 2u || s->swap_bucket_count == 0u || npin == 0u) {
        return false;
    }
    /* Bound the buckets by the arena we can afford: the matrix is n^2. */
    uint32_t biggest = 0u;
    for (uint32_t b = 0u; b < s->swap_bucket_count; ++b) {
        biggest = (s->swap_len[b] > biggest) ? s->swap_len[b] : biggest;
    }
    if (biggest < min_bucket) {
        return false;
    }
    const uint32_t n = biggest;
    coord_t *pin_x = SOLVER_ALLOC(s, coord_t, npin);
    coord_t *pin_y = SOLVER_ALLOC(s, coord_t, npin);
    coord_t *net_sum_x = SOLVER_ALLOC(s, coord_t, nnet);
    coord_t *net_sum_y = SOLVER_ALLOC(s, coord_t, nnet);
    coord_t *wa_now = SOLVER_ALLOC(s, coord_t, nnet);
    uint32_t *net_count = SOLVER_ALLOC(s, uint32_t, nnet);
    coord_t *cost = SOLVER_ALLOC(s, coord_t, (size_t)n * n);
    uint32_t *assign = SOLVER_ALLOC(s, uint32_t, n);
    coord_t *u = SOLVER_ALLOC(s, coord_t, n + 1u);
    coord_t *v = SOLVER_ALLOC(s, coord_t, n + 1u);
    uint32_t *p = SOLVER_ALLOC(s, uint32_t, n + 1u);
    uint32_t *way = SOLVER_ALLOC(s, uint32_t, n + 1u);
    coord_t *minv = SOLVER_ALLOC(s, coord_t, n + 1u);
    uint8_t *used = SOLVER_ALLOC(s, uint8_t, n + 1u);
    assign_slot_t *slots = SOLVER_ALLOC(s, assign_slot_t, n);
    if (pin_x == nullptr || pin_y == nullptr || net_sum_x == nullptr || net_sum_y == nullptr ||
        wa_now == nullptr || net_count == nullptr || cost == nullptr || assign == nullptr ||
        u == nullptr ||
        v == nullptr || p == nullptr || way == nullptr || minv == nullptr || used == nullptr ||
        slots == nullptr) {
        return false;
    }
    solver_best_t keep;
    if (!solver_best_init(s, &keep)) {
        return false;
    }
    bool changed = false;
    uint32_t vetoed = 0u;
    for (uint32_t b = 0u; b < s->swap_bucket_count; ++b) {
        const uint32_t len = s->swap_len[b];
        if (len < min_bucket) {
            continue;
        }
        const uint32_t first = s->swap_first[b];
        /* The centroid of each net, as it stands: the star model's fixed point. */
        for (uint32_t net = 0u; net < nnet; ++net) {
            net_sum_x[net] = 0.0f;
            net_sum_y[net] = 0.0f;
            net_count[net] = 0u;
            wa_now[net] = wa_net_length(s, net, PLACE_ID_NONE, 0.0f, 0.0f, s->opt->wa_gamma);
        }
        for (uint32_t p_i = 0u; p_i < npin; ++p_i) {
            const uint32_t comp = s->ctx->pins.comp_id[p_i];
            const uint32_t o = s->orient[comp];
            pin_x[p_i] = s->x[comp] + s->rot_dx[o * (size_t)npin + p_i];
            pin_y[p_i] = s->y[comp] + s->rot_dy[o * (size_t)npin + p_i];
            const uint32_t net = s->ctx->pins.net_id[p_i];
            if (net < nnet) {
                net_sum_x[net] += pin_x[p_i];
                net_sum_y[net] += pin_y[p_i];
                net_count[net] += 1u;
            }
        }
        for (uint32_t i = 0u; i < len; ++i) {
            const uint32_t m = s->swap_member[first + i];
            slots[i].x = s->x[m];
            slots[i].y = s->y[m];
            slots[i].orient = s->orient[m];
        }
        for (uint32_t i = 0u; i < len; ++i) {
            const uint32_t m = s->swap_member[first + i];
            for (uint32_t j = 0u; j < len; ++j) {
                cost[i * len + j] =
                    (s->opt->assign_model == ASSIGN_MODEL_WA)
                        ? assign_cost_wa(s, m, &slots[j], wa_now)
                        : assign_cost(s, m, &slots[j], net_sum_x, net_sum_y, net_count,
                                      pin_x, pin_y);
            }
        }
        assign_hungarian(cost, len, assign, u, v, p, way, minv, used);
        bool moved = false;
        for (uint32_t i = 0u; i < len; ++i) {
            moved = moved || (assign[i] != i);
        }
        if (!moved) {
            continue;
        }
        solver_best_take(s, &keep);
        for (uint32_t i = 0u; i < len; ++i) {
            put_slot(s, s->swap_member[first + i], &slots[assign[i]]);
        }
        /* The star model is not the cost the engine is judged by: keep the
         * permutation only if the real objective agreed. */
        if (solver_best_adopt(s, &keep)) {
            vetoed += 1u;
        } else {
            changed = true;
        }
    }
    if (getenv("DODPLACE_ASSIGN_DEBUG") != nullptr) {
        (void)fprintf(stderr, "assign pass: %u bucket(s) rearranged, %u vetoed\n",
                      (unsigned)(changed ? 1u : 0u), (unsigned)vetoed);
    }
    return changed;
}
