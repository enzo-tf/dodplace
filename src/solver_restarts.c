/*
 * solver_restarts.c - see solver_restarts.h.
 */
#include "solver_restarts.h"
#include "spatial_grid.h"
#include "solver_state.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const placer_context_t *ctx;
    const solver_options_t *opt;
    const solver_t         *start;
    uint32_t                restart;
    uint32_t                ncomp;
    coord_t                *x;
    coord_t                *y;
    uint8_t                *orient;
    coord_t                 score;
    coord_t                 hpwl;
    /* The refine counters of this walk: just the ones the stage owns, so the
     * caller keeps the before/after numbers it measured itself. */
    uint32_t                movable;
    uint32_t                moves_tried;
    uint32_t                moves_accepted;
    uint32_t                moves_walled;
    uint32_t                swaps_applied;
    bool                    ok;
} restart_job_t;

static void *restart_worker(void *arg)
{
    restart_job_t *job = arg;
    arena_t arena;
    if (!arena_init(&arena, job->ctx->scratch.capacity)) {
        if (getenv("DODPLACE_RESTART_DEBUG") != nullptr) {
            (void)fprintf(stderr, "restart %u: arena_init(%zu) failed\n", job->restart,
                          job->ctx->scratch.capacity);
        }
        job->ok = false;
        return nullptr;
    }
    solver_t t;
    memset(&t, 0, sizeof t);
    bool ok = solver_state_init_in(&t, job->ctx, job->opt, &arena);
    if (ok) {
        /* The spatial grids are the caller's job in the serial path; a worker
         * owns its own, they are what the cost function reads. */
        grid_init(&t, &t.comp_grid, job->opt->crossing_cell * 0.5f, t.ncomp,
                  t.ncomp * 16u, GRID_CONFLICT_MAX_PER_CELL);
        grid_init(&t, &t.seg_grid, job->opt->crossing_cell,
                  (t.npin == 0u) ? 1u : t.npin,
                  ((t.npin == 0u) ? 1u : t.npin) * GRID_MAX_CELLS_PER_ITEM,
                  GRID_MAX_ITEMS_PER_CELL);
        ok = t.ok;
    }
    if (!ok && getenv("DODPLACE_RESTART_DEBUG") != nullptr) {
        (void)fprintf(stderr, "restart %u: state/grid init failed (used %zu of %zu)\n",
                      job->restart, arena.used, arena.capacity);
    }
    if (ok) {
        /* Start where the pipeline reached, not where the scene began: the
         * relaxation and the matching moved the parts before the anneal. */
        memcpy(t.x, job->start->x, (size_t)job->ncomp * sizeof(coord_t));
        memcpy(t.y, job->start->y, (size_t)job->ncomp * sizeof(coord_t));
        memcpy(t.orient, job->start->orient, (size_t)job->ncomp * sizeof(uint8_t));
        solver_rebuild_extents(&t);
        t.rng = ((job->opt->seed != 0u) ? job->opt->seed : 1u) + job->restart * 0x9E3779B9u;
        if (t.rng == 0u) {
            t.rng = 1u;
        }
        solver_refine(&t);
        const solver_cost_t cost = solver_evaluate(&t);
        job->score = cost.score;
        job->hpwl = cost.hpwl;
        job->movable = t.stats.movable;
        job->moves_tried = t.stats.moves_tried;
        job->moves_accepted = t.stats.moves_accepted;
        job->moves_walled = t.stats.moves_walled;
        job->swaps_applied = t.stats.swaps_applied;
        memcpy(job->x, t.x, (size_t)job->ncomp * sizeof(coord_t));
        memcpy(job->y, t.y, (size_t)job->ncomp * sizeof(coord_t));
        memcpy(job->orient, t.orient, (size_t)job->ncomp * sizeof(uint8_t));
        ok = t.ok;
    }
    arena_destroy(&arena);
    job->ok = ok;
    return nullptr;
}

bool solver_restarts_parallel(solver_t *s, uint32_t restarts, uint32_t jobs)
{
    const uint32_t n = s->ncomp;
    if (jobs < 2u || restarts < 2u) {
        return false;
    }
    if (jobs > restarts) {
        jobs = restarts;
    }
    coord_t *x = SOLVER_ALLOC(s, coord_t, (size_t)jobs * n);
    coord_t *y = SOLVER_ALLOC(s, coord_t, (size_t)jobs * n);
    uint8_t *orient = SOLVER_ALLOC(s, uint8_t, (size_t)jobs * n);
    restart_job_t *slots = SOLVER_ALLOC(s, restart_job_t, jobs);
    pthread_t *threads = SOLVER_ALLOC(s, pthread_t, jobs);
    coord_t *win_x = SOLVER_ALLOC(s, coord_t, n);
    coord_t *win_y = SOLVER_ALLOC(s, coord_t, n);
    uint8_t *win_orient = SOLVER_ALLOC(s, uint8_t, n);
    if (x == nullptr || y == nullptr || orient == nullptr || slots == nullptr ||
        threads == nullptr || win_x == nullptr || win_y == nullptr || win_orient == nullptr) {
        if (getenv("DODPLACE_RESTART_DEBUG") != nullptr) {
            (void)fprintf(stderr,
                          "restart setup failed: used %zu of %zu (x=%p y=%p o=%p slots=%p)\n",
                          s->scratch->used, s->scratch->capacity, (void *)x, (void *)y,
                          (void *)orient, (void *)slots);
        }
        return false;
    }
    bool have_best = false;
    coord_t best_score = 0.0f;
    coord_t best_hpwl = 0.0f;
    uint32_t win_movable = 0u;
    uint32_t win_moves_tried = 0u;
    uint32_t win_moves_accepted = 0u;
    uint32_t win_moves_walled = 0u;
    uint32_t win_swaps_applied = 0u;
    const uint32_t total = restarts;
    for (uint32_t first = 0u; first < total; first += jobs) {
        const uint32_t batch = ((total - first) < jobs) ? (total - first) : jobs;
        for (uint32_t k = 0u; k < batch; ++k) {
            restart_job_t *job = &slots[k];
            job->ctx = s->ctx;
            job->opt = s->opt;
            job->start = s;
            job->restart = first + k;
            job->ncomp = n;
            job->x = x + (size_t)k * n;
            job->y = y + (size_t)k * n;
            job->orient = orient + (size_t)k * n;
            job->score = 0.0f;
            job->hpwl = 0.0f;
            job->ok = false;
            const int rc = pthread_create(&threads[k], nullptr, restart_worker, job);
            if (rc != 0) {
                (void)fprintf(stderr, "solver: cannot start worker %u (%d)\n", k, rc);
                slots[k].ok = false;
                threads[k] = pthread_self();
            }
        }
        for (uint32_t k = 0u; k < batch; ++k) {
            if (pthread_join(threads[k], nullptr) != 0 || !slots[k].ok) {
                if (getenv("DODPLACE_RESTART_DEBUG") != nullptr) {
                    (void)fprintf(stderr, "restart %u: join/ok failed (ok=%d)\n",
                                  first + k, (int)slots[k].ok);
                }
                continue;
            }
            /* Wirelength first, the way the detailed stage chooses between its
             * chains: the placement contract is written in millimetres, and the
             * score breaks a tie. */
            const bool better =
                !have_best || (slots[k].hpwl < best_hpwl - 0.1f) ||
                (fabsf(slots[k].hpwl - best_hpwl) <= 0.1f && slots[k].score < best_score);
            if (better) {
                have_best = true;
                best_score = slots[k].score;
                best_hpwl = slots[k].hpwl;
                win_movable = slots[k].movable;
                win_moves_tried = slots[k].moves_tried;
                win_moves_accepted = slots[k].moves_accepted;
                win_moves_walled = slots[k].moves_walled;
                win_swaps_applied = slots[k].swaps_applied;
                memcpy(win_x, slots[k].x, (size_t)n * sizeof(coord_t));
                memcpy(win_y, slots[k].y, (size_t)n * sizeof(coord_t));
                memcpy(win_orient, slots[k].orient, (size_t)n * sizeof(uint8_t));
            }
        }
    }
    if (!have_best) {
        if (getenv("DODPLACE_RESTART_DEBUG") != nullptr) {
            (void)fprintf(stderr, "parallel: no worker produced a placement\n");
        }
        return false;
    }
    /* `s` is the workers' starting pose, so it is only written once they are
     * all joined: the ordering is by score, ties going to the earlier walk. */
    memcpy(s->x, win_x, (size_t)n * sizeof(coord_t));
    memcpy(s->y, win_y, (size_t)n * sizeof(coord_t));
    memcpy(s->orient, win_orient, (size_t)n * sizeof(uint8_t));
    solver_rebuild_extents(s);
    s->stats.movable = win_movable;
    s->stats.moves_tried = win_moves_tried;
    s->stats.moves_accepted = win_moves_accepted;
    s->stats.moves_walled = win_moves_walled;
    s->stats.swaps_applied = win_swaps_applied;
    return s->ok;
}
