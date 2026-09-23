/*
 * test_analytic.c - the DCT and the density field it solves.
 *
 * Three things have to be true before the analytical global stage can be
 * trusted with a board: the transform pair has to be the inverse of itself, the
 * Poisson solve has to put the field where the density is (a dense bin is a
 * potential peak, so the force runs off it), and the stage has to separate
 * parts that start on top of each other - without touching a random number
 * generator, because two runs of the same scene must agree to the bit.
 */
#include "place/context.h"
#include "place/solver.h"
#include "density_grid.h"
#include "solver_state.h"

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

#define CHECK_NEAR(got, want, tol)                                             \
    do {                                                                       \
        const double g_ = (double)(got);                                       \
        const double w_ = (double)(want);                                      \
        if (fabs(g_ - w_) > (double)(tol)) {                                   \
            (void)printf("FAIL %s:%d: %s == %f, expected %f\n", __FILE__,      \
                         __LINE__, #got, g_, w_);                              \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

/* Four parts stacked on the same spot, one net pulling them together and a
 * board with room: the density field is the only thing that can separate them. */
enum { N_COMP = 4u, N_PIN = 4u, N_NET = 1u };

static bool build_stacked_scene(placer_context_t *ctx)
{
    const scene_counts_t counts = {
        .num_comps = N_COMP,
        .num_pins = N_PIN,
        .num_nets = N_NET,
        .num_net_entries = N_PIN,
        .num_polygons = 1u,
        .num_vertices = 5u,
    };
    if (!placer_context_begin(ctx, &counts, nullptr)) {
        return false;
    }
    const coord_t outline[10] = {0.0f, 0.0f, 40.0f, 0.0f, 40.0f, 30.0f, 0.0f, 30.0f, 0.0f, 0.0f};
    CHECK(placer_add_polygon(ctx, POLY_KIND_BOARD_OUTLINE, outline, 5u, 0u) != PLACE_ID_NONE);

    for (uint32_t i = 0u; i < N_COMP; ++i) {
        const component_desc_t desc = {
            .x = 20.0f,
            .y = 15.0f,
            .half_w = 1.5f,
            .half_h = 1.0f,
            .kind = COMP_KIND_IC,
            .has_orientation = true,
        };
        CHECK(placer_add_component(ctx, &desc) != PLACE_ID_NONE);
    }
    place_id_t pins[N_PIN];
    for (uint32_t i = 0u; i < N_PIN; ++i) {
        const pin_desc_t pin = {
            .comp = (place_id_t)i,
            .offset_x = 0.5f * (coord_t)i,
            .offset_y = 0.0f,
            .flags_valid = true,
        };
        const place_id_t id = placer_add_pin(ctx, &pin);
        CHECK(id != PLACE_ID_NONE);
        pins[i] = id;
    }
    const net_desc_t net = {.weight = 1.0f, .net_class = 0u};
    CHECK(placer_add_net(ctx, &net, pins, N_PIN) != PLACE_ID_NONE);
    return placer_context_finalize(ctx);
}

static void test_the_transform_is_its_own_inverse(void)
{
    placer_context_t ctx;
    CHECK(build_stacked_scene(&ctx));
    solver_t s;
    solver_options_t opt;
    solver_options_defaults(&opt);
    memset(&s, 0, sizeof s);
    CHECK(solver_state_init(&s, &ctx, &opt));

    dct_axis_t axis;
    CHECK(dct_axis_init(&axis, &s, 16u));
    coord_t in[16];
    coord_t mid[16];
    coord_t back[16];
    for (uint32_t i = 0u; i < 16u; ++i) {
        in[i] = sinf(0.7f * (coord_t)i) + 0.3f * (coord_t)i;
    }
    dct_axis_forward(&axis, in, mid);
    dct_axis_inverse(&axis, mid, back);
    for (uint32_t i = 0u; i < 16u; ++i) {
        CHECK_NEAR(back[i], in[i], 1e-4);
    }

    /* A constant has one non-zero coefficient: the mean. No cosine mode sees
     * it, which is why a uniform density exerts no force. */
    for (uint32_t i = 0u; i < 16u; ++i) {
        in[i] = 3.0f;
    }
    dct_axis_forward(&axis, in, mid);
    CHECK_NEAR(mid[0], 2.0f * 16.0f * 3.0f, 1e-3);
    for (uint32_t k = 1u; k < 16u; ++k) {
        CHECK_NEAR(mid[k], 0.0f, 1e-3);
    }
    placer_context_destroy(&ctx);
}

static void test_the_field_runs_off_a_dense_bin(void)
{
    placer_context_t ctx;
    CHECK(build_stacked_scene(&ctx));
    solver_t s;
    solver_options_t opt;
    solver_options_defaults(&opt);
    memset(&s, 0, sizeof s);
    CHECK(solver_state_init(&s, &ctx, &opt));

    density_grid_t grid;
    CHECK(density_grid_init(&s, &grid, 16u, 16u));
    density_rasterise(&s, &grid);

    /* The parts are stacked at the board's centre: the density peaks there. */
    coord_t peak = 0.0f;
    for (uint32_t i = 0u; i < 16u * 16u; ++i) {
        peak = (grid.rho[i] > peak) ? grid.rho[i] : peak;
    }
    CHECK(peak > 0.5f);

    density_solve(&grid);
    /* Just off the centre, the force must point away from the pile. */
    const coord_t cx = (s.board_min_x + s.board_max_x) * 0.5f + 1.0f;
    const coord_t cy = (s.board_min_y + s.board_max_y) * 0.5f;
    coord_t fx = 0.0f;
    coord_t fy = 0.0f;
    density_force_at(&grid, cx, cy, &fx, &fy);
    CHECK(fx > 0.0f);
    CHECK(fabsf(fy) < fabsf(fx));

    /* Away from the pile the field is quieter than at its edge. */
    coord_t gx = 0.0f;
    coord_t gy = 0.0f;
    density_force_at(&grid, s.board_min_x + 1.0f, cy, &gx, &gy);
    CHECK(fabsf(gx) < fx);
    placer_context_destroy(&ctx);
}

static void test_the_analytic_stage_separates_and_repeats(void)
{
    placement_entry_t first[N_COMP];

    for (uint32_t run = 0u; run < 2u; ++run) {
        placer_context_t ctx;
        CHECK(build_stacked_scene(&ctx));
        solver_options_t opt;
        solver_options_defaults(&opt);
        opt.global_model = GLOBAL_MODEL_ANALYTIC;
        opt.global_iterations = 200u;
        opt.density_bins = 32u;
        /* The global stage alone: no annealing, no legalisation, no matching. */
        opt.enable_refine = false;
        opt.enable_legalize = false;
        opt.enable_matching = false;

        placement_entry_t out[N_COMP];
        solver_stats_t stats;
        CHECK(placer_solve(&ctx, &opt, out, N_COMP, &stats));
        CHECK(stats.overlaps_before > 0u); /* the pile arrives on one spot */
        for (uint32_t i = 0u; i < N_COMP; ++i) {
            if (run == 0u) {
                first[i] = out[i];
                continue;
            }
            CHECK_NEAR(out[i].x, first[i].x, 0.0);
            CHECK_NEAR(out[i].y, first[i].y, 0.0);
        }
        /* The field pushed them apart: no two centres are still on top of
         * each other, and the parts stayed on the board. */
        for (uint32_t i = 0u; i < N_COMP; ++i) {
            CHECK(out[i].x > 0.0f && out[i].x < 40.0f);
            CHECK(out[i].y > 0.0f && out[i].y < 30.0f);
            for (uint32_t j = i + 1u; j < N_COMP; ++j) {
                const coord_t dx = out[i].x - out[j].x;
                const coord_t dy = out[i].y - out[j].y;
                CHECK(sqrtf(dx * dx + dy * dy) > 2.0f);
            }
        }
        placer_context_destroy(&ctx);
    }
}

int main(void)
{
    test_the_transform_is_its_own_inverse();
    test_the_field_runs_off_a_dense_bin();
    test_the_analytic_stage_separates_and_repeats();

    if (g_failures == 0) {
        (void)printf("test_analytic: all checks passed\n");
    } else {
        (void)printf("test_analytic: %d check(s) failed\n", g_failures);
    }
    return g_failures;
}
