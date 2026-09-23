/*
 * matching_auction.c - the assignment, and the commit that moves the parts.
 *
 * The auction: every unassigned passive bids for its best slot, the price of a
 * slot rises with each bid, and a passive that loses a slot moves to its next
 * best. Prices make the result a global optimum of the sum of the costs, not a
 * greedy chain - which is the whole point of doing this instead of dropping
 * each capacitor at its nearest hole.
 */
#include "matching_internal.h"

#include <math.h>
#include <string.h>

/* One round of bidding over every row; returns how many are assigned. */
static uint32_t auction_round(const match_row_t *rows, uint32_t nrows, uint32_t *owner,
                              uint32_t *slot_of, coord_t *price)
{
    uint32_t assigned = 0u;
    for (uint32_t r = 0u; r < nrows; ++r) {
        const match_row_t *row = &rows[r];
        uint32_t best = SOLVER_NO_INDEX;
        uint32_t second = SOLVER_NO_INDEX;
        coord_t best_v = 0.0f;
        coord_t second_v = 0.0f;
        for (uint32_t c = 0u; c < row->count; ++c) {
            const coord_t v = row->cost[c] - price[r * MATCH_MAX_SLOTS + c];
            if (best == SOLVER_NO_INDEX || v < best_v) {
                second = best;
                second_v = best_v;
                best = c;
                best_v = v;
            } else if (second == SOLVER_NO_INDEX || v < second_v) {
                second = c;
                second_v = v;
            }
        }
        if (best == SOLVER_NO_INDEX) {
            continue;
        }
        if (second == SOLVER_NO_INDEX) {
            second_v = best_v; /* one candidate only: bid the minimum */
        }
        const coord_t bid = best_v - second_v + MATCH_EPS;
        const uint32_t flat = r * MATCH_MAX_SLOTS + best;
        const uint32_t held = owner[flat];
        if (held != SOLVER_NO_INDEX) {
            slot_of[held] = SOLVER_NO_INDEX;
        }
        owner[flat] = r;
        slot_of[r] = best;
        price[flat] += (bid > 0.0f) ? bid : MATCH_EPS;
        assigned += 1u;
    }
    return assigned;
}

uint32_t matching_assign_decoupling(solver_t *s)
{
    const constraint_decoupling_t *d = &s->ctx->constraints.decoupling;
    if (d->count == 0u || s->ctx->comps.count == 0u) {
        return 0u;
    }

    raster_mask_t mask;
    if (!raster_mask_init(&mask, s, MATCH_LATTICE)) {
        return 0u;
    }
    raster_mask_build_obstacles(&mask, s, s->opt->courtyard_clearance);

    match_row_t *rows = SOLVER_ALLOC(s, match_row_t, MATCH_MAX_PARTS);
    const uint32_t flat_count = MATCH_MAX_PARTS * MATCH_MAX_SLOTS;
    uint32_t *owner = SOLVER_ALLOC(s, uint32_t, flat_count);
    coord_t *price = SOLVER_ALLOC(s, coord_t, flat_count);
    uint32_t *slot_of = SOLVER_ALLOC(s, uint32_t, MATCH_MAX_PARTS);
    if (!s->ok) {
        return 0u;
    }
    memset(price, 0, (size_t)flat_count * sizeof(coord_t));
    for (uint32_t i = 0u; i < flat_count; ++i) {
        owner[i] = SOLVER_NO_INDEX;
    }

    const uint32_t nrows = matching_build_rows(s, &mask, rows, MATCH_MAX_PARTS);
    if (nrows == 0u) {
        return 0u;
    }
    /* The assignment minimises the wirelength of the passives it moves, but the
     * score also counts crossings and overlaps, and those can get worse: on r9
     * a 36 mm gain in wirelength came with eight new crossings, which is a loss
     * at the weights the solver runs with. So the whole assignment is scored
     * before and after, and undone if it did not pay. */
    coord_t *save_x = SOLVER_ALLOC(s, coord_t, nrows);
    coord_t *save_y = SOLVER_ALLOC(s, coord_t, nrows);
    if (!s->ok) {
        return 0u;
    }
    for (uint32_t r = 0u; r < nrows; ++r) {
        save_x[r] = s->x[rows[r].comp];
        save_y[r] = s->y[rows[r].comp];
    }
    const coord_t score_before = solver_evaluate(s).score;
    for (uint32_t r = 0u; r < nrows; ++r) {
        slot_of[r] = SOLVER_NO_INDEX;
    }

    for (uint32_t iter = 0u; iter < MATCH_MAX_ITER; ++iter) {
        if (auction_round(rows, nrows, owner, slot_of, price) == nrows) {
            break;
        }
    }

    /* No guard on the commit: the cost *is* the wirelength, so the auction's
     * global optimum is a real gain, not a rule satisfied at the nets' cost. */
    uint32_t moved = 0u;
    for (uint32_t r = 0u; r < nrows; ++r) {
        if (slot_of[r] == SOLVER_NO_INDEX) {
            continue;
        }
        const match_row_t *row = &rows[r];
        const coord_t x = row->cx[slot_of[r]];
        const coord_t y = row->cy[slot_of[r]];
        solver_nudge(s, row->comp, x - s->x[row->comp], y - s->y[row->comp]);
        moved += 1u;
    }
    if (moved > 0u && solver_evaluate(s).score >= score_before) {
        for (uint32_t r = 0u; r < nrows; ++r) {
            solver_nudge(s, rows[r].comp, save_x[r] - s->x[rows[r].comp],
                         save_y[r] - s->y[rows[r].comp]);
        }
        moved = 0u; /* the board keeps the arrangement the continuous stages found */
    }
    s->stats.matched = moved;
    return moved;
}
