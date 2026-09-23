/*
 * test_ingestion_degraded.c - ingestion contract and degraded mode.
 *
 * Scenario A  "KiCad minimal": only the mandatory blocks are supplied. Every
 *             optional block must fall back, the scene must be structurally
 *             valid, and no pointer may be null.
 * Scenario B  "enriched": the same scene with optional data (heights, masses,
 *             explicit pin roles, net weights/classes, thermal / decoupling /
 *             symmetry / differential pairs, stackup, clearance matrix,
 *             keepout). Every corresponding degraded bit must be clear.
 * Scenario C  malformed / undersized inputs must fail cleanly, never crash.
 */
#include "place/context.h"

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

#define CHECK_NEAR(got, want)                                                  \
    do {                                                                       \
        const double g_ = (double)(got);                                       \
        const double w_ = (double)(want);                                      \
        if (fabs(g_ - w_) > 1e-4) {                                            \
            (void)printf("FAIL %s:%d: %s == %f, expected %f\n", __FILE__,       \
                         __LINE__, #got, g_, w_);                              \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

/* All 21 optional-data bits must be raised by a bare KiCad ingestion. */
static const uint32_t ALL_DEGRADED_BITS =
    DEGRADED_NO_3D_HEIGHT | DEGRADED_NO_MASS | DEGRADED_NO_ROTATION_DATA |
    DEGRADED_NO_PIN_ELEC | DEGRADED_NO_PIN_CURRENT | DEGRADED_NO_NET_WEIGHTS |
    DEGRADED_NO_NET_CLASSES | DEGRADED_NO_DIFFPAIRS | DEGRADED_NO_KEEPOUTS |
    DEGRADED_NO_STACKUP | DEGRADED_NO_CLEARANCE_MATRIX |
    DEGRADED_NO_DECOUPLING | DEGRADED_NO_THERMAL | DEGRADED_NO_SYMMETRY |
    DEGRADED_NO_AIRFLOW | DEGRADED_NO_CEILING | DEGRADED_NO_VIA_RESTRICTION |
    DEGRADED_COURTYARD_BBOX | DEGRADED_BOARD_OUTLINE_BBOX |
    DEGRADED_NO_MAX_LENGTH | DEGRADED_NO_SEGREGATION;

static void check_mandatory_pointers_non_null(const placer_context_t *ctx)
{
    /* Memory */
    CHECK(ctx->scene.base != nullptr);
    CHECK(ctx->scratch.base != nullptr);
    /* Components SoA */
    CHECK(ctx->comps.x != nullptr);
    CHECK(ctx->comps.y != nullptr);
    CHECK(ctx->comps.half_w != nullptr);
    CHECK(ctx->comps.half_h != nullptr);
    CHECK(ctx->comps.height != nullptr);
    CHECK(ctx->comps.mass != nullptr);
    CHECK(ctx->comps.flags != nullptr);
    CHECK(ctx->comps.first_pin != nullptr);
    CHECK(ctx->comps.pin_count != nullptr);
    CHECK(ctx->comps.polygon_id != nullptr);
    /* Pins SoA */
    CHECK(ctx->pins.offset_x != nullptr);
    CHECK(ctx->pins.offset_y != nullptr);
    CHECK(ctx->pins.comp_id != nullptr);
    CHECK(ctx->pins.net_id != nullptr);
    CHECK(ctx->pins.flags != nullptr);
    CHECK(ctx->pins.max_current != nullptr);
    /* Net CSR */
    CHECK(ctx->nets.net_offsets != nullptr);
    CHECK(ctx->nets.net_to_pins != nullptr);
    CHECK(ctx->nets.weights != nullptr);
    CHECK(ctx->nets.net_class != nullptr);
    /* Polygon pool */
    CHECK(ctx->constraints.polygons.vx != nullptr);
    CHECK(ctx->constraints.polygons.vy != nullptr);
    CHECK(ctx->constraints.polygons.poly_offsets != nullptr);
    CHECK(ctx->constraints.polygons.poly_kind != nullptr);
}

/* ------------------------------------------------------------------------- */
/* Scenario A - KiCad-minimal ingestion                                       */
/* ------------------------------------------------------------------------- */

static void add_u1_soic(placer_context_t *ctx)
{
    /* Explicit courtyard: trusted as centred on the footprint origin. */
    const component_desc_t u1 = {
        .x = 0.0f,
        .y = 0.0f,
        .half_w = 2.5f,
        .half_h = 2.0f,
        .on_bottom = false,
        .locked = false,
        .rot_fixed = false,
        .has_orientation = false, /* no machine rotation data -> degraded */
        .orientation_deg = 0.0f,
        .has_height = false,
        .height = 0.0f,
        .has_mass = false,
        .mass = 0.0f,
        .polygon_xy = nullptr,
        .polygon_n = 0u,
    };
    CHECK(placer_add_component(ctx, &u1) == 0u);

    static const coord_t pin_xy[8][2] = {
        {-3.0f, -1.905f}, {-3.0f, -0.635f}, {-3.0f, 0.635f}, {-3.0f, 1.905f},
        {3.0f, -1.905f},  {3.0f, -0.635f},  {3.0f, 0.635f},  {3.0f, 1.905f},
    };
    static const char *const pin_names[8] = {
        "VDD", "GND", "SIG1", "SIG2", "SIG3", "SIG4", "CLK", "D0",
    };
    for (uint32_t i = 0u; i < 8u; ++i) {
        const pin_desc_t pin = {
            .comp = 0u,
            .offset_x = pin_xy[i][0],
            .offset_y = pin_xy[i][1],
            .name = pin_names[i],
            .flags_valid = false, /* name inference kicks in */
            .flags = 0u,
            .max_current = 0.0f,
        };
        CHECK(placer_add_pin(ctx, &pin) == (place_id_t)i);
    }
}

static void add_c1_0603(placer_context_t *ctx)
{
    /* No courtyard: pad bbox + margin, and the origin is rebased to the pad
     * bbox centre (pads sit at x = 0 and x = 1.575). */
    const component_desc_t c1 = {
        .x = 10.0f,
        .y = 5.0f,
        .half_w = 0.0f,
        .half_h = 0.0f,
        .has_orientation = false,
        .has_height = false,
        .has_mass = false,
    };
    CHECK(placer_add_component(ctx, &c1) == 1u);

    const pin_desc_t p0 = {
        .comp = 1u, .offset_x = 0.0f, .offset_y = 0.0f,
        .name = "1", .flags_valid = false,
    };
    const pin_desc_t p1 = {
        .comp = 1u, .offset_x = 1.575f, .offset_y = 0.0f,
        .name = "2", .flags_valid = false,
    };
    CHECK(placer_add_pin(ctx, &p0) == 8u);
    CHECK(placer_add_pin(ctx, &p1) == 9u);
}

static void scenario_a_kicad_minimal(void)
{
    (void)printf("-- scenario A: KiCad-minimal ingestion\n");

    placer_context_t ctx;
    const scene_counts_t counts = {
        .num_comps = 2u,
        .num_pins = 10u,
        .num_nets = 3u,
        .num_net_entries = 10u,
        .num_polygons = 0u,
        .num_vertices = 0u,
    };
    CHECK(placer_context_begin(&ctx, &counts, nullptr));

    add_u1_soic(&ctx);
    add_c1_0603(&ctx);

    const net_desc_t gnd = {.weight = 0.0f, .net_class = 0xFFu};
    const net_desc_t v33 = {.weight = 0.0f, .net_class = 0xFFu};
    const net_desc_t sig = {.weight = 0.0f, .net_class = 0xFFu};
    const place_id_t gnd_pins[2] = {1u, 9u};
    const place_id_t v33_pins[2] = {0u, 8u};
    const place_id_t sig_pins[6] = {2u, 3u, 4u, 5u, 6u, 7u};

    CHECK(placer_add_net(&ctx, &gnd, gnd_pins, 2u) == 0u);
    CHECK(placer_add_net(&ctx, &v33, v33_pins, 2u) == 1u);
    CHECK(placer_add_net(&ctx, &sig, sig_pins, 6u) == 2u);

    CHECK(placer_context_finalize(&ctx));

    /* The byte estimate must be sufficient on the first attempt: capacity is
     * exactly the estimate, so no growth retry happened. */
    CHECK(ctx.scene.capacity == placer_context_estimate_scene_bytes(&counts));
    CHECK(ctx.scratch.capacity == placer_context_estimate_scratch_bytes(&counts));

    /* No null pointers anywhere in the mandatory blocks. */
    check_mandatory_pointers_non_null(&ctx);

    /* Every optional block fell back, and each fallback is counted once. */
    CHECK(ctx.degraded == ALL_DEGRADED_BITS);
    CHECK(ctx.warnings == 21u);
    CHECK(ctx.errors == 0u);

    /* --- courtyards ------------------------------------------------------- */
    /* U1 keeps its explicit AABB, untouched. */
    CHECK_NEAR(ctx.comps.half_w[0], 2.5f);
    CHECK_NEAR(ctx.comps.half_h[0], 2.0f);
    CHECK_NEAR(ctx.comps.x[0], 0.0f);
    CHECK_NEAR(ctx.comps.y[0], 0.0f);
    /* C1: pad bbox (= 1.575 x 0 mm) + 0.25 mm margin, origin rebased. */
    CHECK_NEAR(ctx.comps.half_w[1], 0.7875f + 0.25f);
    CHECK_NEAR(ctx.comps.half_h[1], 0.0f + 0.25f);
    CHECK_NEAR(ctx.comps.x[1], 10.0f + 0.7875f);
    CHECK_NEAR(ctx.comps.y[1], 5.0f);
    /* Rebased offsets are symmetric around the new centre. */
    CHECK_NEAR(ctx.pins.offset_x[9] - ctx.pins.offset_x[8], 1.575f);
    CHECK_NEAR(ctx.pins.offset_x[8], -0.7875f);

    /* --- optional per-component data -------------------------------------- */
    CHECK_NEAR(ctx.comps.height[0], 0.0f);
    CHECK_NEAR(ctx.comps.height[1], 0.0f);
    CHECK_NEAR(ctx.comps.mass[0], 0.0f);
    CHECK(comp_orientation(ctx.comps.flags[0]) == 0u);
    CHECK(!comp_on_bottom(ctx.comps.flags[0]));
    CHECK(!comp_is_locked(ctx.comps.flags[0]));

    /* --- pin CSR ---------------------------------------------------------- */
    CHECK(ctx.comps.first_pin[0] == 0u);
    CHECK(ctx.comps.pin_count[0] == 8u);
    CHECK(ctx.comps.first_pin[1] == 8u);
    CHECK(ctx.comps.pin_count[1] == 2u);

    /* --- electrical-role inference ---------------------------------------- */
    CHECK(ctx.pins.flags[0] == (PIN_POWER | PIN_INFERRED));  /* VDD  */
    CHECK(ctx.pins.flags[1] == (PIN_GROUND | PIN_INFERRED)); /* GND  */
    CHECK(ctx.pins.flags[6] == (PIN_CLOCK | PIN_INFERRED));  /* CLK  */
    CHECK(ctx.pins.flags[2] == PIN_INFERRED);                /* SIG1 */
    CHECK(ctx.pins.flags[8] == PIN_INFERRED);                /* "1"  */

    /* --- net CSR ---------------------------------------------------------- */
    CHECK(ctx.nets.num_nets == 3u);
    CHECK(ctx.nets.num_entries == 10u);
    CHECK(ctx.nets.net_offsets[0] == 0u);
    CHECK(ctx.nets.net_offsets[1] == 2u);
    CHECK(ctx.nets.net_offsets[2] == 4u);
    CHECK(ctx.nets.net_offsets[3] == 10u);
    CHECK(ctx.nets.net_to_pins[0] == 1u && ctx.nets.net_to_pins[1] == 9u);
    CHECK(ctx.nets.net_to_pins[2] == 0u && ctx.nets.net_to_pins[3] == 8u);
    for (uint32_t i = 0u; i < ctx.nets.num_nets; ++i) {
        CHECK_NEAR(ctx.nets.weights[i], 1.0f);
        CHECK(ctx.nets.net_class[i] == 0u);
    }

    /* --- board outline fallback ------------------------------------------- */
    CHECK(ctx.constraints.polygons.num_polys == 1u);
    CHECK(ctx.constraints.polygons.num_vertices == 5u);
    CHECK(ctx.constraints.polygons.poly_kind[0] == POLY_KIND_BOARD_OUTLINE);
    /* Union of both courtyards: x [-2.5, 11.825], y [-2.0, 5.25]. */
    coord_t min_x = 1e9f;
    coord_t max_x = -1e9f;
    coord_t min_y = 1e9f;
    coord_t max_y = -1e9f;
    for (uint32_t v = 0u; v < 5u; ++v) {
        const coord_t vx = ctx.constraints.polygons.vx[v];
        const coord_t vy = ctx.constraints.polygons.vy[v];
        if (vx < min_x) { min_x = vx; }
        if (vx > max_x) { max_x = vx; }
        if (vy < min_y) { min_y = vy; }
        if (vy > max_y) { max_y = vy; }
    }
    CHECK_NEAR(min_x, -2.5f);
    CHECK_NEAR(max_x, 11.825f);
    CHECK_NEAR(min_y, -2.0f);
    CHECK_NEAR(max_y, 5.25f);

    /* --- optional tables are empty and indexed by nothing ------------------ */
    CHECK(ctx.constraints.decoupling.count == 0u);
    CHECK(ctx.constraints.thermal.count == 0u);
    CHECK(ctx.constraints.symmetry.count == 0u);
    CHECK(ctx.constraints.diffpairs.count == 0u);
    CHECK(ctx.constraints.present == CT_PRESENT_POLYGONS);

    /* --- structural validation is clean ------------------------------------ */
    validation_report_t report;
    CHECK(placer_context_validate(&ctx, &report));
    CHECK(report.errors == 0u);
    CHECK(report.warnings == 0u);

    /* --- negative test: a broken context must be reported, not dereferenced - */
    placer_context_t broken = ctx;
    broken.comps.x = nullptr;
    CHECK(!placer_context_validate(&broken, &report));
    CHECK(report.errors > 0u);

    broken = ctx;
    broken.nets.net_offsets = nullptr;
    CHECK(!placer_context_validate(&broken, &report));
    CHECK(report.errors > 0u);

    broken = ctx;
    broken.ingest.finalized = false;
    broken.errors = 1u;
    CHECK(!placer_context_validate(&broken, &report));
    CHECK(report.errors > 0u);

    placer_context_destroy(&ctx);
    /* Destroy is idempotent and leaves no dangling pointer. */
    placer_context_destroy(&ctx);
    CHECK(ctx.scene.base == nullptr);
    CHECK(ctx.comps.x == nullptr);
}

/* ------------------------------------------------------------------------- */
/* Scenario B - fully enriched ingestion                                      */
/* ------------------------------------------------------------------------- */

static void scenario_b_enriched(void)
{
    (void)printf("-- scenario B: enriched ingestion\n");

    placer_context_t ctx;
    const scene_counts_t counts = {
        .num_comps = 2u,
        .num_pins = 4u,
        .num_nets = 3u,
        .num_net_entries = 4u,
        .num_polygons = 3u, /* U1 courtyard + outline + keepout */
        .num_vertices = 4u + 5u + 4u,
        .num_decoupling = 1u,
        .num_thermal = 1u,
        .num_symmetry = 1u,
        .num_diffpairs = 1u,
    };
    CHECK(placer_context_begin(&ctx, &counts, nullptr));

    /* U1: explicit AABB + exact courtyard polygon + full 3D data. */
    const coord_t u1_poly[8] = {
        -2.5f, -2.0f, 2.5f, -2.0f, 2.5f, 2.0f, -2.5f, 2.0f,
    };
    const component_desc_t u1 = {
        .x = 0.0f, .y = 0.0f, .half_w = 2.5f, .half_h = 2.0f,
        .has_orientation = true, .orientation_deg = 90.0f,
        .has_height = true, .height = 1.5f,
        .has_mass = true, .mass = 0.5f,
        .polygon_xy = u1_poly, .polygon_n = 4u,
    };
    CHECK(placer_add_component(&ctx, &u1) == 0u);

    const component_desc_t c1 = {
        .x = 10.0f, .y = 5.0f, .half_w = 0.9f, .half_h = 0.45f,
        .has_orientation = true, .orientation_deg = 0.0f,
        .has_height = true, .height = 0.8f,
        .has_mass = true, .mass = 0.01f,
    };
    CHECK(placer_add_component(&ctx, &c1) == 1u);

    const pin_desc_t pins[4] = {
        {.comp = 0u, .offset_x = -3.0f, .offset_y = -1.9f, .name = "VDD",
         .flags_valid = true, .flags = PIN_POWER, .max_current = 0.5f},
        {.comp = 0u, .offset_x = -3.0f, .offset_y = 1.9f, .name = "GND",
         .flags_valid = true, .flags = PIN_GROUND, .max_current = 0.5f},
        {.comp = 1u, .offset_x = -0.8f, .offset_y = 0.0f, .name = "1",
         .flags_valid = true, .flags = 0u, .max_current = 1.0f},
        {.comp = 1u, .offset_x = 0.8f, .offset_y = 0.0f, .name = "2",
         .flags_valid = true, .flags = PIN_GROUND, .max_current = 1.0f},
    };
    for (uint32_t i = 0u; i < 4u; ++i) {
        CHECK(placer_add_pin(&ctx, &pins[i]) == (place_id_t)i);
    }

    const net_desc_t v33 = {.weight = 3.0f, .net_class = 1u};
    const net_desc_t gnd = {.weight = 1.0f, .net_class = 0u};
    const net_desc_t dpx = {.weight = 5.0f, .net_class = 2u};
    const place_id_t v33_pins[2] = {0u, 2u};
    const place_id_t gnd_pins[2] = {1u, 3u};
    CHECK(placer_add_net(&ctx, &v33, v33_pins, 2u) == 0u);
    CHECK(placer_add_net(&ctx, &gnd, gnd_pins, 2u) == 1u);
    CHECK(placer_add_net(&ctx, &dpx, nullptr, 0u) == 2u);

    /* Board outline and keepout (optional, must not fall back). */
    const coord_t outline[10] = {
        -5.0f, -5.0f, 15.0f, -5.0f, 15.0f, 10.0f, -5.0f, 10.0f, -5.0f, -5.0f,
    };
    const coord_t keepout[8] = {8.0f, 3.0f, 12.0f, 3.0f, 12.0f, 7.0f, 8.0f, 7.0f};
    CHECK(placer_add_polygon(&ctx, POLY_KIND_BOARD_OUTLINE, outline, 5u, 0u) != PLACE_ID_NONE);
    CHECK(placer_add_polygon(&ctx, POLY_KIND_KEEPOUT, keepout, 4u, 0x3u) != PLACE_ID_NONE);

    /* Optional constraint tables. */
    CHECK(placer_add_decoupling(&ctx, 0u, 1u, 0u, 5.0f, 1.0f));
    CHECK(placer_add_thermal(&ctx, 0u, 0.25f, 3.0f, 40.0f));
    CHECK(placer_add_symmetry(&ctx, 0u, 1u, SYM_AXIS_X, 1.0f));
    CHECK(placer_add_diffpair(&ctx, 0u, 2u, 0.1f));

    /* Optional board data. The clearance matrix is scene-arena owned: board
     * enrichment must outlive finalize's scratch reset. */
    coord_t *matrix = (coord_t *)arena_alloc0(&ctx.scene, sizeof(coord_t) * 4u,
                                              PLACE_CACHELINE);
    CHECK(matrix != nullptr);
    matrix[0] = 0.2f;
    matrix[1] = 0.3f;
    matrix[2] = 0.3f;
    matrix[3] = 0.2f;
    ctx.rules.class_clearance = matrix;
    ctx.rules.num_classes = 2u;
    ctx.rules.allow_vias_under_body = false;
    ctx.stackup.has_stackup = true;
    ctx.stackup.copper_layers = 4u;
    ctx.stackup.total_thickness = 1.6f;
    ctx.stackup.ceiling_height = 10.0f;
    ctx.stackup.airflow_x = 1.0f;
    ctx.stackup.airflow_y = 0.0f;

    CHECK(placer_context_finalize(&ctx));
    check_mandatory_pointers_non_null(&ctx);

    /* Only the two features that have no data model in phase 1 stay degraded. */
    CHECK(ctx.degraded == (DEGRADED_NO_MAX_LENGTH | DEGRADED_NO_SEGREGATION));
    CHECK(ctx.warnings == 2u);
    CHECK(ctx.errors == 0u);

    /* Enrichment reached the dense arrays. */
    CHECK_NEAR(ctx.comps.height[0], 1.5f);
    CHECK_NEAR(ctx.comps.mass[1], 0.01f);
    CHECK(comp_orientation(ctx.comps.flags[0]) == 1u); /* 90 deg */
    CHECK((ctx.comps.flags[0] & COMP_HAS_HEIGHT) != 0u);
    CHECK((ctx.comps.flags[0] & COMP_HAS_MASS) != 0u);
    CHECK((ctx.comps.flags[0] & COMP_POLY_COURTYARD) != 0u);
    CHECK(ctx.pins.flags[0] == PIN_POWER); /* authoritative, not inferred */
    CHECK((ctx.pins.flags[0] & PIN_INFERRED) == 0u);
    CHECK_NEAR(ctx.nets.weights[0], 3.0f);
    CHECK(ctx.nets.net_class[2] == 2u);

    /* Sparse tables, their presence bits and their CSR indices. */
    CHECK(ctx.constraints.present == (CT_PRESENT_DECOUPLING | CT_PRESENT_THERMAL |
                                      CT_PRESENT_SYMMETRY | CT_PRESENT_DIFFPAIRS |
                                      CT_PRESENT_POLYGONS));
    CHECK(ctx.constraints.decoupling.count == 1u);
    CHECK(ctx.constraints.decoupling.first_by_comp != nullptr);
    CHECK(ctx.constraints.decoupling.first_by_comp[0] == 0u);
    CHECK(ctx.constraints.decoupling.first_by_comp[1] == 1u);
    CHECK(ctx.constraints.decoupling.first_by_comp[2] == 1u);
    CHECK_NEAR(ctx.constraints.decoupling.max_dist_sq[0], 25.0f);
    CHECK(ctx.constraints.thermal.first_by_comp[1] == 1u);
    CHECK(ctx.constraints.symmetry.first_by_comp[1] == 1u);
    CHECK(ctx.constraints.symmetry.axis[0] == SYM_AXIS_X);
    CHECK(ctx.constraints.diffpairs.count == 1u);
    CHECK(ctx.constraints.polygons.num_polys == 3u);

    validation_report_t report;
    CHECK(placer_context_validate(&ctx, &report));
    CHECK(report.errors == 0u);

    placer_context_destroy(&ctx);
}

/* ------------------------------------------------------------------------- */
/* Scenario C - malformed and undersized inputs                               */
/* ------------------------------------------------------------------------- */

static void scenario_c_failures(void)
{
    (void)printf("-- scenario C: malformed and undersized inputs\n");

    /* C1: an empty netlist is a documented failure. */
    {
        placer_context_t ctx;
        const scene_counts_t counts = {.num_comps = 1u, .num_pins = 1u, .num_nets = 0u};
        CHECK(placer_context_begin(&ctx, &counts, nullptr));
        const component_desc_t comp = {
            .x = 0.0f, .y = 0.0f, .half_w = 1.0f, .half_h = 1.0f, .has_orientation = true,
        };
        CHECK(placer_add_component(&ctx, &comp) == 0u);
        const pin_desc_t pin = {.comp = 0u, .flags_valid = true, .flags = PIN_POWER};
        CHECK(placer_add_pin(&ctx, &pin) == 0u);
        CHECK(!placer_context_finalize(&ctx));
        CHECK(ctx.errors > 0u);
        validation_report_t report;
        CHECK(!placer_context_validate(&ctx, &report));
        CHECK(report.errors > 0u);
        placer_context_destroy(&ctx);
    }

    /* C2: an explicit scene capacity that is too small fails without crashing. */
    {
        placer_context_t ctx;
        const scene_counts_t counts = {
            .num_comps = 1u, .num_pins = 1u, .num_nets = 1u, .num_net_entries = 1u,
            .num_polygons = 1u, .num_vertices = 3u,
        };
        const placer_options_t opt = {.scene_capacity = 64u};
        CHECK(!placer_context_begin(&ctx, &counts, &opt));
        CHECK(ctx.errors > 0u);
        placer_context_destroy(&ctx);
    }

    /* C3: wrong pin order (out of component grouping) is rejected. */
    {
        placer_context_t ctx;
        const scene_counts_t counts = {
            .num_comps = 2u, .num_pins = 2u, .num_nets = 1u, .num_net_entries = 2u,
        };
        CHECK(placer_context_begin(&ctx, &counts, nullptr));
        const component_desc_t comp = {
            .x = 0.0f, .y = 0.0f, .half_w = 1.0f, .half_h = 1.0f, .has_orientation = true,
        };
        (void)placer_add_component(&ctx, &comp);
        (void)placer_add_component(&ctx, &comp);
        const pin_desc_t p1 = {.comp = 1u, .flags_valid = true, .flags = PIN_POWER};
        const pin_desc_t p0 = {.comp = 0u, .flags_valid = true, .flags = PIN_POWER};
        CHECK(placer_add_pin(&ctx, &p1) == 0u);
        CHECK(placer_add_pin(&ctx, &p0) == PLACE_ID_NONE); /* must be refused */
        CHECK(ctx.errors > 0u);
        placer_context_destroy(&ctx);
    }

    /* C4: adding after finalize is refused; out-of-range ids are refused. */
    {
        placer_context_t ctx;
        const scene_counts_t counts = {
            .num_comps = 1u, .num_pins = 1u, .num_nets = 1u, .num_net_entries = 1u,
        };
        CHECK(placer_context_begin(&ctx, &counts, nullptr));
        const component_desc_t comp = {
            .x = 0.0f, .y = 0.0f, .half_w = 1.0f, .half_h = 1.0f, .has_orientation = true,
        };
        CHECK(placer_add_component(&ctx, &comp) == 0u);
        const pin_desc_t pin = {.comp = 0u, .flags_valid = true, .flags = PIN_POWER};
        CHECK(placer_add_pin(&ctx, &pin) == 0u);
        const place_id_t net_pins[1] = {0u};
        const net_desc_t net = {.weight = 1.0f, .net_class = 0u};
        CHECK(placer_add_net(&ctx, &net, net_pins, 1u) == 0u);
        CHECK(placer_context_finalize(&ctx));
        /* Once finalised, the scene is frozen... */
        CHECK(placer_add_component(&ctx, &comp) == PLACE_ID_NONE);
        /* ...and connection mistakes are reported instead of corrupting the CSR. */
        CHECK(!placer_connect(&ctx, 0u, 0u)); /* pin already connected */
        CHECK(!placer_connect(&ctx, 5u, 0u)); /* pin out of range */
        CHECK(ctx.errors > 0u);
        placer_context_destroy(&ctx);
    }

    /* C5: a transient, zeroed context is safe to destroy. */
    {
        placer_context_t ctx;
        memset(&ctx, 0, sizeof ctx);
        placer_context_destroy(&ctx);
        validation_report_t report;
        CHECK(!placer_context_validate(&ctx, &report));
        CHECK(report.errors > 0u);
    }
}

int main(void)
{
    scenario_a_kicad_minimal();
    scenario_b_enriched();
    scenario_c_failures();

    if (g_failures == 0) {
        (void)printf("test_ingestion_degraded: all checks passed\n");
    } else {
        (void)printf("test_ingestion_degraded: %d check(s) failed\n", g_failures);
    }
    return g_failures;
}
