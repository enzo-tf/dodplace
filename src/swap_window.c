/*
 * swap_window.c - see swap_window.h.
 */
#include "swap_window.h"
#include "solver_best.h"
#include "swap.h"

#include <math.h>
#include <string.h>

typedef struct {
    coord_t x;
    coord_t y;
    uint8_t orient;
} slot_t;

/* The wirelength of the given nets, with whatever poses the parts hold now. */
static coord_t nets_hpwl(const solver_t *s, const uint32_t *net_ids, uint32_t count)
{
    const netlist_csr_t *nets = &s->ctx->nets;
    const pins_soa_t *pins = &s->ctx->pins;
    const uint32_t npin = s->npin;
    coord_t total = 0.0f;
    for (uint32_t k = 0u; k < count; ++k) {
        const uint32_t net = net_ids[k];
        const uint32_t begin = nets->net_offsets[net];
        const uint32_t end = nets->net_offsets[net + 1u];
        if (end - begin < 2u) {
            continue;
        }
        coord_t lo_x = 0.0f;
        coord_t hi_x = 0.0f;
        coord_t lo_y = 0.0f;
        coord_t hi_y = 0.0f;
        for (uint32_t e = begin; e < end; ++e) {
            const uint32_t pin = nets->net_to_pins[e];
            const uint32_t comp = pins->comp_id[pin];
            const uint32_t o = s->orient[comp];
            const coord_t px = s->x[comp] + s->rot_dx[o * npin + pin];
            const coord_t py = s->y[comp] + s->rot_dy[o * npin + pin];
            if (e == begin) {
                lo_x = hi_x = px;
                lo_y = hi_y = py;
                continue;
            }
            lo_x = (px < lo_x) ? px : lo_x;
            hi_x = (px > hi_x) ? px : hi_x;
            lo_y = (py < lo_y) ? py : lo_y;
            hi_y = (py > hi_y) ? py : hi_y;
        }
        total += (hi_x - lo_x) + (hi_y - lo_y);
    }
    return total;
}

/* Put a part in a slot. The extents follow the orientation, exactly as the
 * annealer's own put(): a bucket shares one footprint, so this is all a
 * permutation of poses needs. */
static void set_pose(solver_t *s, uint32_t comp, const slot_t *slot)
{
    s->x[comp] = slot->x;
    s->y[comp] = slot->y;
    s->orient[comp] = slot->orient;
    const components_soa_t *c = &s->ctx->comps;
    const bool odd = (slot->orient & 1u) != 0u;
    s->half_w[comp] = odd ? c->half_h[comp] : c->half_w[comp];
    s->half_h[comp] = odd ? c->half_w[comp] : c->half_h[comp];
}

/* Put part `member[i]` in slot `perm[i]`. */
static void apply_order(solver_t *s, const uint32_t *member, const slot_t *slots,
                        const uint32_t *perm, uint32_t k)
{
    for (uint32_t i = 0u; i < k; ++i) {
        set_pose(s, member[i], &slots[perm[i]]);
    }
}

/* Every ordering of k items, as a mixed-radix counter over an index array. */
static bool next_permutation(uint32_t *perm, uint32_t k)
{
    uint32_t i = k - 1u;
    while (i > 0u && perm[i - 1u] >= perm[i]) {
        i -= 1u;
    }
    if (i == 0u) {
        return false;
    }
    uint32_t j = k - 1u;
    while (perm[j] <= perm[i - 1u]) {
        j -= 1u;
    }
    const uint32_t swap = perm[i - 1u];
    perm[i - 1u] = perm[j];
    perm[j] = swap;
    for (uint32_t lo = i, hi = k - 1u; lo < hi; ++lo, --hi) {
        const uint32_t t = perm[lo];
        perm[lo] = perm[hi];
        perm[hi] = t;
    }
    return true;
}

/* The best ordering of one window, by the wirelength of the nets it touches.
 * Returns false when nothing beats the ordering the window already has. */
static bool best_order(solver_t *s, const uint32_t *member, uint32_t k, uint32_t *best)
{
    const netlist_csr_t *nets = &s->ctx->nets;
    const pins_soa_t *pins = &s->ctx->pins;
    slot_t slots[SWAP_WINDOW_MAX] = {{0.0f, 0.0f, 0u}};
    uint32_t nets_touched[256] = {0u};
    uint32_t nnet = 0u;
    uint32_t perm[SWAP_WINDOW_MAX] = {0u};
    for (uint32_t i = 0u; i < k; ++i) {
        slots[i].x = s->x[member[i]];
        slots[i].y = s->y[member[i]];
        slots[i].orient = s->orient[member[i]];
        perm[i] = i;
        const uint32_t begin = s->ctx->comps.first_pin[member[i]];
        const uint32_t end = begin + (uint32_t)s->ctx->comps.pin_count[member[i]];
        for (uint32_t p = begin; p < end && nnet < 256u; ++p) {
            const uint32_t net = pins->net_id[p];
            if (net == PLACE_ID_NONE) {
                continue;
            }
            bool seen = false;
            for (uint32_t q = 0u; q < nnet; ++q) {
                if (nets_touched[q] == net) {
                    seen = true;
                    break;
                }
            }
            if (!seen && net < nets->num_nets) {
                nets_touched[nnet++] = net;
            }
        }
    }
    apply_order(s, member, slots, perm, k);
    coord_t best_cost = nets_hpwl(s, nets_touched, nnet);
    for (uint32_t i = 0u; i < k; ++i) {
        best[i] = perm[i];
    }
    while (next_permutation(perm, k)) {
        apply_order(s, member, slots, perm, k);
        const coord_t cost = nets_hpwl(s, nets_touched, nnet);
        if (cost < best_cost - 1e-4f) {
            best_cost = cost;
            for (uint32_t i = 0u; i < k; ++i) {
                best[i] = perm[i];
            }
        }
    }
    /* Leave the window on its best ordering: the identity when the search found
     * nothing, the winner when it did. */
    apply_order(s, member, slots, best, k);
    for (uint32_t i = 0u; i < k; ++i) {
        if (best[i] != i) {
            return true;
        }
    }
    return false;
}

bool swap_window_pass(solver_t *s, uint32_t k)
{
    if (k < 2u || k > SWAP_WINDOW_MAX || s->swap_bucket_count == 0u) {
        return false;
    }
    uint32_t *members = SOLVER_ALLOC(s, uint32_t, s->ncomp);
    uint32_t *best = SOLVER_ALLOC(s, uint32_t, k);
    uint32_t *pair = SOLVER_ALLOC(s, uint32_t, 2u);
    if (members == nullptr || best == nullptr || pair == nullptr) {
        return false;
    }
    solver_best_t before;
    if (!solver_best_init(s, &before)) {
        return false;
    }
    solver_best_take(s, &before);
    bool changed = false;
    for (uint32_t round = 0u; round < 8u; ++round) {
        bool improved = false;
        for (uint32_t b = 0u; b < s->swap_bucket_count; ++b) {
            const uint32_t first = s->swap_first[b];
            const uint32_t len = s->swap_len[b];
            if (len < 2u) {
                continue;
            }
            /*
             * Every pair first, then the runs. The pairs are the exhaustive
             * part - a bucket of 48 capacitors is 1 128 pairs, and the one that
             * pays is rarely between neighbours on the x axis - and the runs
             * are what a stochastic walk cannot do at all: k parts have k!
             * orderings and the best one is a global property of the window.
             */
            for (uint32_t i = 0u; i < len; ++i) {
                for (uint32_t j = i + 1u; j < len; ++j) {
                    pair[0] = s->swap_member[first + i];
                    pair[1] = s->swap_member[first + j];
                    if (best_order(s, pair, 2u, best)) {
                        improved = true;
                        changed = true;
                    }
                }
            }
            if (len < k) {
                continue;
            }
            for (uint32_t i = 0u; i < len; ++i) {
                members[i] = s->swap_member[first + i];
            }
            /* Along a rail: the windows are runs of neighbours, which is where
             * a permutation has anything to gain. Insertion sort on x then y:
             * stable, and a bucket is short. */
            for (uint32_t i = 1u; i < len; ++i) {
                const uint32_t m = members[i];
                uint32_t j = i;
                while (j > 0u &&
                       (s->x[members[j - 1u]] > s->x[m] ||
                        (s->x[members[j - 1u]] == s->x[m] && s->y[members[j - 1u]] > s->y[m]))) {
                    members[j] = members[j - 1u];
                    j -= 1u;
                }
                members[j] = m;
            }
            /* Along a rail: the windows are runs of neighbours in the axis the
             * bucket spreads along, which is where a permutation has anything
             * to gain. Nearest-neighbour windows were measured too: no better
             * on r10 (seed 42: 29 781 against 29 674) and harder to explain. */
            for (uint32_t start = 0u; start + k <= len; ++start) {
                if (best_order(s, &members[start], k, best)) {
                    improved = true;
                    changed = true;
                }
            }
        }
        if (!improved) {
            break;
        }
    }
    if (changed) {
        /* The exchange is free of geometry, but crossings and the constraint
         * terms can still move: keep the pass only if the whole score fell. */
        const solver_cost_t after = solver_evaluate(s);
        if (after.score >= before.cost.score) {
            solver_best_restore(s, &before);
            return false;
        }
    }
    return changed;
}
