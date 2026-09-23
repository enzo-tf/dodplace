/*
 * pin_torque.c - the orientation heuristic.
 *
 * A part with a net on one side and nothing on the other has one orientation
 * that points its pins at their partners and three that make the router go
 * around. The torque its connections exert is
 *
 *     tau = sum over pins of (r_pin x F_pin)
 *
 * with r_pin the pin offset from the centre and F_pin the vector to the
 * centroid of the *other* pins on that net. The orientation that zeroes it is
 * the one where the pin field faces its wiring, and unlike a full cost
 * evaluation it costs one pass over the part's own pins.
 */
#include "solver_internal.h"

#include <math.h>

static coord_t pin_torque_at(const solver_t *s, uint32_t comp, uint8_t orient)
{
    const pins_soa_t *pins = &s->ctx->pins;
    const netlist_csr_t *nets = &s->ctx->nets;
    const uint32_t npin = pins->count;
    const uint32_t begin = s->ctx->comps.first_pin[comp];
    const uint32_t end = begin + (uint32_t)s->ctx->comps.pin_count[comp];
    if (end - begin < 3u) {
        return 0.0f; /* nothing to untangle on a two-terminal part */
    }

    const coord_t cx = s->x[comp];
    const coord_t cy = s->y[comp];
    coord_t torque = 0.0f;
    for (uint32_t p = begin; p < end; ++p) {
        const place_id_t net = pins->net_id[p];
        if (net == PLACE_ID_NONE) {
            continue;
        }
        const uint32_t nb = nets->net_offsets[net];
        const uint32_t ne = nets->net_offsets[net + 1u];
        if (ne - nb < 2u) {
            continue;
        }
        const coord_t px = cx + s->rot_dx[orient * npin + p];
        const coord_t py = cy + s->rot_dy[orient * npin + p];
        coord_t tx = 0.0f;
        coord_t ty = 0.0f;
        uint32_t others = 0u;
        for (uint32_t e = nb; e < ne; ++e) {
            const place_id_t q = nets->net_to_pins[e];
            if (q == p) {
                continue;
            }
            const place_id_t other = pins->comp_id[q];
            tx += s->x[other] + s->rot_dx[s->orient[other] * npin + q];
            ty += s->y[other] + s->rot_dy[s->orient[other] * npin + q];
            others += 1u;
        }
        if (others == 0u) {
            continue;
        }
        tx /= (coord_t)others;
        ty /= (coord_t)others;
        const coord_t rx = s->rot_dx[orient * npin + p];
        const coord_t ry = s->rot_dy[orient * npin + p];
        torque += rx * (ty - py) - ry * (tx - px);
    }
    return torque;
}

/*
 * The orientation among the allowed ones with the smallest torque - the pose in
 * which the part's pins face the nets they belong to. Ties keep the current
 * orientation, so a part that is already canonical is not rotated for nothing.
 */
uint8_t solver_torque_orientation(const solver_t *s, uint32_t comp)
{
    const uint8_t current = s->orient[comp];
    coord_t best = fabsf(pin_torque_at(s, comp, current));
    uint8_t best_orient = current;
    for (uint32_t o = 0u; o < 4u; ++o) {
        if ((s->opt->rotation_mask & (1u << o)) == 0u || o == current) {
            continue;
        }
        const coord_t torque = fabsf(pin_torque_at(s, comp, (uint8_t)o));
        if (torque < best) {
            best = torque;
            best_orient = (uint8_t)o;
        }
    }
    return best_orient;
}

/*
 * How far the movable parts have drifted from where the designer left them.
 *
 * A board that arrives 74 % locked is not a blank sheet: those parts are frozen
 * around a layout that already works, and the free ones were placed to fit it.
 * Without this term the annealer is free to sail a package 100 mm across the
 * board chasing a few millimetres of wire - and it does, burying it in a
 * neighbour no push can rescue and tangling the ratsnest on the way. The anchor
 * makes every move pay for the distance it covers.
 */
