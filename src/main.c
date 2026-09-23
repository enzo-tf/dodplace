/*
 * dodplace-solve - command line front end of the dodplace engine.
 *
 * Phase 1 scope: load a scene.bin, finalize it, validate it and print a
 * report. `--identity` emits a placement.bin that keeps every ingested
 * position, which exercises the complete IR pipeline (Python -> C -> Python)
 * before the solver exists.
 */
#include "place/context.h"
#include "place/io.h"
#include "place/solver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const struct {
    uint32_t    bit;
    const char *name;
} k_degraded_names[] = {
    {DEGRADED_NO_3D_HEIGHT, "height-unknown"},
    {DEGRADED_NO_MASS, "mass-unknown"},
    {DEGRADED_NO_ROTATION_DATA, "rotation-data-unknown"},
    {DEGRADED_NO_PIN_ELEC, "pin-role-inferred"},
    {DEGRADED_NO_PIN_CURRENT, "pin-current-unknown"},
    {DEGRADED_NO_NET_WEIGHTS, "net-weights-uniform"},
    {DEGRADED_NO_NET_CLASSES, "net-classes-default"},
    {DEGRADED_NO_DIFFPAIRS, "no-differential-pairs"},
    {DEGRADED_NO_KEEPOUTS, "no-keepouts"},
    {DEGRADED_NO_STACKUP, "stackup-unknown"},
    {DEGRADED_NO_CLEARANCE_MATRIX, "clearance-matrix-absent"},
    {DEGRADED_NO_DECOUPLING, "no-decoupling-data"},
    {DEGRADED_NO_THERMAL, "no-thermal-data"},
    {DEGRADED_NO_SYMMETRY, "no-symmetry-data"},
    {DEGRADED_NO_AIRFLOW, "airflow-unknown"},
    {DEGRADED_NO_CEILING, "ceiling-unknown"},
    {DEGRADED_NO_VIA_RESTRICTION, "no-via-restriction"},
    {DEGRADED_COURTYARD_BBOX, "courtyard-from-pad-bbox"},
    {DEGRADED_BOARD_OUTLINE_BBOX, "outline-from-components-bbox"},
    {DEGRADED_NO_MAX_LENGTH, "no-length-bound"},
    {DEGRADED_NO_SEGREGATION, "no-floorplan-segregation"},
};

static void print_usage(FILE *out, const char *argv0)
{
    (void)fprintf(out,
                  "usage: %s [options] <scene.bin>\n"
                  "\n"
                  "strategies (one is required with -o):\n"
                  "      --identity     keep the ingested positions (pipeline smoke test)\n"
                  "      --solve        place the board (clustering, force-directed,\n"
                  "                     annealing, legalisation)\n"
                  "\n"
                  "solver options:\n"
                  "      --seed N           random seed, default 1\n"
                  "      --global-model M   repulsion (pairwise 1/d^2, default) or\n"
                  "                         analytic (ePlace density field)\n"
                  "      --w-density F      density force, relative to the net pull (1.0)\n"
                  "      --momentum F       Nesterov look-ahead for the analytic model (0.9)\n"
                  "      --density-bins N   bins per axis of the density grid (64)\n"
                  "      --iterations N     force-directed steps, default 400\n"
                  "      --moves N          annealing moves per walk, default 1000000\n"
                  "      --no-cluster       skip semantic clustering\n"
                  "      --no-matching      skip the discrete decoupling assignment\n"
                  "      --swap-prob F      share of moves that exchange identical parts (0.15)\n"
                  "      --swap-max-pins N  only parts with at most this many pins (6)\n"
                  "      --swap-window N    parts per exact permutation window (5, 0 = off)\n"
                  "      --swap-assign N    smallest bucket solved by assignment (6, 0 = off)\n"
                  "      --restarts N       independent annealing walks, best kept (1)\n"
                  "      --jobs N           walks run at once, one arena each (1)\n"
                  "      --pad-clearance MM copper-to-copper margin (default 0.5)\n"
                  "      --polish-reach MM  mask polish band above that margin (default 0.2)\n"
                  "      --w-crossings F    weight of the ratsnest crossing term (default 5.0)\n"
                  "      --anneal-t-start-ratio F  start temperature as a share of the score\n"
                  "      --anneal-t-end-ratio F    end temperature, share of the start\n"
                  "      --max-net-degree N rails have at least this many pins (default 15)\n"
                  "      --exclude-plane-nets  skip ground/power rails in HPWL and RUDY\n"
                  "      --no-anchor        let parts drift: no recall towards the input\n"
                  "      --max-slip-x MM    hard lane half-width (default: recall only)\n"
                  "      --max-slip-y MM    hard lane half-height\n"
                  "      --no-crossings     ignore the ratsnest crossing term\n"
                  "      --no-global        skip the force-directed stage\n"
                  "      --no-refine        skip the annealing\n"
                  "      --no-legalize      skip the overlap removal\n"
                  "\n"
                  "options:\n"
                  "  -o, --output PATH  write a placement.bin\n"
                  "      --stats        print the solver statistics\n"
                  "      --quiet        print only errors\n"
                  "  -h, --help         show this help\n"
                  "      --version      show the IR version\n",
                  argv0);
}

static void print_stats(const solver_stats_t *st)
{
    (void)printf("solver:\n");
    (void)printf("  clusters          %u (%u components)\n", (unsigned)st->clusters,
                 (unsigned)st->clustered_components);
    (void)printf("  movable           %u of the scene\n", (unsigned)st->movable);
    (void)printf("  hpwl              %.1f -> %.1f mm\n", (double)st->hpwl_before,
                 (double)st->hpwl_after);
    (void)printf("  crossings         %u -> %u\n", (unsigned)st->crossings_before,
                 (unsigned)st->crossings_after);
    (void)printf("  overlaps          %u -> %u\n", (unsigned)st->overlaps_before,
                 (unsigned)st->overlaps_after);
    (void)printf("  moves             %u tried, %u accepted\n", (unsigned)st->moves_tried,
                 (unsigned)st->moves_accepted);
    (void)printf("  legalize pushes   %u\n", (unsigned)st->legalize_pushes);
    (void)printf("  matched           %u passive(s) assigned to a slot\n",
                 (unsigned)st->matched);
    (void)printf("  overlaps left     %u true (movable), %u within clearance\n",
                 (unsigned)st->unplaced, (unsigned)st->clearance_violations);
    (void)printf("  overlaps in       %u true (movable) before the solver touched it\n",
                 (unsigned)st->unplaced_before);
    (void)printf("  locked overlaps   %u true, not the solver's to fix\n",
                 (unsigned)st->locked_overlaps);
    (void)printf("  too tall          %u frozen above the enclosure ceiling\n",
                 (unsigned)st->too_tall);
    (void)printf("  polished          %u mask micro-move(s)\n", (unsigned)st->polished);
    (void)printf("  swaps             %u part exchange(s) accepted\n",
                 (unsigned)st->swaps_applied);
}

static void print_degraded(uint32_t mask)
{
    const size_t count = sizeof k_degraded_names / sizeof k_degraded_names[0];
    uint32_t categories = 0u;
    for (size_t i = 0u; i < count; ++i) {
        if ((mask & k_degraded_names[i].bit) != 0u) {
            categories += 1u;
        }
    }
    (void)printf("  degraded (mask 0x%08X, %u categor%s):", (unsigned)mask,
                 (unsigned)categories, categories == 1u ? "y" : "ies");
    if (categories == 0u) {
        (void)printf(" none");
    }
    for (size_t i = 0u; i < count; ++i) {
        if ((mask & k_degraded_names[i].bit) != 0u) {
            (void)printf(" %s", k_degraded_names[i].name);
        }
    }
    (void)printf("\n");
}

static void print_summary(const placer_context_t *ctx)
{
    const size_t nsec = sizeof k_degraded_names / sizeof k_degraded_names[0];
    (void)printf("scene: %u components, %u pins, %u nets (%u connections)\n",
                 (unsigned)ctx->comps.count, (unsigned)ctx->pins.count,
                 (unsigned)ctx->nets.num_nets, (unsigned)ctx->nets.num_entries);
    (void)printf("  polygons: %u (%u vertices); net classes: %u\n",
                 (unsigned)ctx->constraints.polygons.num_polys,
                 (unsigned)ctx->constraints.polygons.num_vertices,
                 (unsigned)ctx->rules.num_classes);
    (void)printf("  constraints: %u decoupling, %u thermal, %u symmetry, %u diffpair\n",
                 (unsigned)ctx->constraints.decoupling.count,
                 (unsigned)ctx->constraints.thermal.count,
                 (unsigned)ctx->constraints.symmetry.count,
                 (unsigned)ctx->constraints.diffpairs.count);
    (void)printf("  arenas: scene %zu/%zu B, scratch %zu/%zu B\n", ctx->scene.used,
                 ctx->scene.capacity, ctx->scratch.used, ctx->scratch.capacity);
    print_degraded(ctx->degraded);
    (void)nsec;
}

/*
 * One line a script can parse, plus the breakdown a human reads. The point is
 * to compare two builds of the solver without a spreadsheet: the same board,
 * the same seed, the same numbers.
 */
static uint64_t bench_now(void)
{
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void print_bench(const placer_context_t *ctx, const solver_stats_t *st,
                        const solver_options_t *opt_used, double seconds_load,
                        double seconds_write)
{
    (void)printf("\nbench (seconds):\n");
    (void)printf("  load        %7.3f\n", seconds_load);
    (void)printf("  cluster     %7.3f\n", (double)st->seconds_cluster);
    (void)printf("  incumbent   %7.3f\n", (double)st->seconds_incumbent);
    (void)printf("  global      %7.3f\n", (double)st->seconds_global);
    (void)printf("  refine      %7.3f\n", (double)st->seconds_refine);
    (void)printf("  legalize    %7.3f\n", (double)st->seconds_legalize);
    (void)printf("  write       %7.3f\n", seconds_write);
    (void)printf("  total       %7.3f\n", (double)st->seconds);
    (void)printf("  accept/q    %u %u %u %u of %u moves\n", (unsigned)st->accept_q[0],
                 (unsigned)st->accept_q[1], (unsigned)st->accept_q[2],
                 (unsigned)st->accept_q[3], (unsigned)(st->moves_tried));
    (void)printf("  (cost calls %7.3f, %u evaluations)\n", (double)st->seconds_evaluate,
                 (unsigned)st->moves_tried);
    (void)printf("BENCH hpwl=%.1f hpwl_in=%.1f crossings=%u crossings_in=%u "
                 "overlaps_movable=%u overlaps_movable_in=%u overlaps_locked=%u "
                 "seconds=%.3f scratch_peak=%zu scratch_cap=%zu components=%u movable=%u "
                 "w_hpwl=%.2f w_cross=%.2f w_over=%.2f w_depth=%.2f w_disp=%.2f seed=%u\n",
                 (double)st->hpwl_after, (double)st->hpwl_before, (unsigned)st->crossings_after,
                 (unsigned)st->crossings_before, (unsigned)st->unplaced,
                 (unsigned)st->unplaced_before, (unsigned)st->locked_overlaps,
                 (double)st->seconds, st->scratch_peak, ctx->scratch.capacity,
                 (unsigned)ctx->comps.count, (unsigned)st->movable,
                 (double)opt_used->w_hpwl, (double)opt_used->w_crossings,
                 (double)opt_used->w_overlaps, (double)opt_used->w_overlap_depth,
                 (double)opt_used->w_displacement, (unsigned)opt_used->seed);
}

static bool write_solved_placement(const placer_context_t *ctx, const char *path,
                                   const solver_options_t *opt, bool show_stats, bool bench,
                                   double seconds_load)
{
    const uint32_t count = ctx->comps.count;
    placement_entry_t *entries = (placement_entry_t *)calloc((size_t)count, sizeof *entries);
    if (entries == nullptr) {
        return false;
    }
    solver_stats_t stats;
    if (!placer_solve(ctx, opt, entries, count, &stats)) {
        free(entries);
        (void)fprintf(stderr, "error: the solver could not run on this scene\n");
        return false;
    }
    if (show_stats) {
        print_stats(&stats);
    }
    const uint64_t t0 = bench ? bench_now() : 0u;
    const bool ok = placement_save(path, entries, count);
    if (bench) {
        print_bench(ctx, &stats, opt, seconds_load, (double)(bench_now() - t0) * 1e-9);
    }
    free(entries);
    return ok;
}

static bool write_identity_placement(const placer_context_t *ctx, const char *path)
{
    const uint32_t count = ctx->comps.count;
    placement_entry_t *entries = nullptr;
    if (count > 0u) {
        entries = (placement_entry_t *)calloc((size_t)count, sizeof *entries);
        if (entries == nullptr) {
            return false;
        }
    }
    for (uint32_t c = 0u; c < count; ++c) {
        entries[c].x = ctx->comps.x[c];
        entries[c].y = ctx->comps.y[c];
        entries[c].orient = (uint8_t)comp_orientation(ctx->comps.flags[c]);
        entries[c].side = comp_on_bottom(ctx->comps.flags[c]) ? 1u : 0u;
        entries[c].flags = comp_is_locked(ctx->comps.flags[c]) ? PLACEMENT_FLAG_LOCKED : 0u;
        entries[c].reserved = 0u;
    }
    const bool ok = placement_save(path, entries, count);
    free(entries);
    return ok;
}

int main(int argc, char **argv)
{
    const char *scene_path = nullptr;
    const char *out_path = nullptr;
    bool identity = false;
    bool solve = false;
    bool quiet = false;
    bool show_stats = false;
    bool bench = false;
    solver_options_t options;
    solver_options_defaults(&options);

    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            print_usage(stdout, argv[0]);
            return 0;
        }
        if (strcmp(arg, "--version") == 0) {
            (void)printf("dodplace-solve, scene IR %u.%u, placement IR %u.%u\n",
                         PLACE_SCENE_VERSION_MAJOR, PLACE_SCENE_VERSION_MINOR,
                         PLACE_SCENE_VERSION_MAJOR, PLACE_SCENE_VERSION_MINOR);
            return 0;
        }
        if (strcmp(arg, "-o") == 0 || strcmp(arg, "--output") == 0) {
            if (i + 1 >= argc) {
                (void)fprintf(stderr, "error: %s needs a path\n", arg);
                return 2;
            }
            out_path = argv[++i];
            continue;
        }
        if (strcmp(arg, "--identity") == 0) {
            identity = true;
            continue;
        }
        if (strcmp(arg, "--solve") == 0) {
            solve = true;
            continue;
        }
        if (strcmp(arg, "--stats") == 0) {
            show_stats = true;
            continue;
        }
        if (strcmp(arg, "--bench") == 0) {
            bench = true;
            show_stats = true;
            continue;
        }
        if (strcmp(arg, "--no-crossings") == 0) {
            options.w_crossings = 0.0f;
            continue;
        }
        if (strcmp(arg, "--no-cluster") == 0) {
            options.enable_clustering = false;
            continue;
        }
        if (strcmp(arg, "--global-model") == 0) {
            if (i + 1 >= argc) {
                (void)fprintf(stderr, "error: --global-model needs a name\n");
                return 2;
            }
            const char *name = argv[++i];
            if (strcmp(name, "repulsion") == 0) {
                options.global_model = GLOBAL_MODEL_REPULSION;
            } else if (strcmp(name, "analytic") == 0) {
                options.global_model = GLOBAL_MODEL_ANALYTIC;
            } else {
                (void)fprintf(stderr, "error: --global-model takes repulsion or analytic\n");
                return 2;
            }
            continue;
        }
        if (strcmp(arg, "--w-density") == 0 || strcmp(arg, "--momentum") == 0 ||
            strcmp(arg, "--density-bins") == 0) {
            if (i + 1 >= argc) {
                (void)fprintf(stderr, "error: %s needs a number\n", arg);
                return 2;
            }
            const char *value = argv[++i];
            if (strcmp(arg, "--density-bins") == 0) {
                options.density_bins = (uint32_t)strtoul(value, nullptr, 10);
            } else {
                const coord_t number = (coord_t)atof(value);
                if (strcmp(arg, "--w-density") == 0) {
                    options.w_density = number;
                } else {
                    options.momentum = number;
                }
            }
            continue;
        }
        if (strcmp(arg, "--swap-prob") == 0 || strcmp(arg, "--swap-max-pins") == 0) {
            if (i + 1 >= argc) {
                (void)fprintf(stderr, "error: %s needs a number\n", arg);
                return 2;
            }
            const char *value = argv[++i];
            if (strcmp(arg, "--swap-prob") == 0) {
                options.swap_prob = (coord_t)atof(value);
            } else {
                options.swap_max_pins = (uint32_t)strtoul(value, nullptr, 10);
            }
            continue;
        }
        if (strcmp(arg, "--swap-window") == 0) {
            if (i + 1 >= argc) {
                (void)fprintf(stderr, "error: --swap-window needs a number\n");
                return 2;
            }
            options.swap_window = (uint32_t)strtoul(argv[++i], nullptr, 10);
            continue;
        }
        if (strcmp(arg, "--swap-assign") == 0) {
            if (i + 1 >= argc) {
                (void)fprintf(stderr, "error: --swap-assign needs a number\n");
                return 2;
            }
            options.swap_assign_min = (uint32_t)strtoul(argv[++i], nullptr, 10);
            continue;
        }
        if (strcmp(arg, "--restarts") == 0) {
            if (i + 1 >= argc) {
                (void)fprintf(stderr, "error: --restarts needs a number\n");
                return 2;
            }
            options.refine_restarts = (uint32_t)strtoul(argv[++i], nullptr, 10);
            continue;
        }
        if (strcmp(arg, "--jobs") == 0) {
            if (i + 1 >= argc) {
                (void)fprintf(stderr, "error: --jobs needs a number\n");
                return 2;
            }
            options.jobs = (uint32_t)strtoul(argv[++i], nullptr, 10);
            continue;
        }
        if (strcmp(arg, "--assign-model") == 0) {
            if (i + 1 >= argc) {
                (void)fprintf(stderr, "error: --assign-model needs a name\n");
                return 2;
            }
            const char *name = argv[++i];
            if (strcmp(name, "star") == 0) {
                options.assign_model = ASSIGN_MODEL_STAR;
            } else if (strcmp(name, "wa") == 0) {
                options.assign_model = ASSIGN_MODEL_WA;
            } else {
                (void)fprintf(stderr, "error: --assign-model takes star or wa\n");
                return 2;
            }
            continue;
        }
        if (strcmp(arg, "--wa-gamma") == 0) {
            if (i + 1 >= argc) {
                (void)fprintf(stderr, "error: --wa-gamma needs a number\n");
                return 2;
            }
            options.wa_gamma = (coord_t)atof(argv[++i]);
            continue;
        }
        if (strcmp(arg, "--pad-clearance") == 0 || strcmp(arg, "--w-crossings") == 0 ||
            strcmp(arg, "--polish-reach") == 0) {
            if (i + 1 >= argc) {
                (void)fprintf(stderr, "error: %s needs a number\n", arg);
                return 2;
            }
            const coord_t value = (coord_t)atof(argv[++i]);
            if (strcmp(arg, "--pad-clearance") == 0) {
                options.pad_clearance = value;
            } else if (strcmp(arg, "--w-crossings") == 0) {
                options.w_crossings = value;
            } else {
                options.polish_reach = value;
            }
            continue;
        }
        if (strcmp(arg, "--anneal-t-start-ratio") == 0 ||
            strcmp(arg, "--anneal-t-end-ratio") == 0) {
            if (i + 1 >= argc) {
                (void)fprintf(stderr, "error: %s needs a number\n", arg);
                return 2;
            }
            const coord_t value = (coord_t)atof(argv[++i]);
            if (strcmp(arg, "--anneal-t-start-ratio") == 0) {
                options.anneal_t_start_ratio = value;
            } else {
                options.anneal_t_end_ratio = value;
            }
            continue;
        }
        if (strcmp(arg, "--exclude-plane-nets") == 0) {
            options.exclude_plane_nets = true;
            continue;
        }
        if (strcmp(arg, "--max-net-degree") == 0) {
            if (i + 1 >= argc) {
                (void)fprintf(stderr, "error: %s needs a number\n", arg);
                return 2;
            }
            options.max_net_degree = (uint32_t)strtoul(argv[++i], nullptr, 10);
            continue;
        }
        if (strcmp(arg, "--no-anchor") == 0) {
            options.w_displacement = 0.0f;
            continue;
        }
        if (strcmp(arg, "--max-slip-x") == 0 || strcmp(arg, "--max-slip-y") == 0) {
            if (i + 1 >= argc) {
                (void)fprintf(stderr, "error: %s needs a number\n", arg);
                return 2;
            }
            const coord_t value = (coord_t)atof(argv[++i]);
            if (strcmp(arg, "--max-slip-x") == 0) {
                options.max_slip_x = value;
            } else {
                options.max_slip_y = value;
            }
            continue;
        }
        if (strcmp(arg, "--no-matching") == 0) {
            options.enable_matching = false;
            continue;
        }
        if (strcmp(arg, "--no-global") == 0) {
            options.enable_global = false;
            continue;
        }
        if (strcmp(arg, "--no-refine") == 0) {
            options.enable_refine = false;
            continue;
        }
        if (strcmp(arg, "--no-legalize") == 0) {
            options.enable_legalize = false;
            continue;
        }
        if (strcmp(arg, "--seed") == 0 || strcmp(arg, "--iterations") == 0 ||
            strcmp(arg, "--moves") == 0) {
            if (i + 1 >= argc) {
                (void)fprintf(stderr, "error: %s needs a number\n", arg);
                return 2;
            }
            const unsigned long value = strtoul(argv[++i], nullptr, 10);
            if (strcmp(arg, "--seed") == 0) {
                options.seed = (uint32_t)value;
            } else if (strcmp(arg, "--iterations") == 0) {
                options.global_iterations = (uint32_t)value;
            } else {
                options.refine_moves = (uint32_t)value;
            }
            continue;
        }
        if (strcmp(arg, "--quiet") == 0) {
            quiet = true;
            continue;
        }
        if (arg[0] == '-') {
            (void)fprintf(stderr, "error: unknown option '%s'\n", arg);
            print_usage(stderr, argv[0]);
            return 2;
        }
        if (scene_path != nullptr) {
            (void)fprintf(stderr, "error: unexpected argument '%s'\n", arg);
            return 2;
        }
        scene_path = arg;
    }

    if (scene_path == nullptr) {
        print_usage(stderr, argv[0]);
        return 2;
    }
    if (out_path != nullptr && !identity && !solve) {
        (void)fprintf(stderr, "error: select a strategy: --solve or --identity\n");
        return 2;
    }
    if (identity && solve) {
        (void)fprintf(stderr, "error: --identity and --solve are mutually exclusive\n");
        return 2;
    }

    placer_context_t ctx;
    scene_io_report_t io;
    const uint64_t t_load0 = bench_now();
    if (!scene_load(&ctx, scene_path, &io)) {
        (void)fprintf(stderr, "error: %s (%u error(s))\n",
                      io.message[0] != '\0' ? io.message : "cannot load scene",
                      (unsigned)io.errors);
        return 1;
    }

    const double seconds_load = (double)(bench_now() - t_load0) * 1e-9;
    if (!quiet) {
        print_summary(&ctx);
        (void)printf("  loaded %u section(s), skipped %u\n", (unsigned)io.sections_loaded,
                     (unsigned)io.sections_skipped);
    }

    if (out_path != nullptr) {
        const bool ok =
            solve ? write_solved_placement(&ctx, out_path, &options, show_stats, bench,
                                           seconds_load)
                  : write_identity_placement(&ctx, out_path);
        if (!ok) {
            (void)fprintf(stderr, "error: cannot write '%s'\n", out_path);
            placer_context_destroy(&ctx);
            return 1;
        }
        if (!quiet) {
            (void)printf("wrote %s placement: %s (%u components)\n",
                         solve ? "solved" : "identity", out_path,
                         (unsigned)ctx.comps.count);
        }
    }

    placer_context_destroy(&ctx);
    return 0;
}
