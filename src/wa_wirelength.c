/*
 * wa_wirelength.c - see wa_wirelength.h.
 */
#include "wa_wirelength.h"

#include <math.h>

/* One axis of the soft bounding box: the weighted mean of the coordinates,
 * weighted by exp(+v/g) for the top and exp(-v/g) for the bottom. Both sums are
 * shifted by the extremum so the exponentials cannot overflow. */
static coord_t wa_axis(const solver_t *s, uint32_t net, uint32_t moved_pin, coord_t px,
                       coord_t py, coord_t gamma, bool high, bool use_y)
{
    const netlist_csr_t *nets = &s->ctx->nets;
    const pins_soa_t *pins = &s->ctx->pins;
    const uint32_t npin = s->npin;
    const uint32_t begin = nets->net_offsets[net];
    const uint32_t end = nets->net_offsets[net + 1u];
    coord_t ext = 0.0f;
    for (uint32_t e = begin; e < end; ++e) {
        coord_t v;
        if (e == moved_pin) {
            v = use_y ? py : px;
        } else {
            const uint32_t comp = pins->comp_id[e];
            const uint32_t o = s->orient[comp];
            v = s->x[comp] + (use_y ? s->rot_dy[o * (size_t)npin + e]
                                    : s->rot_dx[o * (size_t)npin + e]);
        }
        if (e == begin) {
            ext = v;
        } else if (high) {
            ext = (v > ext) ? v : ext;
        } else {
            ext = (v < ext) ? v : ext;
        }
    }
    coord_t num = 0.0f;
    coord_t den = 0.0f;
    for (uint32_t e = begin; e < end; ++e) {
        coord_t v;
        if (e == moved_pin) {
            v = use_y ? py : px;
        } else {
            const uint32_t comp = pins->comp_id[e];
            const uint32_t o = s->orient[comp];
            v = s->x[comp] + (use_y ? s->rot_dy[o * (size_t)npin + e]
                                    : s->rot_dx[o * (size_t)npin + e]);
        }
        const coord_t shifted = high ? (v - ext) : (ext - v);
        const coord_t weight = expf(shifted / gamma);
        num += v * weight;
        den += weight;
    }
    return (den > 0.0f) ? (num / den) : ext;
}

coord_t wa_net_length(const solver_t *s, uint32_t net, uint32_t moved_pin, coord_t px,
                      coord_t py, coord_t gamma)
{
    const netlist_csr_t *nets = &s->ctx->nets;
    if (net >= nets->num_nets || nets->net_offsets[net + 1u] - nets->net_offsets[net] < 2u) {
        return 0.0f;
    }
    if (!(gamma > 0.0f)) {
        /* No smoothing: the estimate degenerates to the true bounding box. */
        const coord_t lo_x = wa_axis(s, net, moved_pin, px, py, 1e-6f, false, false);
        const coord_t hi_x = wa_axis(s, net, moved_pin, px, py, 1e-6f, true, false);
        const coord_t lo_y = wa_axis(s, net, moved_pin, px, py, 1e-6f, false, true);
        const coord_t hi_y = wa_axis(s, net, moved_pin, px, py, 1e-6f, true, true);
        return (hi_x - lo_x) + (hi_y - lo_y);
    }
    return (wa_axis(s, net, moved_pin, px, py, gamma, true, false) -
            wa_axis(s, net, moved_pin, px, py, gamma, false, false)) +
           (wa_axis(s, net, moved_pin, px, py, gamma, true, true) -
            wa_axis(s, net, moved_pin, px, py, gamma, false, true));
}
