/*
 * swap_detailed.c - the detailed stage, as a choice between two chains.
 *
 * Both passes are monotone in the engine's score: the assignment keeps a
 * bucket's permutation only when the score fell, and the windows keep their
 * sweep only when it fell. Chained, they are not - the assignment lands the
 * placement in a different basin and the windows then settle somewhere else,
 * which on r10's third seed was 17 points worse than the windows alone. So the
 * stage runs both chains from the annealed placement and keeps the better one.
 *
 * "Better" is the wirelength, because that is the metric the placement contract
 * is written in, and a permutation is exactly the move that trades wirelength
 * against the crossing proxy. The score breaks a tie, and the per-bucket gate
 * inside the assignment still uses the score, so no bucket is ever rearranged
 * into something the engine's own cost calls worse.
 */
#include "swap_detailed.h"
#include "solver_best.h"
#include "swap_assign.h"
#include "swap_window.h"

void swap_detailed_stage(solver_t *s)
{
    const uint32_t window = s->opt->swap_window;
    const uint32_t assign = s->opt->swap_assign_min;
    if (window < 2u && assign < 2u) {
        return;
    }
    solver_best_t anchor;
    if (!solver_best_init(s, &anchor)) {
        return;
    }
    solver_best_take(s, &anchor); /* the placement the annealer left */

    if (assign < 2u) {
        (void)swap_window_pass(s, window);
        return;
    }
    (void)swap_assign_pass(s, assign);
    if (window >= 2u) {
        (void)swap_window_pass(s, window);
    }
    solver_best_t chained;
    if (!solver_best_init(s, &chained)) {
        return;
    }
    solver_best_take(s, &chained);
    /*
     * A second round, because the first moved the nets' centroids: the matrix
     * the second assignment solves is not the one the first saw. Its passes are
     * gated on the score while this round is judged on wirelength, so the round
     * is kept only if it did not give wirelength back.
     */
    if (window >= 2u) {
        solver_best_t round1_pose;
        if (solver_best_init(s, &round1_pose)) {
            solver_best_take(s, &round1_pose);
            const bool assigned = swap_assign_pass(s, assign);
            const bool windowed = swap_window_pass(s, window);
            if ((assigned || windowed) &&
                solver_evaluate(s).hpwl > round1_pose.cost.hpwl + 0.1f) {
                solver_best_restore(s, &round1_pose);
            }
        }
    }

    solver_best_restore(s, &anchor);
    solver_best_take(s, &anchor); /* the anchor pose, freshly scored */
    if (window < 2u) {
        solver_best_restore(s, &anchor);
        if (chained.cost.score < anchor.cost.score) {
            solver_best_restore(s, &chained);
        }
        return;
    }
    (void)swap_window_pass(s, window);
    const solver_cost_t plain = solver_evaluate(s);
    const bool chained_better =
        (chained.cost.hpwl < plain.hpwl - 0.1f) ||
        (fabsf(chained.cost.hpwl - plain.hpwl) <= 0.1f && chained.cost.score < plain.score);
    if (chained_better) {
        solver_best_restore(s, &chained);
    }
}
