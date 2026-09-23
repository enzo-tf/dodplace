/*
 * test_swap_window.c - the exact permutation of interchangeable parts.
 *
 * Two instances of one part sit on two slots, and the netlist wants them the
 * other way round. The window pass has to find that, and it has to find it
 * without moving a single rectangle: the set of poses on the board must come
 * out of the pass exactly as it went in, only assigned to different instances.
 * That property is the whole reason the move is allowed on a board whose DRC
 * is already clean.
 */
#include "place/context.h"
#include "place/solver.h"
#include "solver_state.h"
#include "swap_window.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_failures;

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            (void)printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                                \
        }                                                                \
    } while (0)

enum {
    SW_P1 = 0,   /* the pair: same part, same footprint */
    SW_P2 = 1,
    SW_LEFT = 2, /* a fixed pad of net B, on the left */
    SW_RIGHT = 3,/* a fixed pad of net A, on the right */
    SW_COUNT = 4,
    SW_PINS = 4,
    SW_NETS = 2,
};

static const uint32_t k_part = 0xC0FFEEu;

static bool build_scene(placer_context_t *ctx)
{
    const scene_counts_t counts = {
        .num_comps = SW_COUNT,
        .num_pins = SW_PINS,
        .num_nets = SW_NETS,
        .num_net_entries = SW_PINS,
        .num_polygons = 1u,
        .num_vertices = 5u,
    };
    if (!placer_context_begin(ctx, &counts, nullptr)) {
        return false;
    }
    const coord_t outline[10] = {0.0f, 0.0f, 40.0f, 0.0f, 40.0f, 30.0f, 0.0f, 30.0f, 0.0f, 0.0f};
    CHECK(placer_add_polygon(ctx, POLY_KIND_BOARD_OUTLINE, outline, 5u, 0u) != PLACE_ID_NONE);

    /* P1 sits on the left, P2 on the right - and each carries the net whose
     * other end is on the *far* side. Swapping them is worth 60 mm. */
    const component_desc_t p1 = {
        .x = 5.0f, .y = 15.0f, .half_w = 1.0f, .half_h = 1.0f,
        .kind = COMP_KIND_CAPACITOR, .has_orientation = true, .part_id = k_part,
    };
    const component_desc_t p2 = {
        .x = 35.0f, .y = 15.0f, .half_w = 1.0f, .half_h = 1.0f,
        .kind = COMP_KIND_CAPACITOR, .has_orientation = true, .part_id = k_part,
    };
    const component_desc_t left = {
        .x = 5.0f, .y = 5.0f, .half_w = 0.5f, .half_h = 0.5f,
        .locked = true, .kind = COMP_KIND_OTHER, .has_orientation = true,
    };
    const component_desc_t right = {
        .x = 35.0f, .y = 5.0f, .half_w = 0.5f, .half_h = 0.5f,
        .locked = true, .kind = COMP_KIND_OTHER, .has_orientation = true,
    };
    CHECK(placer_add_component(ctx, &p1) != PLACE_ID_NONE);
    CHECK(placer_add_component(ctx, &p2) != PLACE_ID_NONE);
    CHECK(placer_add_component(ctx, &left) != PLACE_ID_NONE);
    CHECK(placer_add_component(ctx, &right) != PLACE_ID_NONE);

    place_id_t pins[SW_PINS];
    for (uint32_t i = 0u; i < SW_PINS; ++i) {
        const pin_desc_t pin = {
            .comp = (place_id_t)i,
            .offset_x = 0.0f,
            .offset_y = 0.0f,
            .flags_valid = true,
        };
        const place_id_t id = placer_add_pin(ctx, &pin);
        CHECK(id != PLACE_ID_NONE);
        pins[i] = id;
    }
    /* net A: P1's pin and the pad on the right; net B: P2's pin and the pad on
     * the left. Each part is on the wrong side of its own net. */
    const place_id_t net_a[2] = {pins[SW_P1], pins[SW_RIGHT]};
    const place_id_t net_b[2] = {pins[SW_P2], pins[SW_LEFT]};
    const net_desc_t net = {.weight = 1.0f, .net_class = 0u};
    CHECK(placer_add_net(ctx, &net, net_a, 2u) != PLACE_ID_NONE);
    CHECK(placer_add_net(ctx, &net, net_b, 2u) != PLACE_ID_NONE);
    return placer_context_finalize(ctx);
}

static void poses_of(const placement_entry_t *out, coord_t *pose, uint32_t n)
{
    for (uint32_t i = 0u; i < n; ++i) {
        pose[i * 3u] = out[i].x;
        pose[i * 3u + 1u] = out[i].y;
        pose[i * 3u + 2u] = (coord_t)out[i].orient;
    }
}

static bool same_pose_set(const coord_t *a, const coord_t *b, uint32_t n)
{
    bool used[SW_COUNT] = {false, false, false, false};
    for (uint32_t i = 0u; i < n; ++i) {
        bool found = false;
        for (uint32_t j = 0u; j < n && !found; ++j) {
            if (!used[j] && fabsf(a[i * 3u] - b[j * 3u]) < 1e-4f &&
                fabsf(a[i * 3u + 1u] - b[j * 3u + 1u]) < 1e-4f &&
                a[i * 3u + 2u] == b[j * 3u + 2u]) {
                used[j] = true;
                found = true;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

static void test_the_window_permutes_without_moving_a_rectangle(void)
{
    placement_entry_t out[SW_COUNT];
    placement_entry_t again[SW_COUNT];
    coord_t after[SW_COUNT * 3u];
    coord_t repeat[SW_COUNT * 3u];

    for (uint32_t run = 0u; run < 2u; ++run) {
        placer_context_t ctx;
        CHECK(build_scene(&ctx));
        /* The window pass alone: no global, no annealing, no legalisation. */
        solver_options_t opt;
        solver_options_defaults(&opt);
        opt.enable_global = false;
        opt.enable_legalize = false;
        opt.enable_matching = false;
        opt.refine_moves = 0u;
        opt.swap_window = 2u;

        solver_stats_t stats;
        placement_entry_t *target = (run == 0u) ? out : again;
        CHECK(placer_solve(&ctx, &opt, target, SW_COUNT, &stats));
        if (run == 0u) {
            /* The pair swapped: each net's two pins now coincide in x. */
            CHECK(target[SW_P1].x == 35.0f);
            CHECK(target[SW_P2].x == 5.0f);
            coord_t input[SW_COUNT * 3u];
            for (uint32_t i = 0u; i < SW_COUNT; ++i) {
                input[i * 3u] = ctx.comps.x[i];
                input[i * 3u + 1u] = ctx.comps.y[i];
                input[i * 3u + 2u] = (coord_t)(ctx.comps.flags[i] & COMP_ORIENT_MASK);
            }
            poses_of(target, after, SW_COUNT);
            /* No rectangle moved: the poses are the same set, reassigned. */
            CHECK(same_pose_set(input, after, SW_COUNT));
        } else {
            poses_of(target, repeat, SW_COUNT);
            for (uint32_t k = 0u; k < SW_COUNT * 3u; ++k) {
                CHECK(fabsf(repeat[k] - after[k]) < 1e-6f);
            }
        }
        placer_context_destroy(&ctx);
    }
}

int main(void)
{
    test_the_window_permutes_without_moving_a_rectangle();
    if (g_failures == 0) {
        (void)printf("test_swap_window: all checks passed\n");
    } else {
        (void)printf("test_swap_window: %d check(s) failed\n", g_failures);
    }
    return g_failures;
}
