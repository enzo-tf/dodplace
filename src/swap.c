/*
 * swap.c - see swap.h.
 */
#include "swap.h"
#include "copper_test.h"
#include "shape.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static bool pose_is_clear(const solver_t *s, uint32_t comp, coord_t x, coord_t y)
{
    for (uint32_t j = 0u; j < s->ncomp; ++j) {
        if (j == comp || s->ruin_mask[j] != 0u) {
            continue;
        }
        /* The real conflict test, not the copper margin: the neighbours that
         * made a pose legal sit at the courtyard clearance, and demanding the
         * copper margin of them rejected every candidate (swaps stayed at 0).
         * The margin is the optimiser's business, the cost function judges it. */
        if (shape_overlaps_posed(s, comp, x, y, j, s->opt->courtyard_clearance)) {
            return false;
        }
    }
    return true;
}

static void put(const solver_t *s, uint32_t comp, coord_t x, coord_t y, uint8_t orient)
{
    s->x[comp] = x;
    s->y[comp] = y;
    s->orient[comp] = orient;
    const components_soa_t *c = &s->ctx->comps;
    const bool odd = (orient & 1u) != 0u;
    s->half_w[comp] = odd ? c->half_h[comp] : c->half_w[comp];
    s->half_h[comp] = odd ? c->half_w[comp] : c->half_h[comp];
}

bool swap_propose(solver_t *s, swap_move_t *move)
{
    if (s->swap_bucket_count == 0u) {
        return false;
    }
    const uint32_t bucket = solver_rand_below(s, s->swap_bucket_count);
    const uint32_t first = s->swap_first[bucket];
    const uint32_t len = s->swap_len[bucket];
    if (len < 2u) {
        return false;
    }
    const uint32_t a = s->swap_member[first + solver_rand_below(s, len)];
    uint32_t b = s->swap_member[first + solver_rand_below(s, len)];
    if (a == b) {
        return false;
    }

    move->a = a;
    move->b = b;
    move->ax = s->x[a];
    move->ay = s->y[a];
    move->a_orient = s->orient[a];
    move->bx = s->x[b];
    move->by = s->y[b];
    move->b_orient = s->orient[b];

    /* Same footprint on the same side: the two rectangles land on cells that
     * already hold one of them, so only the surroundings can object. */
    put(s, a, move->bx, move->by, move->b_orient);
    put(s, b, move->ax, move->ay, move->a_orient);
    if (!pose_is_clear(s, a, move->bx, move->by) || !pose_is_clear(s, b, move->ax, move->ay)) {
        if (getenv("DODPLACE_SWAP_DEBUG") != nullptr) {
            (void)fprintf(stderr, "swap refused: a=%u clear=%d b=%u clear=%d\n", a,
                          (int)pose_is_clear(s, a, move->bx, move->by), b,
                          (int)pose_is_clear(s, b, move->ax, move->ay));
        }
        swap_undo(s, move);
        return false;
    }
    return true;
}

void swap_undo(solver_t *s, const swap_move_t *move)
{
    put(s, move->a, move->ax, move->ay, move->a_orient);
    put(s, move->b, move->bx, move->by, move->b_orient);
}
