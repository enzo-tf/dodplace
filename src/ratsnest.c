/*
 * ratsnest.c - the connectivity tree and the crossings it produces.
 *
 * The ratsnest is a rectilinear minimum spanning tree per net (Prim), not a
 * star: a star's hub is whichever pin happens to be leftmost, so moving a part
 * by a millimetre redrew every edge and changed the crossing count without the
 * layout changing. An MST is stable under small moves and its edges are the
 * ones a router actually lays down.
 */
#include "solver_internal.h"
#include "ratsnest.h"
#include "spatial_grid.h"

#include <math.h>
#include <stdlib.h>

void ratsnest_build(solver_t *s)
{
    const netlist_csr_t *nets = &s->ctx->nets;
    const pins_soa_t *pins = &s->ctx->pins;
    const uint32_t npin = pins->count;
    uint32_t seg = 0u;

    for (uint32_t n = 0u; n < nets->num_nets; ++n) {
        const uint32_t begin = nets->net_offsets[n];
        const uint32_t end = nets->net_offsets[n + 1u];
        s->seg_count_start[n] = seg;
        const uint32_t count = end - begin;
        if (count < 2u) {
            continue;
        }
        /* A net whose every pin sits on a locked part cannot be improved by any
         * move, so its crossings are a constant: leaving it out is free. */
        if (s->net_movable != nullptr && s->net_movable[n] == 0u) {
            continue;
        }
        if (count > SOLVER_MST_MAX_PINS) {
            continue; /* a power net with a hundred pins: not a routability signal */
        }

        /* --- Prim, over rectilinear distance, no heap: k is small ---------- */
        uint32_t idx[SOLVER_MST_MAX_PINS];
        coord_t best[SOLVER_MST_MAX_PINS];
        uint32_t from[SOLVER_MST_MAX_PINS];
        coord_t px[SOLVER_MST_MAX_PINS];
        coord_t py[SOLVER_MST_MAX_PINS];

        coord_t cx = 0.0f;
        coord_t cy = 0.0f;
        for (uint32_t e = begin; e < end; ++e) {
            const uint32_t pin = nets->net_to_pins[e];
            const uint32_t comp = pins->comp_id[pin];
            const uint32_t k = e - begin;
            px[k] = s->x[comp] + s->rot_dx[s->orient[comp] * npin + pin];
            py[k] = s->y[comp] + s->rot_dy[s->orient[comp] * npin + pin];
            cx += px[k];
            cy += py[k];
        }
        cx /= (coord_t)count;
        cy /= (coord_t)count;

        /* start from the pin closest to the centre: the tree then grows out of
         * the middle of the net, which is the shape a router would take */
        uint32_t seed = 0u;
        coord_t seed_d = fabsf(px[0] - cx) + fabsf(py[0] - cy);
        for (uint32_t k = 1u; k < count; ++k) {
            const coord_t d = fabsf(px[k] - cx) + fabsf(py[k] - cy);
            if (d < seed_d) {
                seed_d = d;
                seed = k;
            }
        }
        for (uint32_t k = 0u; k < count; ++k) {
            idx[k] = k;
            from[k] = seed;
            best[k] = fabsf(px[k] - px[seed]) + fabsf(py[k] - py[seed]);
        }
        best[seed] = -1.0f; /* in the tree */

        for (uint32_t step = 0u; step < count - 1u && seg < npin; ++step) {
            uint32_t pick = SOLVER_NO_INDEX;
            coord_t pick_d = 0.0f;
            for (uint32_t k = 0u; k < count; ++k) {
                if (best[k] < 0.0f) {
                    continue;
                }
                if (pick == SOLVER_NO_INDEX || best[k] < pick_d) {
                    pick = k;
                    pick_d = best[k];
                }
            }
            if (pick == SOLVER_NO_INDEX) {
                break;
            }
            s->seg_x1[seg] = px[from[pick]];
            s->seg_y1[seg] = py[from[pick]];
            s->seg_x2[seg] = px[pick];
            s->seg_y2[seg] = py[pick];
            s->seg_net[seg] = n;
            seg += 1u;
            best[pick] = -1.0f;
            for (uint32_t k = 0u; k < count; ++k) {
                if (best[k] < 0.0f) {
                    continue;
                }
                const coord_t d = fabsf(px[k] - px[pick]) + fabsf(py[k] - py[pick]);
                if (d < best[k]) {
                    best[k] = d;
                    from[k] = pick;
                }
            }
        }
        (void)idx;
    }
    s->seg_count_start[nets->num_nets] = seg;
    s->nsegments = seg;
}

/*
 * Crossings between ratsnest segments that share a grid cell. A pair is counted
 * once, using the smaller home cell as the owner; segments of the same net are
 * skipped because a net's own chain crossing itself is not a routability
 * problem between nets.
 */
