/*
 * solver_core.c - the pipeline: cluster, place, refine, legalise, write.
 *
 * Every stage reads and writes the same working placement; a stage that fails
 * sets `ok` and the run unwinds without touching the scene.
 */
#include "solver_internal.h"
#include "matching.h"
#include "ratsnest.h"
#include "spatial_grid.h"
#include "solver_best.h"
#include "solver_restarts.h"
#include "solver_state.h"
#include "swap_detailed.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t now_ns(void)
{
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* The arena ran out: report how much was needed, then unwind. */
static bool out_of_scratch(placer_context_t *ctx, arena_mark_t mark, solver_stats_t *stats)
{
    if (stats != nullptr) {
        memset(stats, 0, sizeof *stats);
        stats->scratch_needed = ctx->scratch.used;
    }
    (void)fprintf(stderr,
                  "solver: the scratch arena is too small (%zu bytes used, %zu "
                  "available); the scene was built with a smaller estimate\n",
                  ctx->scratch.used, ctx->scratch.capacity);
    arena_rewind(&ctx->scratch, mark);
    return false;
}

solver_cost_t solver_evaluate(solver_t *s)
{
    solver_cost_t cost;
    uint64_t t0 = now_ns();
    cost.hpwl = solver_hpwl(s);
    uint64_t t1 = now_ns();
    if (s->opt->w_crossings > 0.0f) {
        ratsnest_build(s); /* the crossing count reads its segments */
        uint64_t t2 = now_ns();
        cost.crossings = count_crossings(s);
        uint64_t t3 = now_ns();
        s->profile_hpwl_ns += t1 - t0;
        s->profile_ratsnest_ns += t2 - t1;
        s->profile_crossings_ns += t3 - t2;
        t0 = t3;
    } else {
        cost.crossings = 0u; /* the caller does not pay for what it does not weigh */
    }
    cost.overlaps = count_conflicts(s, s->opt->courtyard_clearance, nullptr, nullptr);
    cost.overlap_depth = solver_overlap_depth(s, s->opt->courtyard_clearance);
    cost.displacement = (s->opt->w_displacement > 0.0f) ? solver_displacement(s) : 0.0f;
    cost.thermal = solver_thermal_cost(s);
    cost.decoupling = solver_decoupling_cost(s);
    cost.diffpair = solver_diffpair_cost(s);
    s->profile_overlaps_ns += now_ns() - t0;
    s->profile_calls += 1u;
    cost.score = s->opt->w_hpwl * cost.hpwl +
                 s->opt->w_crossings * (coord_t)cost.crossings +
                 s->opt->w_overlaps * (coord_t)cost.overlaps +
                 s->opt->w_overlap_depth * cost.overlap_depth +
                 s->opt->w_displacement * cost.displacement +
                 s->opt->w_thermal * cost.thermal +
                 s->opt->w_decoupling * cost.decoupling +
                 s->opt->w_diffpair * cost.diffpair;
    return cost;
}

/* ========================================================================= */
/* State setup                                                               */
/* ========================================================================= */


/* Pin offsets pre-rotated for the four orientations. */
static void write_placement(const solver_t *s, const placer_context_t *ctx,
                            placement_entry_t *out, uint32_t capacity)
{
    for (uint32_t i = 0u; i < s->ncomp && i < capacity; ++i) {
        out[i].x = s->x[i];
        out[i].y = s->y[i];
        out[i].orient = s->orient[i];
        out[i].side = s->side[i];
        out[i].reserved = 0u;
        uint8_t flags = 0u;
        if (comp_is_locked(ctx->comps.flags[i])) {
            flags |= PLACEMENT_FLAG_LOCKED;
        }
        /* "moved" means we changed something, at any stage */
        if (s->x[i] != ctx->comps.x[i] || s->y[i] != ctx->comps.y[i] ||
            s->orient[i] != (uint8_t)comp_orientation(ctx->comps.flags[i])) {
            flags |= PLACEMENT_FLAG_MOVED;
        }
        out[i].flags = flags;
    }
}

bool placer_solve(const placer_context_t *ctx, const solver_options_t *opt,
                  placement_entry_t *out, uint32_t capacity, solver_stats_t *stats)
{
    if (ctx == nullptr || out == nullptr || !ctx->ingest.finalized) {
        return false;
    }
    if (ctx->comps.count == 0u || capacity < ctx->comps.count) {
        return false;
    }

    solver_options_t defaults;
    if (opt == nullptr) {
        solver_options_defaults(&defaults);
        opt = &defaults;
    }

    /* The scene is read-only; only the scratch arena is touched. */
    arena_mark_t mark = arena_mark(&((placer_context_t *)ctx)->scratch);
    const uint64_t t_solve0 = now_ns();

    solver_t s;
    memset(&s, 0, sizeof s);
    /* The options are copied so a chained round can tighten its own start
     * temperature and neighbour radius without touching the caller's struct. */
    solver_options_t tuned;
    if (opt != nullptr) {
        tuned = *opt;
    } else {
        solver_options_defaults(&tuned);
    }
    opt = &tuned;
    if (!solver_state_init(&s, ctx, opt)) {
        return out_of_scratch((placer_context_t *)ctx, mark, stats);
    }

    /* Grids are sized once from the board extents, then rebuilt in place. */
    grid_init(&s, &s.comp_grid, s.opt->crossing_cell * 0.5f, s.ncomp, s.ncomp * 16u,
              GRID_CONFLICT_MAX_PER_CELL);
    grid_init(&s, &s.seg_grid, s.opt->crossing_cell, (s.npin == 0u) ? 1u : s.npin,
              ((s.npin == 0u) ? 1u : s.npin) * GRID_MAX_CELLS_PER_ITEM,
              GRID_MAX_ITEMS_PER_CELL);
    if (!s.ok) {
        if (stats != nullptr) {
            memset(stats, 0, sizeof *stats);
            stats->scratch_needed = ((placer_context_t *)ctx)->scratch.used;
        }
        arena_rewind(&((placer_context_t *)ctx)->scratch, mark);
        return false;
    }

    const uint64_t t_cluster0 = now_ns();
    if (opt->enable_clustering) {
        solver_cluster(&s);
    }
    solver_sync_cluster_members(&s);
    solver_rebuild_extents(&s);
    const uint64_t t_cluster1 = now_ns();

    {
        solver_cost_t start = solver_evaluate(&s);
        s.stats.hpwl_before = start.hpwl;
        s.stats.crossings_before = start.crossings;
        s.stats.overlaps_before = start.overlaps;
        /* without the clearance, to separate what the designer left behind from
         * what the solver failed to clean up */
        s.stats.unplaced_before = solver_count_conflicts(&s, 0.0f);
        s.stats.locked_overlaps = solver_count_locked_conflicts(&s, 0.0f);
    }

    /*
     * The incumbent. The input is a placement too - usually the one a human
     * signed off - so legalising it as it stands costs one pass and gives the
     * pipeline something to beat. The pipeline then runs from the input again
     * and is adopted only if it scores better (solver_best.c).
     */
    const bool transformed = opt->enable_global || opt->enable_refine;
    solver_best_t incumbent;
    if (transformed && !solver_best_init(&s, &incumbent)) {
        return out_of_scratch((placer_context_t *)ctx, mark, stats);
    }
    /*
     * Every stage below allocates its working tables from the same arena, and
     * nothing it allocates outlives the call. Rewinding to this mark before
     * each one keeps a stage repeatable: the incumbent is legalised here and
     * the pipeline legalises its own result later, and without the rewind the
     * second legalisation found no room, did nothing, and left the caller
     * reading the first call's statistics on a layout it had not touched.
     */
    const arena_mark_t stage_mark = arena_mark(&((placer_context_t *)ctx)->scratch);

    const uint64_t t_incumbent0 = now_ns();
    if (transformed) {
        if (opt->enable_matching) {
            arena_rewind(&((placer_context_t *)ctx)->scratch, stage_mark);
            (void)matching_assign_decoupling(&s);
        }
        /* The discrete assignment belongs to the incumbent: choosing which
         * capacitor fills which slot changes no rectangle, so it is the one
         * stage that can improve a layout the designer already signed off. */
        if (opt->enable_legalize) {
            arena_rewind(&((placer_context_t *)ctx)->scratch, stage_mark);
            solver_legalize(&s);
        }
        solver_best_take(&s, &incumbent);
        solver_load_input_pose(&s);
        solver_sync_cluster_members(&s);
        solver_rebuild_extents(&s);
    }
    const uint64_t t_incumbent1 = now_ns();

    const uint64_t t_global0 = t_incumbent1;
    if (opt->enable_global) {
        arena_rewind(&((placer_context_t *)ctx)->scratch, stage_mark);
        /* The relaxation has to pay for itself. It is a redraw, and on a board
         * that arrives well placed - r10 is one - it can end up worse than the
         * pose it started from: measured, 31.7 m of wirelength became 41.5 m
         * before the anchors were clamped, and the stages that follow inherit
         * whatever it produced. So its result is scored against the pose it
         * started from, and a relaxation that does not win is rolled back. */
        const solver_cost_t anchored = solver_evaluate(&s);
        solver_global(&s);
        /* The relaxation is continuous and knows nothing of lanes; bring it
         * back inside them before anything else looks at the layout. */
        for (uint32_t i = 0u; i < s.ncomp; ++i) {
            if (solver_component_is_movable(&s, i)) {
                solver_clamp_to_anchor(&s, i);
            }
        }
        if (solver_evaluate(&s).score >= anchored.score) {
            solver_load_input_pose(&s);
            solver_sync_cluster_members(&s);
            solver_rebuild_extents(&s);
        }
    }
    if (opt->enable_matching) {
        arena_rewind(&((placer_context_t *)ctx)->scratch, stage_mark);
        (void)matching_assign_decoupling(&s);
    }
    const uint64_t t_global1 = now_ns();
    if (opt->enable_refine) {
        /*
         * Multi-start. The annealer is a stochastic walk whose outcome swings
         * by hundreds of millimetres between seeds - measured 29 818.9 against
         * 30 398.1 on the same board - so the answer is to walk it several
         * times from the same starting pose and keep the best, not to tune one
         * walk. `refine_restarts` 1 reproduces the single-walk reference bit
         * for bit: the first restart uses the seed itself.
         */
        const uint32_t restarts = (opt->refine_restarts > 0u) ? opt->refine_restarts : 1u;
        const uint32_t rounds = (tuned.chain_rounds > 0u) ? tuned.chain_rounds : 1u;
        const coord_t base_t_start = tuned.anneal_t_start_ratio;
        solver_best_t start;
        solver_best_t best;
        if (!solver_best_init(&s, &start) || !solver_best_init(&s, &best)) {
            return out_of_scratch((placer_context_t *)ctx, mark, stats);
        }
        solver_best_take(&s, &start);
        /* The restarts' bookkeeping - one row of poses per worker - is done
         * with once the winner is in `s`, and the detailed stage that follows
         * needs the room back: on r10 the parallel path left the arena three
         * hundred kilobytes tighter and the Hungarian matrices did not fit. */
        const arena_mark_t detail_mark = arena_mark(&((placer_context_t *)ctx)->scratch);
        for (uint32_t round = 0u; round < rounds; ++round) {
        if (round > 0u) {
            /* Chained annealing: the next round starts from the placement the
             * last one settled on, with a colder start and a tightened
             * neighbourhood - the basin is known, only its bottom is not. */
            tuned.anneal_t_start_ratio =
                base_t_start * powf(tuned.chain_decay, (coord_t)round);
            const coord_t base_radius =
                (tuned.refine_radius > 0.0f) ? tuned.refine_radius : 8.0f;
            tuned.refine_radius = base_radius * powf(tuned.chain_decay, (coord_t)round);
            solver_best_take(&s, &start);
            solver_best_take(&s, &best); /* this round keeps its own winner */
        }
        /* Two or more workers: one arena each, nothing shared but the scene,
         * and the same ordering of the walks. */
        if (opt->jobs >= 2u && restarts >= 2u) {
            if (!solver_restarts_parallel(&s, restarts, opt->jobs)) {
                return out_of_scratch((placer_context_t *)ctx, mark, stats);
            }
        } else {
            const uint32_t seed0 = (opt->seed != 0u) ? opt->seed : 1u;
            for (uint32_t restart = 0u; restart < restarts; ++restart) {
                if (restart > 0u) {
                    s.rng = seed0 + restart * 0x9E3779B9u;
                    if (s.rng == 0u) {
                        s.rng = 1u;
                    }
                    solver_best_restore(&s, &start);
                }
                solver_refine(&s);
                const solver_cost_t cost = solver_evaluate(&s);
                /* Wirelength first, the score breaking a tie: same rule as the
                 * workers and as the detailed stage. */
                if (restart == 0u || cost.hpwl < best.cost.hpwl - 0.1f ||
                    (fabsf(cost.hpwl - best.cost.hpwl) <= 0.1f &&
                     cost.score < best.cost.score)) {
                    solver_best_take(&s, &best);
                }
            }
            solver_best_restore(&s, &best);
        }
        }
        arena_rewind(&((placer_context_t *)ctx)->scratch, detail_mark);
        swap_detailed_stage(&s);
    }
    const uint64_t t_refine1 = now_ns();
    if (opt->enable_legalize) {
        arena_rewind(&((placer_context_t *)ctx)->scratch, stage_mark);
        solver_legalize(&s);
    }
    if (transformed) {
        (void)solver_best_adopt(&s, &incumbent);
    }
    if (!s.ok) {
        return out_of_scratch((placer_context_t *)ctx, mark, stats);
    }
    const uint64_t t_legal1 = now_ns();

    const double ns_to_s = 1e-9;
    s.stats.seconds_cluster = (coord_t)((double)(t_cluster1 - t_cluster0) * ns_to_s);
    s.stats.seconds_incumbent = (coord_t)((double)(t_incumbent1 - t_incumbent0) * ns_to_s);
    s.stats.seconds_global = (coord_t)((double)(t_global1 - t_global0) * ns_to_s);
    s.stats.seconds_refine = (coord_t)((double)(t_refine1 - t_global1) * ns_to_s);
    s.stats.seconds_legalize = (coord_t)((double)(t_legal1 - t_refine1) * ns_to_s);
    s.stats.seconds_evaluate = (coord_t)((double)(s.profile_hpwl_ns + s.profile_ratsnest_ns +
                                                  s.profile_crossings_ns +
                                                  s.profile_overlaps_ns) *
                                         ns_to_s);

    {
        solver_cost_t end = solver_evaluate(&s);
        s.stats.hpwl_after = end.hpwl;
        s.stats.crossings_after = end.crossings;
        s.stats.overlaps_after = end.overlaps;
    }

    if (s.profile && s.profile_calls > 0u) {
        const double calls = (double)s.profile_calls;
        (void)fprintf(stderr,
                      "solver profile, %u evaluations:\n"
                      "  hpwl       %6.2f ms total, %6.3f ms each\n"
                      "  ratsnest   %6.2f ms total, %6.3f ms each\n"
                      "  crossings  %6.2f ms total, %6.3f ms each\n"
                      "  overlaps   %6.2f ms total, %6.3f ms each\n",
                      (unsigned)s.profile_calls, (double)s.profile_hpwl_ns / 1e6,
                      (double)s.profile_hpwl_ns / 1e6 / calls,
                      (double)s.profile_ratsnest_ns / 1e6,
                      (double)s.profile_ratsnest_ns / 1e6 / calls,
                      (double)s.profile_crossings_ns / 1e6,
                      (double)s.profile_crossings_ns / 1e6 / calls,
                      (double)s.profile_overlaps_ns / 1e6,
                      (double)s.profile_overlaps_ns / 1e6 / calls);
    }
    if (getenv("DODPLACE_DUMP_SHAPE") != nullptr) {
        /* The exact values the engine used, for comparing with KiCad's view of
         * the same parts. DODPLACE_DUMP_SHAPE=1 dumps a fixed suspect list;
         * any other value is a comma-separated list of component indices. */
        const char *arg = getenv("DODPLACE_DUMP_SHAPE");
        uint32_t ids[32];
        uint32_t n_ids = 0u;
        if (strcmp(arg, "1") == 0) {
            const uint32_t preset[] = {520u, 643u, 540u, 686u, 570u,
                                       647u, 595u, 677u, 623u, 662u};
            for (uint32_t k = 0u; k < sizeof preset / sizeof preset[0]; ++k) {
                ids[n_ids++] = preset[k];
            }
        } else {
            const char *p = arg;
            while (*p != '\0' && n_ids < 32u) {
                ids[n_ids++] = (uint32_t)strtoul(p, (char **)&p, 10);
                if (*p == ',') {
                    p += 1;
                }
            }
        }
        const polygon_pool_t *pool = &ctx->constraints.polygons;
        for (uint32_t k = 0u; k < n_ids; ++k) {
            const uint32_t i = ids[k];
            if (i >= s.ncomp) {
                continue;
            }
            const place_id_t poly = ctx->comps.polygon_id[i];
            coord_t lo_x = 0.0f;
            coord_t hi_x = 0.0f;
            coord_t lo_y = 0.0f;
            coord_t hi_y = 0.0f;
            if (poly != PLACE_ID_NONE && poly < pool->num_polys) {
                const uint32_t b = pool->poly_offsets[poly];
                const uint32_t e = pool->poly_offsets[poly + 1u];
                lo_x = hi_x = pool->vx[b];
                lo_y = hi_y = pool->vy[b];
                for (uint32_t v = b + 1u; v < e; ++v) {
                    lo_x = (pool->vx[v] < lo_x) ? pool->vx[v] : lo_x;
                    hi_x = (pool->vx[v] > hi_x) ? pool->vx[v] : hi_x;
                    lo_y = (pool->vy[v] < lo_y) ? pool->vy[v] : lo_y;
                    hi_y = (pool->vy[v] > hi_y) ? pool->vy[v] : hi_y;
                }
            }
            (void)fprintf(stderr,
                          "shape %u: pos (%.4f, %.4f) half (%.4f, %.4f) orient %u side %u | "
                          "poly local [%.4f, %.4f] x [%.4f, %.4f] centre (%.4f, %.4f)\n",
                          i, (double)s.x[i], (double)s.y[i], (double)s.half_w[i],
                          (double)s.half_h[i], (unsigned)s.orient[i], (unsigned)s.side[i],
                          (double)lo_x, (double)hi_x, (double)lo_y, (double)hi_y,
                          (double)((lo_x + hi_x) * 0.5f), (double)((lo_y + hi_y) * 0.5f));
        }
    }
    write_placement(&s, ctx, out, capacity);
    s.stats.seconds = (coord_t)((double)(now_ns() - t_solve0) * 1e-9);
    s.stats.scratch_peak = ((placer_context_t *)ctx)->scratch.used;
    if (stats != nullptr) {
        *stats = s.stats;
    }
    arena_rewind(&((placer_context_t *)ctx)->scratch, mark);
    return true;
}
