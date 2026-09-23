/*
 * matching_slots.c - the candidate slots of one passive and what they cost.
 */
#include "matching_internal.h"
#include "lns_repair.h"

#include <math.h>

/*
 * The marginal wirelength of putting this passive at (x, y), with every other
 * pin frozen: the sum, over its own nets, of the bounding-box half perimeter
 * the net would then have. This is the SOTA edge cost - it prices the move
 * against the supply rail *and* the ground return at once, which is what the
 * distance to the decoupling pin alone could never see.
 */
static coord_t hpwl_delta_at(const solver_t *s, uint32_t comp, coord_t x, coord_t y)
{
    const pins_soa_t *pins = &s->ctx->pins;
    const netlist_csr_t *nets = &s->ctx->nets;
    const uint32_t npin = pins->count;
    const uint32_t begin = s->ctx->comps.first_pin[comp];
    const uint32_t end = begin + (uint32_t)s->ctx->comps.pin_count[comp];
    coord_t total = 0.0f;

    for (uint32_t p = begin; p < end; ++p) {
        const place_id_t net = pins->net_id[p];
        if (net == PLACE_ID_NONE) {
            continue;
        }
        const coord_t px = x + s->rot_dx[s->orient[comp] * npin + p];
        const coord_t py = y + s->rot_dy[s->orient[comp] * npin + p];
        coord_t lo_x = px;
        coord_t hi_x = px;
        coord_t lo_y = py;
        coord_t hi_y = py;
        const uint32_t nb = nets->net_offsets[net];
        const uint32_t ne = nets->net_offsets[net + 1u];
        for (uint32_t e = nb; e < ne; ++e) {
            const place_id_t q = nets->net_to_pins[e];
            if (q == p) {
                continue;
            }
            const place_id_t oc = pins->comp_id[q];
            const coord_t qx = s->x[oc] + s->rot_dx[s->orient[oc] * npin + q];
            const coord_t qy = s->y[oc] + s->rot_dy[s->orient[oc] * npin + q];
            lo_x = (qx < lo_x) ? qx : lo_x;
            hi_x = (qx > hi_x) ? qx : hi_x;
            lo_y = (qy < lo_y) ? qy : lo_y;
            hi_y = (qy > hi_y) ? qy : hi_y;
        }
        total += (hi_x - lo_x) + (hi_y - lo_y);
    }
    return total;
}

uint32_t matching_collect_slots(const solver_t *s, const raster_mask_t *mask, uint32_t comp,
                                coord_t tx, coord_t ty, coord_t reach, match_row_t *row)
{
    const coord_t gap = s->opt->courtyard_clearance;
    coord_t lo_x = 0.0f;
    coord_t lo_y = 0.0f;
    coord_t hi_x = 0.0f;
    coord_t hi_y = 0.0f;
    solver_union_extent(s, comp, &lo_x, &lo_y, &hi_x, &hi_y);

    row->comp = comp;
    row->count = 0u;
    row->tx = tx;
    row->ty = ty;

    /* A ring walk, nearest first, so a crowded target keeps the candidates that
     * matter when the list fills up. */
    const int32_t rings = (int32_t)(reach / MATCH_LATTICE) + 1;
    for (int32_t ring = 0; ring <= rings && row->count < MATCH_MAX_SLOTS; ++ring) {
        for (int32_t gy = -ring; gy <= ring; ++gy) {
            for (int32_t gx = -ring; gx <= ring; ++gx) {
                if (ring > 0 && gx > -ring && gx < ring && gy > -ring && gy < ring) {
                    continue;
                }
                const coord_t x = tx + (coord_t)gx * MATCH_LATTICE;
                const coord_t y = ty + (coord_t)gy * MATCH_LATTICE;
                const coord_t dx = x - tx;
                const coord_t dy = y - ty;
                if (sqrtf(dx * dx + dy * dy) > reach) {
                    continue;
                }
                if (!raster_mask_box_clear(mask, s->ctx->comps.height[comp],
                                           x + lo_x - gap * 0.5f, y + lo_y - gap * 0.5f,
                                           x + hi_x + gap * 0.5f, y + hi_y + gap * 0.5f)) {
                    continue;
                }
                if (!lns_slot_free(s, comp, x, y, gap)) {
                    continue; /* the mask is a filter; the exact test decides */
                }
                const uint32_t k = row->count;
                row->cx[k] = x;
                row->cy[k] = y;
                row->cost[k] = hpwl_delta_at(s, comp, x, y);
                row->count += 1u;
                if (row->count >= MATCH_MAX_SLOTS) {
                    break;
                }
            }
        }
    }
    return row->count;
}
