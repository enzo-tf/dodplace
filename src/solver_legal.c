/*
 * legalize.c - stage 4: make the layout manufacturable.
 *
 * The annealing leaves a good layout that is not legal: courtyards touch,
 * parts sit outside the board. This stage resolves every overlap it is allowed
 * to resolve, by minimum translation vector first and by slot search after
 * (lns_repair.c), and confines the result to the board and out of the keepouts.
 *
 * The SAT of the spec reduces to something cheaper here: the courtyards are
 * rectangles and the orientations are quarter turns, so an oriented box is
 * always an axis-aligned box with swapped extents (solver_rebuild_extents). The
 * minimum translation vector is therefore the smaller of the two penetration
 * depths - no separating-axis search needed.
 */
#include "solver_internal.h"
#include "lns_repair.h"
#include "legalize_keepout.h"
#include "polish.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

void solver_legalize(solver_t *s)
{
    const coord_t gap = s->opt->courtyard_clearance;
    const uint32_t max_pairs = (s->ncomp > 0u) ? s->ncomp * 4u : 1u;
    uint32_t *pi = SOLVER_ALLOC(s, uint32_t, max_pairs);
    uint32_t *pj = SOLVER_ALLOC(s, uint32_t, max_pairs);
    if (!s->ok) {
        return;
    }

    /* The annealing may have rotated a master since the members were last
     * placed: re-derive them, or the union below is computed from stale
     * positions. */
    solver_sync_cluster_members(s);

    /* The annealing rotates parts, and a quarter turn swaps their extents: a
     * component that fitted before may stick out now, with no overlap to notice
     * it. Confine first, then resolve. */
    for (uint32_t i = 0u; i < s->ncomp; ++i) {
        if (solver_component_is_movable(s, i)) {
            solver_clamp_to_board(s, i);
        }
    }

    for (uint32_t pass = 0u; pass < s->opt->legalize_passes; ++pass) {
        const uint32_t found = solver_collect_overlap_pairs(s, pi, pj, max_pairs);
        if (found == 0u) {
            break;
        }
        for (uint32_t k = 0u; k < found; ++k) {
            const uint32_t i = pi[k];
            const uint32_t j = pj[k];
            const coord_t dx = s->x[i] - s->x[j];
            const coord_t dy = s->y[i] - s->y[j];
            const coord_t pen_x = (s->half_w[i] + s->half_w[j] + gap) - fabsf(dx);
            const coord_t pen_y = (s->half_h[i] + s->half_h[j] + gap) - fabsf(dy);
            if (pen_x <= 0.0f || pen_y <= 0.0f) {
                continue;
            }
            const bool mi = solver_component_is_movable(s, i);
            const bool mj = solver_component_is_movable(s, j);
            if (!mi && !mj) {
                continue; /* two locked parts: the designer's overlap is not ours */
            }
            /* A big package holds its ground and a passive gives way: the
             * small part is the one with somewhere to go. */
            const coord_t area_i = s->half_w[i] * s->half_h[i];
            const coord_t area_j = s->half_w[j] * s->half_h[j];
            /* the smaller part yields the whole push; a package holds its ground */
            const coord_t share_i = (mi && mj) ? (area_i > area_j ? 0.0f : 1.0f) : 1.0f;
            const coord_t share_j = 1.0f - share_i;
            if (pen_x < pen_y) {
                const coord_t push = pen_x + 0.001f;
                const coord_t dir = (dx >= 0.0f) ? 1.0f : -1.0f;
                if (mi) {
                    solver_nudge(s, i, dir * push * share_i, 0.0f);
                }
                if (mj) {
                    solver_nudge(s, j, -dir * push * share_j, 0.0f);
                }
            } else {
                const coord_t push = pen_y + 0.001f;
                const coord_t dir = (dy >= 0.0f) ? 1.0f : -1.0f;
                if (mi) {
                    solver_nudge(s, i, 0.0f, dir * push * share_i);
                }
                if (mj) {
                    solver_nudge(s, j, 0.0f, -dir * push * share_j);
                }
            }
            s->stats.legalize_pushes += 1u;
        }
        for (uint32_t i = 0u; i < s->ncomp; ++i) {
            if (solver_component_is_movable(s, i)) {
                solver_clamp_to_board(s, i);
            }
        }
        push_out_of_keepouts(s);
    }

    /*
     * Whatever is still stuck gets a slot, biggest part first. Rounds rather
     * than one sweep: moving a part frees the space the next one needs, and a
     * single pass would stop while the board is still dirty.
     */
    uint32_t *order = SOLVER_ALLOC(s, uint32_t, s->ncomp);
    if (s->ok) {
        uint32_t count = 0u;
        lns_order_by_area(s, order, &count);
        for (uint32_t round = 0u; round < s->opt->legalize_rounds; ++round) {
            uint32_t resolved = 0u;
            for (uint32_t k = 0u; k < count; ++k) {
                const uint32_t comp = order[k];
                if (!lns_comp_collides(s, comp, gap)) {
                    continue;
                }
                coord_t nx = 0.0f;
                coord_t ny = 0.0f;
                if (!lns_spiral_slot(s, comp, &nx, &ny) && !lns_seed_slot(s, comp, &nx, &ny) &&
                    !lns_ruin_and_recreate(s, comp, gap) && !lns_restore_to_seed(s, comp)) {
                    continue; /* the board has no room for it at this clearance */
                }
                /* every slot helper proves the part fits the board, so no
                 * clamp here: a clamp after a verified placement would only be
                 * able to move the part into something */
                solver_nudge(s, comp, nx - s->x[comp], ny - s->y[comp]);
                resolved += 1u;
                s->stats.legalize_pushes += 1u;
            }
            if (resolved == 0u) {
                break;
            }
        }
    }

    /* No blanket clamp here. Every slot helper proves the part fits the board
     * before placing it, so a clamp at this point could only take a part that
     * is already legal and shove it into a neighbour - the last thing the stage
     * does would be to undo it. */

    /* Two numbers, because they mean different things: a true collision is a
     * layout failure, while a clearance violation between two locked parts is
     * the designer's own decision that the solver is not allowed to undo. */
    /* The mask rule on top of the clearance: a few microns, at the very end,
     * where nothing else moves any more. */
    solver_polish(s);

    if (getenv("DODPLACE_DUMP_CONFLICTS") != nullptr) {
        for (uint32_t q = 0u; q < s->ncomp; ++q) {
            if (s->ctx->comps.flags[q] == 0u) { continue; }
            if (q == 563u || q == 592u) {
                (void)fprintf(stderr, "  probe %u: (%.3f, %.3f) half (%.3f, %.3f) mov=%d\n", q,
                              (double)s->x[q], (double)s->y[q], (double)s->half_w[q],
                              (double)s->half_h[q], (int)solver_component_is_movable(s, q));
            }
        }
        const uint32_t ncomp = s->ncomp;
        for (uint32_t i = 0u; i < ncomp; ++i) {
            if (!solver_component_is_movable(s, i)) {
                continue;
            }
            for (uint32_t j = 0u; j < ncomp; ++j) {
                if (j == i || s->ruin_mask[j] != 0u) {
                    continue;
                }
                if (!boxes_overlap(s->x[i], s->y[i], s->half_w[i], s->half_h[i], s->x[j],
                                   s->y[j], s->half_w[j], s->half_h[j], 0.0f)) {
                    continue;
                }
                (void)fprintf(stderr,
                              "  final conflict %u(mov=1,%.2fx%.2f @ %.2f,%.2f) x "
                              "%u(mov=%d,%.2fx%.2f @ %.2f,%.2f)\n",
                              i, (double)(s->half_w[i] * 2.0f), (double)(s->half_h[i] * 2.0f),
                              (double)s->x[i], (double)s->y[i], j,
                              (int)solver_component_is_movable(s, j),
                              (double)(s->half_w[j] * 2.0f), (double)(s->half_h[j] * 2.0f),
                              (double)s->x[j], (double)s->y[j]);
            }
        }
    }
    s->stats.unplaced = solver_count_conflicts(s, 0.0f);
    s->stats.unplaced_fixable = s->stats.unplaced;
    s->stats.locked_overlaps = solver_count_locked_conflicts(s, 0.0f);
    s->stats.clearance_violations = solver_count_conflicts(s, s->opt->courtyard_clearance);
}
