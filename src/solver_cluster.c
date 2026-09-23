/*
 * Stage 1 - semantic clustering.
 *
 * On a board you never place the individual parts of a function separately: a
 * switching converter, its inductor and its output capacitor are one object,
 * and scattering them across the board destroys EMC whatever the wirelength
 * says. The patterns are read from the netlist hypergraph and from the
 * component kinds the producer derived from the reference prefixes.
 *
 * Three patterns, in decreasing confidence:
 *
 *   decoupling   an explicit IC/capacitor pair declared by the producer (user
 *                rules or the netlist heuristic) - the strongest signal
 *   power loop   an inductor whose two nets reach an IC pin and a capacitor
 *   oscillator   a crystal whose two nets each carry a capacitor
 *
 * The relative geometry is *frozen from the input layout*: the designer's
 * arrangement is the seed, and the cluster then moves as a rigid body.
 */
#include "solver_internal.h"

#include <math.h>
#include <string.h>

static bool kind_is(const solver_t *s, uint32_t comp, comp_kind_t kind)
{
    return s->ctx->comps.kind[comp] == kind;
}

/* Pins of a component, up to `max` of them. */
static uint32_t comp_pins(const solver_t *s, uint32_t comp, uint32_t *out, uint32_t max)
{
    const components_soa_t *c = &s->ctx->comps;
    const uint32_t begin = c->first_pin[comp];
    const uint32_t end = begin + c->pin_count[comp];
    uint32_t n = 0u;
    for (uint32_t p = begin; p < end && n < max; ++p) {
        out[n++] = p;
    }
    return n;
}

static bool net_has_kind(const solver_t *s, uint32_t net, comp_kind_t kind)
{
    const netlist_csr_t *nets = &s->ctx->nets;
    const pins_soa_t *pins = &s->ctx->pins;
    for (uint32_t e = nets->net_offsets[net]; e < nets->net_offsets[net + 1u]; ++e) {
        if (s->ctx->comps.kind[pins->comp_id[nets->net_to_pins[e]]] == kind) {
            return true;
        }
    }
    return false;
}

static uint32_t net_first_of_kind(const solver_t *s, uint32_t net, comp_kind_t kind)
{
    const netlist_csr_t *nets = &s->ctx->nets;
    const pins_soa_t *pins = &s->ctx->pins;
    for (uint32_t e = nets->net_offsets[net]; e < nets->net_offsets[net + 1u]; ++e) {
        const uint32_t comp = pins->comp_id[nets->net_to_pins[e]];
        if (s->ctx->comps.kind[comp] == kind) {
            return comp;
        }
    }
    return SOLVER_NO_INDEX;
}

/* Appends a satellite to an existing cluster, freezing its relative pose. */
static void cluster_add(solver_t *s, uint32_t cl, uint32_t slave, uint32_t *write)
{
    const uint32_t master = s->cluster_master[cl];
    const uint8_t o = s->orient[master];
    const coord_t dx = s->x[slave] - s->x[master];
    const coord_t dy = s->y[slave] - s->y[master];
    coord_t lx = dx;
    coord_t ly = dy;
    if (o == 1u) {          /* R(-90) in KiCad's y-down frame */
        lx = dy;
        ly = -dx;
    } else if (o == 2u) {
        lx = -dx;
        ly = -dy;
    } else if (o == 3u) {
        lx = -dy;
        ly = dx;
    }
    s->member_comp[*write] = slave;
    s->member_dx[*write] = lx;
    s->member_dy[*write] = ly;
    s->member_dorient[*write] = (uint8_t)((s->orient[slave] - s->orient[master]) & 3u);
    *write += 1u;
    s->cluster_count[cl] += 1u;
    s->cluster_of[slave] = cl;
    s->stats.clustered_components += 1u;
}

/*
 * Starts a cluster from two free parts. The larger one becomes the master, so
 * the small decoupling capacitor or load capacitor orbits the part that
 * defines the block.
 */
static uint32_t cluster_new(solver_t *s, uint8_t *claimed, uint32_t a, uint32_t b,
                            uint32_t *write)
{
    const components_soa_t *c = &s->ctx->comps;
    if (a == b || a == SOLVER_NO_INDEX || b == SOLVER_NO_INDEX) {
        return SOLVER_NO_INDEX;
    }
    if (claimed[a] != 0u || claimed[b] != 0u) {
        return SOLVER_NO_INDEX;
    }
    if ((c->flags[a] & COMP_LOCKED) != 0u || (c->flags[b] & COMP_LOCKED) != 0u) {
        return SOLVER_NO_INDEX; /* a locked part is an anchor, not a satellite */
    }

    const coord_t area_a = c->half_w[a] * c->half_h[a];
    const coord_t area_b = c->half_w[b] * c->half_h[b];
    const uint32_t master = (area_a >= area_b) ? a : b;
    const uint32_t slave = (master == a) ? b : a;

    const uint32_t cl = s->nclusters;
    s->cluster_master[cl] = master;
    s->cluster_first[cl] = *write; /* members of a cluster stay contiguous */
    s->cluster_count[cl] = 0u;
    s->cluster_of[master] = cl;
    s->nclusters += 1u;
    claimed[master] = 1u;
    claimed[slave] = 1u;
    s->stats.clusters += 1u;
    s->stats.clustered_components += 1u;
    cluster_add(s, cl, slave, write);
    return cl;
}

void solver_cluster(solver_t *s)
{
    const pins_soa_t *pins = &s->ctx->pins;
    uint8_t *claimed = SOLVER_ALLOC(s, uint8_t, s->ncomp);
    if (!s->ok) {
        return;
    }
    memset(s->cluster_of, 0xFF, (size_t)s->ncomp * sizeof(uint32_t));
    s->nclusters = 0u;
    s->stats.clusters = 0u;
    s->stats.clustered_components = 0u;
    uint32_t write = 0u;

    /* --- 1. explicit decoupling pairs ---------------------------------- */
    const constraint_decoupling_t *dec = &s->ctx->constraints.decoupling;
    for (uint32_t r = 0u; r < dec->count; ++r) {
        (void)cluster_new(s, claimed, dec->ic_comp[r], dec->cap_comp[r], &write);
    }

    /* --- 2. power loops: inductor between an IC and a capacitor --------- */
    for (uint32_t comp = 0u; comp < s->ncomp; ++comp) {
        if (claimed[comp] != 0u || !kind_is(s, comp, COMP_KIND_INDUCTOR)) {
            continue;
        }
        uint32_t p[4];
        if (comp_pins(s, comp, p, 4u) < 2u) {
            continue;
        }
        const uint32_t net_a = pins->net_id[p[0]];
        const uint32_t net_b = pins->net_id[p[1]];
        if (net_a == PLACE_ID_NONE || net_b == PLACE_ID_NONE) {
            continue;
        }
        const bool a_ic = net_has_kind(s, net_a, COMP_KIND_IC);
        const bool b_ic = net_has_kind(s, net_b, COMP_KIND_IC);
        const bool a_cap = net_has_kind(s, net_a, COMP_KIND_CAPACITOR);
        const bool b_cap = net_has_kind(s, net_b, COMP_KIND_CAPACITOR);

        uint32_t ic = SOLVER_NO_INDEX;
        uint32_t cap = SOLVER_NO_INDEX;
        if (a_ic && b_cap) {
            ic = net_first_of_kind(s, net_a, COMP_KIND_IC);
            cap = net_first_of_kind(s, net_b, COMP_KIND_CAPACITOR);
        } else if (b_ic && a_cap) {
            ic = net_first_of_kind(s, net_b, COMP_KIND_IC);
            cap = net_first_of_kind(s, net_a, COMP_KIND_CAPACITOR);
        }
        if (ic == SOLVER_NO_INDEX) {
            continue;
        }
        /* the IC drives the block: it becomes the master when present */
        const uint32_t cl = cluster_new(s, claimed, ic, comp, &write);
        if (cl != SOLVER_NO_INDEX && cap != SOLVER_NO_INDEX && claimed[cap] == 0u) {
            claimed[cap] = 1u;
            cluster_add(s, cl, cap, &write);
        }
    }

    /* --- 3. oscillators: crystal plus its two load capacitors ----------- */
    for (uint32_t comp = 0u; comp < s->ncomp; ++comp) {
        if (claimed[comp] != 0u || !kind_is(s, comp, COMP_KIND_CRYSTAL)) {
            continue;
        }
        uint32_t p[4];
        if (comp_pins(s, comp, p, 4u) < 2u) {
            continue;
        }
        const uint32_t net_a = pins->net_id[p[0]];
        const uint32_t net_b = pins->net_id[p[1]];
        if (net_a == PLACE_ID_NONE || net_b == PLACE_ID_NONE) {
            continue;
        }
        const uint32_t cap_a = net_first_of_kind(s, net_a, COMP_KIND_CAPACITOR);
        const uint32_t cap_b = net_first_of_kind(s, net_b, COMP_KIND_CAPACITOR);
        if (cap_a == SOLVER_NO_INDEX || cap_b == SOLVER_NO_INDEX) {
            continue;
        }
        const uint32_t cl = cluster_new(s, claimed, comp, cap_a, &write);
        if (cl != SOLVER_NO_INDEX && claimed[cap_b] == 0u) {
            claimed[cap_b] = 1u;
            cluster_add(s, cl, cap_b, &write);
        }
    }
}
