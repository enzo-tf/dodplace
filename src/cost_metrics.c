/*
 * cost_metrics.c - the wirelength terms of the score.
 *
 * HPWL is the half perimeter of each net's bounding box; the displacement term
 * measures how far the movable parts have drifted from where the designer left
 * them - a board that arrives three quarters locked is not a blank sheet.
 */
#include "solver_internal.h"

#include <math.h>

coord_t solver_hpwl(const solver_t *s)
{
    const netlist_csr_t *nets = &s->ctx->nets;
    const pins_soa_t *pins = &s->ctx->pins;
    coord_t total = 0.0f;

    for (uint32_t n = 0u; n < nets->num_nets; ++n) {
        const uint32_t begin = nets->net_offsets[n];
        const uint32_t end = nets->net_offsets[n + 1u];
        if (end - begin < 2u) {
            continue; /* a net needs two pins to have a length */
        }
        if (s->net_plane != nullptr && s->net_plane[n] != 0u) {
            continue; /* a rail, not a trace: see solver_state.c */
        }
        coord_t min_x = 0.0f;
        coord_t max_x = 0.0f;
        coord_t min_y = 0.0f;
        coord_t max_y = 0.0f;
        for (uint32_t e = begin; e < end; ++e) {
            const uint32_t pin = nets->net_to_pins[e];
            const uint32_t comp = pins->comp_id[pin];
            const uint32_t o = s->orient[comp];
            const coord_t px = s->x[comp] + s->rot_dx[o * s->ctx->pins.count + pin];
            const coord_t py = s->y[comp] + s->rot_dy[o * s->ctx->pins.count + pin];
            if (e == begin) {
                min_x = max_x = px;
                min_y = max_y = py;
            } else {
                min_x = (px < min_x) ? px : min_x;
                max_x = (px > max_x) ? px : max_x;
                min_y = (py < min_y) ? py : min_y;
                max_y = (py > max_y) ? py : max_y;
            }
        }
        total += nets->weights[n] * ((max_x - min_x) + (max_y - min_y));
    }
    return total;
}

/* ========================================================================= */
/* Uniform grids                                                             */
/* ========================================================================= */

coord_t solver_displacement(solver_t *s)
{
    const components_soa_t *c = &s->ctx->comps;
    coord_t total = 0.0f;
    for (uint32_t i = 0u; i < s->ncomp; ++i) {
        if (!solver_component_is_movable(s, i)) {
            continue;
        }
        /* Quadratic, and weighted by area: a small drift is what placement is
         * for, while a package sailing 40 mm across a packed board is what
         * wedges itself into a pocket no push can open. A 0402 hopping a few
         * millimetres pays almost nothing; a 6.6 mm part jumping 20 mm pays
         * hundreds of millimetre-equivalents and the annealer leaves it where
         * the designer put it. */
        const coord_t dx = s->x[i] - c->x[i];
        const coord_t dy = s->y[i] - c->y[i];
        const coord_t area = s->half_w[i] * s->half_h[i];
        total += area * (dx * dx + dy * dy);
    }
    return total;
}

/* Nudges a component; a clustered satellite moves its whole cluster instead. */

/*
 * Thermal exclusion, soft.
 *
 * A part that dissipates is a part the layout should keep away from the other
 * hot parts, and the rule is a radius: inside it the pair costs, outside it
 * costs nothing. Squared, so the penalty is gentle at the rim and steep at the
 * centre - a solver that can trade a millimetre of wire for a millimetre of
 * clearance does, and one that cannot is not forced into an illegal layout.
 *
 * Only the declared heat sources take part (thermal_min_power, 0.3 W: it keeps
 * the 34 drivers and regulators and leaves the 481 LEDs out, which is what
 * stops the rule from pushing the LED matrix apart). Pairs of two locked parts
 * are skipped, like the overlap count: the solver cannot move either one.
 */
coord_t solver_thermal_cost(const solver_t *s)
{
    const constraint_thermal_t *t = &s->ctx->constraints.thermal;
    if (s->opt->w_thermal <= 0.0f || s->nhot < 2u) {
        return 0.0f;
    }
    coord_t total = 0.0f;
    for (uint32_t a = 0u; a < s->nhot; ++a) {
        const uint32_t i = s->hot[a];
        const coord_t ri = t->exclusion_radius[i];
        for (uint32_t b = a + 1u; b < s->nhot; ++b) {
            const uint32_t j = s->hot[b];
            if (!solver_component_is_movable(s, i) && !solver_component_is_movable(s, j)) {
                continue;
            }
            const coord_t rj = t->exclusion_radius[j];
            const coord_t radius = (ri > rj) ? ri : rj;
            const coord_t dx = s->x[i] - s->x[j];
            const coord_t dy = s->y[i] - s->y[j];
            const coord_t dist = sqrtf(dx * dx + dy * dy);
            if (dist >= radius) {
                continue;
            }
            const coord_t pen = radius - dist;
            total += pen * pen;
        }
    }
    return total;
}

/*
 * Power integrity, as a distance the solver pays for.
 *
 * A decoupling capacitor only works if its loop with the IC is short: the
 * parasitic inductance of the trace eats the charge before it arrives. The
 * producer states the rule - which IC pin, which capacitor, and how far the
 * pair may drift (max_dist_sq, 1.5 to 2.5 mm on r10) - and this term charges
 * for the excess, squared, so a capacitor that leaves its pin is pulled back
 * long before it reaches another rail.
 */
coord_t solver_decoupling_cost(const solver_t *s)
{
    const constraint_decoupling_t *d = &s->ctx->constraints.decoupling;
    if (s->opt->w_decoupling <= 0.0f || d->count == 0u || s->ctx->pins.count == 0u) {
        return 0.0f;
    }
    const pins_soa_t *pins = &s->ctx->pins;
    const uint32_t npin = pins->count;
    coord_t total = 0.0f;
    for (uint32_t k = 0u; k < d->count; ++k) {
        const uint32_t cap = d->cap_comp[k];
        const uint32_t ic = d->ic_comp[k];
        if (cap == PLACE_ID_NONE || ic == PLACE_ID_NONE) {
            continue;
        }
        if (!solver_component_is_movable(s, cap)) {
            continue; /* nothing to pull: the capacitor is the designer's */
        }
        coord_t tx = s->x[ic];
        coord_t ty = s->y[ic];
        const place_id_t pin = d->ic_pin[k];
        if (pin != PLACE_ID_NONE && pins->comp_id[pin] == ic) {
            tx += s->rot_dx[s->orient[ic] * npin + pin];
            ty += s->rot_dy[s->orient[ic] * npin + pin];
        }
        const coord_t dx = s->x[cap] - tx;
        const coord_t dy = s->y[cap] - ty;
        const coord_t allowed = (d->max_dist_sq[k] > 0.0f) ? sqrtf(d->max_dist_sq[k]) : 2.5f;
        const coord_t dist = sqrtf(dx * dx + dy * dy);
        if (dist <= allowed) {
            continue;
        }
        /* Linear and capped: a squared excess over 96 rules dwarfs the
         * wirelength (it took r10 from 36.9 m to 76.9 m measured), while a
         * gentle pull in the first few millimetres is what the rule is for.
         * Past a few millimetres the capacitor is simply not decoupling any
         * more, and charging more would only scatter the rest of the board. */
        const coord_t excess = dist - allowed;
        const coord_t charged = (excess < 5.0f) ? excess : 5.0f;
        total += d->weight[k] * charged;
    }
    return total;
}

/*
 * Differential pairs: the two strands must arrive together. Their skew is the
 * difference of the two nets' half perimeters, and only the part beyond
 * max_skew_mm is charged - a pair that is already matched costs nothing. The
 * table is empty on r10, so this runs on synthetic scenes today and waits for
 * a board that declares its pairs.
 */
coord_t solver_diffpair_cost(const solver_t *s)
{
    const diffpair_table_t *dp = &s->ctx->constraints.diffpairs;
    if (s->opt->w_diffpair <= 0.0f || dp->count == 0u || s->ctx->nets.num_nets == 0u) {
        return 0.0f;
    }
    const netlist_csr_t *nets = &s->ctx->nets;
    coord_t total = 0.0f;
    for (uint32_t k = 0u; k < dp->count; ++k) {
        const place_id_t np = dp->net_p[k];
        const place_id_t nn = dp->net_n[k];
        if (np >= nets->num_nets || nn >= nets->num_nets) {
            continue;
        }
        coord_t len[2] = {0.0f, 0.0f};
        for (uint32_t side = 0u; side < 2u; ++side) {
            const place_id_t net = (side == 0u) ? np : nn;
            const uint32_t begin = nets->net_offsets[net];
            const uint32_t end = nets->net_offsets[net + 1u];
            coord_t lo_x = 0.0f;
            coord_t hi_x = 0.0f;
            coord_t lo_y = 0.0f;
            coord_t hi_y = 0.0f;
            for (uint32_t e = begin; e < end; ++e) {
                const place_id_t q = nets->net_to_pins[e];
                const place_id_t oc = s->ctx->pins.comp_id[q];
                const coord_t px = s->x[oc] + s->rot_dx[s->orient[oc] * PIN_STRIDE(s) + q];
                const coord_t py = s->y[oc] + s->rot_dy[s->orient[oc] * PIN_STRIDE(s) + q];
                if (e == begin) {
                    lo_x = hi_x = px;
                    lo_y = hi_y = py;
                } else {
                    lo_x = (px < lo_x) ? px : lo_x;
                    hi_x = (px > hi_x) ? px : hi_x;
                    lo_y = (py < lo_y) ? py : lo_y;
                    hi_y = (py > hi_y) ? py : hi_y;
                }
            }
            len[side] = (hi_x - lo_x) + (hi_y - lo_y);
        }
        const coord_t skew = fabsf(len[0] - len[1]);
        const coord_t allowed = (dp->max_skew_mm[k] > 0.0f) ? dp->max_skew_mm[k] : 0.5f;
        if (skew <= allowed) {
            continue;
        }
        const coord_t excess = skew - allowed;
        const coord_t charged = (excess < 5.0f) ? excess : 5.0f;
        total += charged;
    }
    return total;
}
