/*
 * Stage 3 - untangling.
 *
 * This is where a placer earns its keep, and it is not about wirelength. Two
 * layouts can have exactly the same HPWL and completely different routability:
 * one is a clean fan-out, the other a butterfly of crossings that no router will
 * fit on four layers. So the annealing optimises a mixed cost:
 *
 *     score = w_hpwl * HPWL + w_crossings * crossings + w_overlaps * overlaps
 *
 * The moves are the discrete choices a placer actually has: swap two parts, or
 * rotate one by a quarter turn. A rotation is often worth more than a swap,
 * because it can remove a whole bundle of crossings without moving the part.
 *
 * The walk ends with a deterministic rotation sweep: every movable part is tried
 * in every orientation it allows, and the best is kept. Random search finds the
 * basin, the sweep cleans it.
 */
#include "solver_internal.h"
#include "swap.h"

#include <math.h>
#include <string.h>

static void snapshot(solver_t *s, coord_t score)
{
    memcpy(s->best_x, s->x, (size_t)s->ncomp * sizeof(coord_t));
    memcpy(s->best_y, s->y, (size_t)s->ncomp * sizeof(coord_t));
    memcpy(s->best_orient, s->orient, (size_t)s->ncomp * sizeof(uint8_t));
    s->best_score = score;
    s->have_best = true;
}

static void restore(solver_t *s)
{
    memcpy(s->x, s->best_x, (size_t)s->ncomp * sizeof(coord_t));
    memcpy(s->y, s->best_y, (size_t)s->ncomp * sizeof(coord_t));
    memcpy(s->orient, s->best_orient, (size_t)s->ncomp * sizeof(uint8_t));
    solver_rebuild_extents(s);
}

/* A part may rotate unless the producer pinned it or the mask forbids it. */
static bool can_rotate(const solver_t *s, uint32_t comp)
{
    if (s->opt->respect_rot_fixed &&
        (s->ctx->comps.flags[comp] & COMP_ROT_FIXED) != 0u) {
        return false;
    }
    return s->opt->rotation_mask != 0u;
}

static uint32_t next_orientation(const solver_t *s, uint32_t comp, uint32_t step)
{
    const uint8_t current = s->orient[comp];
    for (uint32_t attempt = 1u; attempt <= 4u; ++attempt) {
        const uint32_t candidate = (current + attempt + step) & 3u;
        if ((s->opt->rotation_mask & (1u << candidate)) != 0u) {
            return candidate;
        }
    }
    return current;
}

void solver_refine(solver_t *s)
{
    if (s->opt->refine_moves == 0u || s->ncomp == 0u) {
        return;
    }

    /* --- the movable set: cluster masters and free parts ----------------- */
    s->nmovable = 0u;
    for (uint32_t i = 0u; i < s->ncomp; ++i) {
        if (solver_component_is_movable(s, i)) {
            s->movable[s->nmovable++] = i;
        }
    }
    s->stats.movable = s->nmovable;
    for (uint32_t q = 0u; q < 4u; ++q) {
        s->stats.accept_q[q] = 0u;
        s->stats.accept_down_q[q] = 0u;
        s->stats.accept_up_q[q] = 0u;
    }
    if (s->nmovable == 0u) {
        return;
    }

    solver_cost_t current = solver_evaluate(s);
    snapshot(s, current.score);

    /* Temperature relative to the starting score: the cost mixes millimetres
     * and counts, so an absolute value would not transfer between boards. */
    const coord_t t_start = (current.score > 0.0f)
                                ? current.score * s->opt->anneal_t_start_ratio
                                : 1.0f;
    const coord_t t_end = t_start * s->opt->anneal_t_end_ratio;
    /*
     * The temperature is the geometric law itself, evaluated from the step
     * index rather than accumulated:
     *
     *     T(k) = T_start * (T_end / T_start)^(k / N)
     *
     * Multiplying a float by a factor 0.9999931 a million times drifts, and a
     * walk whose temperature collapses early stops exploring while the budget
     * says otherwise. One powf per move costs microseconds against a walk that
     * runs for minutes.
     */
    const coord_t ratio = t_end / t_start;
    const coord_t inv_moves = 1.0f / (coord_t)s->opt->refine_moves;

    for (uint32_t move = 0u; move < s->opt->refine_moves; ++move) {
        const coord_t temperature = t_start * powf(ratio, (coord_t)move * inv_moves);
        const uint32_t comp = s->movable[solver_rand_below(s, s->nmovable)];
        /* The swap draw is short-circuited when the probability is zero, so a
         * run at --swap-prob 0 consumes the generator exactly as before and
         * reproduces the Gold reference bit for bit. */
        const bool want_swap = s->swap_bucket_count > 0u && s->opt->swap_prob > 0.0f &&
                               solver_rand_unit(s) < s->opt->swap_prob;
        const bool do_rotate =
            !want_swap && can_rotate(s, comp) && solver_rand_unit(s) < 0.45f;
        swap_move_t swap_move;
        swap_move.a = SOLVER_NO_INDEX;
        swap_move.b = SOLVER_NO_INDEX;

        coord_t saved_x = 0.0f;
        coord_t saved_y = 0.0f;
        uint32_t other = SOLVER_NO_INDEX;
        uint8_t saved_orient = s->orient[comp];

        if (do_rotate) {
            /* Most rotations are aimed: the torque heuristic names the pose
             * where the pins face their nets, which is where a crossing
             * disappears. A share stays random so the walk can still leave a
             * local minimum the heuristic cannot see. */
            const uint8_t candidate = (solver_rand_unit(s) < 0.6f)
                                          ? solver_torque_orientation(s, comp)
                                          : (uint8_t)next_orientation(s, comp,
                                                                      solver_rand_below(s, 3u));
            if (candidate == s->orient[comp]) {
                continue; /* already canonical */
            }
            if (!solver_rotation_fits(s, comp, candidate)) {
                continue; /* a block turned past the board edge is not a move */
            }
            s->orient[comp] = candidate;
            solver_rebuild_extents(s);
        } else {
            other = s->movable[solver_rand_below(s, s->nmovable)];
            if (other == comp) {
                continue;
            }
            /* A warm round is a local search: the partner has to be in the
             * neighbourhood, or the walk is exploring the board again instead
             * of the basin it was handed. */
            if (s->opt->refine_radius > 0.0f) {
                const coord_t rdx = s->x[comp] - s->x[other];
                const coord_t rdy = s->y[comp] - s->y[other];
                if (rdx * rdx + rdy * rdy >
                    s->opt->refine_radius * s->opt->refine_radius) {
                    continue;
                }
            }
            saved_x = s->x[comp];
            saved_y = s->y[comp];
            const coord_t ox = s->x[other];
            const coord_t oy = s->y[other];
            /* A swap moves both parts to each other's place; neither may end
             * up outside its own lane. Rejecting here, before the move, keeps
             * the annealer inside the boxes instead of letting it wander and
             * having the legaliser drag everything back at the end. */
            if (!solver_within_anchor(s, comp, ox, oy) ||
                !solver_within_anchor(s, other, saved_x, saved_y)) {
                s->stats.moves_walled += 1u;
                continue;
            }
            const uint32_t cl_a = s->cluster_of[comp];
            const uint32_t cl_b = s->cluster_of[other];
            if (cl_a != SOLVER_NO_INDEX) {
                solver_apply_cluster(s, cl_a, ox, oy);
            } else {
                s->x[comp] = ox;
                s->y[comp] = oy;
            }
            if (cl_b != SOLVER_NO_INDEX) {
                solver_apply_cluster(s, cl_b, saved_x, saved_y);
            } else {
                s->x[other] = saved_x;
                s->y[other] = saved_y;
            }
        }

        s->stats.moves_tried += 1u;
        const solver_cost_t candidate = solver_evaluate(s);
        const coord_t delta = candidate.score - current.score;
        /* The quench: the last stretch of the walk takes improvements only, so
         * it settles into the basin the exploration found instead of leaving
         * the moment the budget ends. */
        const bool quenching = (move + s->opt->quench_moves >= s->opt->refine_moves);
        const bool accept =
            (delta <= 0.0f) ||
            (!quenching && solver_rand_unit(s) < expf(-delta / temperature));
        if (accept) {
            current = candidate;
            s->stats.moves_accepted += 1u;
            s->stats.accept_q[(move * 4u) / s->opt->refine_moves] += 1u;
            if (delta <= 0.0f) {
                s->stats.accept_down_q[(move * 4u) / s->opt->refine_moves] += 1u;
            } else {
                s->stats.accept_up_q[(move * 4u) / s->opt->refine_moves] += 1u;
            }
            if (candidate.score < s->best_score) {
                snapshot(s, candidate.score);
            }
        } else if (swap_move.a != SOLVER_NO_INDEX) {
            swap_undo(s, &swap_move);
            s->stats.swaps_applied -= 1u;
        } else if (do_rotate) {
            s->orient[comp] = saved_orient;
            solver_rebuild_extents(s);
        } else {
            /* put both back where they were */
            const uint32_t cl_a = s->cluster_of[comp];
            const uint32_t cl_b = s->cluster_of[other];
            const coord_t bx = s->x[comp];
            const coord_t by = s->y[comp];
            if (cl_b != SOLVER_NO_INDEX) {
                solver_apply_cluster(s, cl_b, bx, by);
            } else {
                s->x[other] = bx;
                s->y[other] = by;
            }
            if (cl_a != SOLVER_NO_INDEX) {
                solver_apply_cluster(s, cl_a, saved_x, saved_y);
            } else {
                s->x[comp] = saved_x;
                s->y[comp] = saved_y;
            }
        }
    }

    if (s->have_best) {
        restore(s);
    }

    /* --- deterministic rotation sweep ------------------------------------ */
    for (uint32_t k = 0u; k < s->nmovable; ++k) {
        const uint32_t comp = s->movable[k];
        if (!can_rotate(s, comp)) {
            continue;
        }
        const uint8_t original = s->orient[comp];
        const uint8_t canonical = solver_torque_orientation(s, comp);
        coord_t best_score = solver_evaluate(s).score;
        uint8_t best_orient = original;
        /* the canonical pose is tried first: when it wins the remaining three
         * evaluations are not worth their clock cycles */
        for (uint32_t step = 0u; step < 4u; ++step) {
            const uint8_t o = (step == 0u) ? canonical : (uint8_t)step;
            if ((s->opt->rotation_mask & (1u << o)) == 0u || o == original ||
                (step != 0u && o == canonical)) {
                continue;
            }
            if (!solver_rotation_fits(s, comp, o)) {
                continue; /* it would not fit the board in that pose */
            }
            s->orient[comp] = o;
            solver_rebuild_extents(s);
            const coord_t score = solver_evaluate(s).score;
            if (score < best_score) {
                best_score = score;
                best_orient = o;
                if (o == canonical) {
                    break; /* the heuristic's answer already pays off */
                }
            }
        }
        s->orient[comp] = best_orient;
        solver_rebuild_extents(s);
    }
}
