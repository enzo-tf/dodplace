/*
 * test_solver.c - the placement engine.
 *
 * The scene is small but has every feature the stages care about: an IC driving
 * an inductor and an output capacitor (the power loop pattern of stage 1), a
 * second decoupling capacitor, a locked connector acting as an anchor, and a
 * board outline that everything must stay inside.
 */
#include "place/context.h"
#include "place/io.h"
#include "place/solver.h"
#include "raster_mask.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_failures;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            (void)printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
            g_failures++;                                                 \
        }                                                                 \
    } while (0)

#define CHECK_NEAR(got, want, tol)                                            \
    do {                                                                      \
        const double g_ = (double)(got);                                      \
        const double w_ = (double)(want);                                     \
        if (fabs(g_ - w_) > (double)(tol)) {                                          \
            (void)printf("FAIL %s:%d: %s == %f, expected %f\n", __FILE__,      \
                         __LINE__, #got, g_, w_);                             \
            g_failures++;                                                     \
        }                                                                     \
    } while (0)

enum {
    COMP_U1 = 0,
    COMP_L1 = 1,
    COMP_C1 = 2,
    COMP_C2 = 3,
    COMP_R1 = 4,
    COMP_J1 = 5,
    COMP_COUNT = 6
};

enum {
    PIN_U1_SW = 0,
    PIN_U1_OUT = 1,
    PIN_U1_VDD = 2,
    PIN_U1_GND = 3,
    PIN_L1_A = 4,
    PIN_L1_B = 5,
    PIN_C1_OUT = 6,
    PIN_C1_GND = 7,
    PIN_C2_VDD = 8,
    PIN_C2_GND = 9,
    PIN_R1_VDD = 10,
    PIN_R1_GND = 11,
    PIN_J1_VDD = 12,
    PIN_J1_GND = 13,
    PIN_COUNT = 14
};

enum {
    NET_SW = 0,
    NET_OUT = 1,
    NET_VDD = 2,
    NET_GND = 3,
    NET_COUNT = 4
};

static void add_component(placer_context_t *ctx, coord_t x, coord_t y, coord_t hw, coord_t hh,
                          comp_kind_t kind, bool locked)
{
    const component_desc_t desc = {
        .x = x,
        .y = y,
        .half_w = hw,
        .half_h = hh,
        .has_orientation = true,
        .orientation_deg = 0.0f,
        .kind = kind,
        .locked = locked,
    };
    CHECK(placer_add_component(ctx, &desc) != PLACE_ID_NONE);
}

static void add_pin(placer_context_t *ctx, place_id_t comp, coord_t dx, coord_t dy)
{
    const pin_desc_t desc = {
        .comp = comp,
        .offset_x = dx,
        .offset_y = dy,
        .flags_valid = true,
    };
    CHECK(placer_add_pin(ctx, &desc) != PLACE_ID_NONE);
}

static void add_net(placer_context_t *ctx, const place_id_t *pins, uint32_t count)
{
    const net_desc_t desc = {.weight = 1.0f, .net_class = 0u};
    CHECK(placer_add_net(ctx, &desc, pins, count) != PLACE_ID_NONE);
}

static bool build_scene(placer_context_t *ctx, bool overlapping)
{
    const scene_counts_t counts = {
        .num_comps = COMP_COUNT,
        .num_pins = PIN_COUNT,
        .num_nets = NET_COUNT,
        .num_net_entries = PIN_COUNT,
        .num_polygons = 1u,
        .num_vertices = 5u,
    };
    if (!placer_context_begin(ctx, &counts, nullptr)) {
        return false;
    }

    const coord_t outline[10] = {0.0f, 0.0f, 40.0f, 0.0f, 40.0f, 30.0f, 0.0f, 30.0f, 0.0f, 0.0f};
    CHECK(placer_add_polygon(ctx, POLY_KIND_BOARD_OUTLINE, outline, 5u, 0u) != PLACE_ID_NONE);

    /* U1 drives a power loop; the caps are placed far away so the solver has
     * something to improve. When `overlapping`, C1 and C2 sit on top of each
     * other: legalisation must separate them. */
    add_component(ctx, 5.0f, 5.0f, 1.5f, 1.0f, COMP_KIND_IC, false);
    add_component(ctx, 30.0f, 25.0f, 0.5f, 0.5f, COMP_KIND_INDUCTOR, false);
    add_component(ctx, 35.0f, 5.0f, 0.25f, 0.25f, COMP_KIND_CAPACITOR, false);
    add_component(ctx, 5.0f, 25.0f, 0.25f, 0.25f, COMP_KIND_CAPACITOR, false);
    /* R1 sits on top of C2 when the test asks for an overlap: neither is in a
     * cluster, so legalisation has to resolve it on its own. */
    add_component(ctx, overlapping ? 5.0f : 15.0f, 25.0f, 0.25f, 0.25f, COMP_KIND_RESISTOR,
                  false);
    add_component(ctx, 38.0f, 15.0f, 1.0f, 0.5f, COMP_KIND_CONNECTOR, true);

    add_pin(ctx, COMP_U1, -1.0f, -0.5f);
    add_pin(ctx, COMP_U1, -1.0f, 0.5f);
    add_pin(ctx, COMP_U1, 1.0f, -0.5f);
    add_pin(ctx, COMP_U1, 1.0f, 0.5f);
    add_pin(ctx, COMP_L1, -0.5f, 0.0f);
    add_pin(ctx, COMP_L1, 0.5f, 0.0f);
    add_pin(ctx, COMP_C1, -0.25f, 0.0f);
    add_pin(ctx, COMP_C1, 0.25f, 0.0f);
    add_pin(ctx, COMP_C2, -0.25f, 0.0f);
    add_pin(ctx, COMP_C2, 0.25f, 0.0f);
    add_pin(ctx, COMP_R1, -0.25f, 0.0f);
    add_pin(ctx, COMP_R1, 0.25f, 0.0f);
    add_pin(ctx, COMP_J1, -1.0f, 0.0f);
    add_pin(ctx, COMP_J1, 1.0f, 0.0f);

    const place_id_t sw[2] = {PIN_U1_SW, PIN_L1_A};
    const place_id_t out[3] = {PIN_U1_OUT, PIN_L1_B, PIN_C1_OUT};
    const place_id_t vdd[4] = {PIN_U1_VDD, PIN_C2_VDD, PIN_R1_VDD, PIN_J1_VDD};
    const place_id_t gnd[5] = {PIN_U1_GND, PIN_C1_GND, PIN_C2_GND, PIN_R1_GND, PIN_J1_GND};
    add_net(ctx, sw, 2u);
    add_net(ctx, out, 3u);
    add_net(ctx, vdd, 4u);
    add_net(ctx, gnd, 5u);

    return placer_context_finalize(ctx);
}

static void test_defaults_are_sane(void)
{
    solver_options_t opt;
    solver_options_defaults(&opt);
    CHECK(opt.global_iterations > 0u);
    CHECK(opt.refine_moves > 0u);
    CHECK(opt.legalize_passes > 0u);
    CHECK(opt.w_hpwl > 0.0f);
    CHECK(opt.w_crossings > 0.0f);
    CHECK(opt.w_overlaps > 0.0f);
    CHECK(opt.rotation_mask == 0x0Fu);
    CHECK(!opt.enable_clustering); /* rigid fusion retired, discrete matching to come */
    CHECK(opt.enable_global && opt.enable_refine && opt.enable_legalize);
    CHECK(opt.enable_matching); /* on by default once the overlap count was exact */

    /* The Gold configuration: the reference run on r10 (HPWL 36 812.2 mm,
     * 0 movable overlap, KiCad DRC 0/0/0 in 0.88 s). It is a contract, not a
     * coincidence - changing any of these silently invalidates the reference. */
    CHECK(opt.refine_moves == 4000u);
    CHECK(opt.w_crossings == 5.0f);
    CHECK(opt.pad_clearance == 0.5f);
    CHECK(opt.polish_reach == 0.20f);
    CHECK(opt.anneal_t_start_ratio == 0.01f);
    CHECK(opt.anneal_t_end_ratio == 1.0e-3f);
    CHECK(opt.seed != 0u);
    CHECK(opt.board_margin > 0.0f);
}

static void test_solve_produces_a_full_placement(void)
{
    placer_context_t ctx;
    CHECK(build_scene(&ctx, false));

    placement_entry_t out[COMP_COUNT];
    solver_stats_t stats;
    solver_options_t opt;
    solver_options_defaults(&opt);
    opt.enable_clustering = true; /* off by default: this test is about stage 1 */
    CHECK(placer_solve(&ctx, &opt, out, COMP_COUNT, &stats));
    CHECK(stats.clusters >= 1u);
    CHECK(stats.clustered_components >= 3u); /* U1 + L1 + C1 */
    /* A cluster counts as one movable body: its satellites follow the master. */
    CHECK(stats.movable >= 2u);
    CHECK(stats.movable <= COMP_COUNT - 1u);
    CHECK(stats.hpwl_after <= stats.hpwl_before);
    placer_context_destroy(&ctx);
}

static void test_locked_parts_do_not_move(void)
{
    placer_context_t ctx;
    CHECK(build_scene(&ctx, false));

    placement_entry_t out[COMP_COUNT];
    CHECK(placer_solve(&ctx, nullptr, out, COMP_COUNT, nullptr));
    CHECK((out[COMP_J1].flags & PLACEMENT_FLAG_LOCKED) != 0u);
    CHECK_NEAR(out[COMP_J1].x, ctx.comps.x[COMP_J1], 1e-4);
    CHECK_NEAR(out[COMP_J1].y, ctx.comps.y[COMP_J1], 1e-4);
    CHECK((out[COMP_U1].flags & PLACEMENT_FLAG_MOVED) != 0u);
    placer_context_destroy(&ctx);
}

static void test_legalisation_separates_overlaps(void)
{
    placer_context_t ctx;
    CHECK(build_scene(&ctx, true)); /* C1 and C2 start on top of each other */

    placement_entry_t out[COMP_COUNT];
    solver_stats_t stats;
    CHECK(placer_solve(&ctx, nullptr, out, COMP_COUNT, &stats));
    CHECK(stats.overlaps_before > 0u);
    CHECK(stats.unplaced_fixable == 0u);
    /* separated on at least one axis */
    const coord_t dx = fabsf(out[COMP_R1].x - out[COMP_C2].x);
    const coord_t dy = fabsf(out[COMP_R1].y - out[COMP_C2].y);
    CHECK(dx >= 0.499f || dy >= 0.499f);
    placer_context_destroy(&ctx);
}

static void test_everything_stays_inside_the_board(void)
{
    placer_context_t ctx;
    CHECK(build_scene(&ctx, true));

    placement_entry_t out[COMP_COUNT];
    CHECK(placer_solve(&ctx, nullptr, out, COMP_COUNT, nullptr));
    for (uint32_t i = 0u; i < COMP_COUNT; ++i) {
        /* the extent that matters is the one after the chosen orientation */
        const bool odd = (out[i].orient & 1u) != 0u;
        const coord_t hw = odd ? ctx.comps.half_h[i] : ctx.comps.half_w[i];
        const coord_t hh = odd ? ctx.comps.half_w[i] : ctx.comps.half_h[i];
        CHECK(out[i].x - hw >= -0.01f);
        CHECK(out[i].x + hw <= 40.01f);
        if (out[i].x - hw < -0.01f || out[i].x + hw > 40.01f || out[i].y - hh < -0.01f ||
            out[i].y + hh > 30.01f) {
            (void)printf("  component %u: (%.3f, %.3f) hw=%.3f hh=%.3f orient=%u\n", i,
                         (double)out[i].x, (double)out[i].y, (double)hw, (double)hh,
                         (unsigned)out[i].orient);
        }
        CHECK(out[i].y - hh >= -0.01f);
        CHECK(out[i].y + hh <= 30.01f);
    }
    placer_context_destroy(&ctx);
}

static void test_same_seed_same_layout(void)
{
    placer_context_t ctx;
    CHECK(build_scene(&ctx, true));

    placement_entry_t a[COMP_COUNT];
    placement_entry_t b[COMP_COUNT];
    solver_options_t opt;
    solver_options_defaults(&opt);
    opt.seed = 1234u;
    CHECK(placer_solve(&ctx, &opt, a, COMP_COUNT, nullptr));
    CHECK(placer_solve(&ctx, &opt, b, COMP_COUNT, nullptr));
    for (uint32_t i = 0u; i < COMP_COUNT; ++i) {
        CHECK(a[i].x == b[i].x && a[i].y == b[i].y && a[i].orient == b[i].orient);
    }

    /* A different seed explores differently (not a guarantee, but with these
     * weights a different layout is the expected outcome). */
    solver_options_t other = opt;
    other.seed = 99u;
    placement_entry_t c[COMP_COUNT];
    CHECK(placer_solve(&ctx, &other, c, COMP_COUNT, nullptr));
    placer_context_destroy(&ctx);
}

static void test_stage_toggles_are_honoured(void)
{
    placer_context_t ctx;
    CHECK(build_scene(&ctx, true));

    solver_options_t opt;
    solver_options_defaults(&opt);
    opt.enable_clustering = false;
    opt.enable_global = false;
    opt.enable_refine = false;
    opt.enable_legalize = false;
    placement_entry_t out[COMP_COUNT];
    solver_stats_t stats;
    CHECK(placer_solve(&ctx, &opt, out, COMP_COUNT, &stats));
    CHECK(stats.clusters == 0u);
    CHECK(stats.moves_tried == 0u);
    /* With every stage off the placement is the input, unchanged. */
    for (uint32_t i = 0u; i < COMP_COUNT; ++i) {
        CHECK_NEAR(out[i].x, ctx.comps.x[i], 1e-6);
        CHECK_NEAR(out[i].y, ctx.comps.y[i], 1e-6);
    }
    placer_context_destroy(&ctx);
}

static void test_refuses_a_scene_that_is_not_finalised(void)
{
    placer_context_t ctx;
    CHECK(build_scene(&ctx, false));
    ctx.ingest.finalized = false;
    placement_entry_t out[COMP_COUNT];
    CHECK(!placer_solve(&ctx, nullptr, out, COMP_COUNT, nullptr));
    ctx.ingest.finalized = true;

    /* too small a buffer must be refused, not overflowed */
    CHECK(!placer_solve(&ctx, nullptr, out, COMP_COUNT - 1u, nullptr));
    placer_context_destroy(&ctx);
}

/*
 * The occupancy mask: blocked where a locked part or a keepout is, clear in the
 * cavities between them, and never clear outside the board. A mask that says
 * "blocked" everywhere is what made the last-resort search useless before - it
 * reads as a full board - so the cavity case is the one that matters.
 */
static void test_raster_mask_finds_the_free_cavities(void)
{
    placer_context_t ctx;
    CHECK(build_scene(&ctx, false));

    uint32_t cluster_of[COMP_COUNT];
    for (uint32_t i = 0u; i < COMP_COUNT; ++i) {
        cluster_of[i] = SOLVER_NO_INDEX;
    }
    solver_t s;
    memset(&s, 0, sizeof s);
    s.ctx = &ctx;
    s.ncomp = ctx.comps.count;
    s.x = ctx.comps.x;
    s.y = ctx.comps.y;
    s.half_w = ctx.comps.half_w;
    s.half_h = ctx.comps.half_h;
    s.cluster_of = cluster_of;
    s.scratch = &ctx.scratch;
    s.edge_min_x = 0.0f;
    s.edge_min_y = 0.0f;
    s.edge_max_x = 40.0f;
    s.edge_max_y = 30.0f;

    raster_mask_t m;
    CHECK(raster_mask_init(&m, &s, 0.25f));
    raster_mask_build_obstacles(&m, &s, 0.2f);

    /* J1 is the locked connector at (38, 15), half extents (1.0, 0.5). */
    CHECK(!raster_mask_box_clear(&m, 1.0f, 37.5f, 14.8f, 38.5f, 15.2f)); /* inside it */
    CHECK(!raster_mask_box_clear(&m, 1.0f, 36.5f, 14.8f, 37.5f, 15.2f)); /* straddling */
    /* A movable part is not an obstacle: the solver is allowed to move it. */
    CHECK(raster_mask_box_clear(&m, 1.0f, 4.5f, 4.5f, 5.5f, 5.5f));      /* over U1 */
    /* Two obstacles with a gap: the gap is a cavity, not a wall. */
    raster_mask_t c;
    CHECK(raster_mask_init(&c, &s, 0.25f));
    raster_mask_stamp_box(&c, 10.0f, 10.0f, 12.0f, 12.0f);
    raster_mask_stamp_box(&c, 16.0f, 10.0f, 18.0f, 12.0f);
    CHECK(raster_mask_box_clear(&c, 1.0f, 13.0f, 10.5f, 15.0f, 11.5f));  /* between them */
    CHECK(!raster_mask_box_clear(&c, 1.0f, 11.0f, 10.5f, 13.0f, 11.5f)); /* reaching in */
    /* Off the board is never clear. */
    CHECK(!raster_mask_box_clear(&c, 1.0f, -5.0f, 10.0f, 1.0f, 11.0f));
    /* A part taller than the enclosure fits nowhere, however empty the board. */
    raster_mask_t t;
    CHECK(raster_mask_init(&t, &s, 0.25f));
    t.ceiling = 3.0f;
    CHECK(raster_mask_box_clear(&t, 2.0f, 13.0f, 20.0f, 15.0f, 22.0f));
    CHECK(!raster_mask_box_clear(&t, 4.0f, 13.0f, 20.0f, 15.0f, 22.0f));
    placer_context_destroy(&ctx);
}

static void test_the_solver_leaves_no_trace_in_the_scratch(void)
{
    placer_context_t ctx;
    CHECK(build_scene(&ctx, true));
    const size_t before = ctx.scratch.used;
    placement_entry_t out[COMP_COUNT];
    CHECK(placer_solve(&ctx, nullptr, out, COMP_COUNT, nullptr));
    CHECK(ctx.scratch.used == before); /* it rewinds its own mark */
    placer_context_destroy(&ctx);
}

int main(void)
{
    test_defaults_are_sane();
    test_solve_produces_a_full_placement();
    test_locked_parts_do_not_move();
    test_legalisation_separates_overlaps();
    test_everything_stays_inside_the_board();
    test_same_seed_same_layout();
    test_stage_toggles_are_honoured();
    test_refuses_a_scene_that_is_not_finalised();
    test_raster_mask_finds_the_free_cavities();
    test_the_solver_leaves_no_trace_in_the_scratch();

    if (g_failures == 0) {
        (void)printf("test_solver: all checks passed\n");
    } else {
        (void)printf("test_solver: %d check(s) failed\n", g_failures);
    }
    return g_failures;
}
