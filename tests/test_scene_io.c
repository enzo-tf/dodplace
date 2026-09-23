/*
 * test_scene_io.c - the IR contract.
 *
 *  A. round trip   : build a scene through the push API, finalize, scene_save,
 *                    scene_load into a fresh context, and require every field -
 *                    raw AND derived (first_pin ranges, net CSR, constraint
 *                    indices) - to be identical.
 *  B. golden file  : the committed ir/fixtures/scene_minimal.bin must load and
 *                    match the scene this test builds. It is the byte-level
 *                    contract the Python writer has to match in phase 2.
 *  C. rejections   : corrupted, truncated, misaligned, mismatched and
 *                    incomplete files must be refused without crashing, and
 *                    must leave no half-built context behind.
 *
 * Regenerate the fixture with:  test_scene_io --emit-fixture <path>
 */
#include "place/context.h"
#include "place/io.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            (void)printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
            g_failures++;                                                 \
        }                                                                 \
    } while (0)

static void check_bytes_equal(const void *a, const void *b, size_t bytes, const char *what)
{
    if (a == nullptr || b == nullptr) {
        if (a != b) {
            (void)printf("FAIL %s: array '%s' is null on one side only\n", __FILE__, what);
            g_failures++;
        }
        return;
    }
    if (memcmp(a, b, bytes) != 0) {
        (void)printf("FAIL %s: array '%s' differs (%zu bytes)\n", __FILE__, what, bytes);
        g_failures++;
    }
}

/* ========================================================================= */
/* Demo scene - covers every section of the IR                               */
/* ========================================================================= */

static const scene_counts_t k_demo_counts = {
    .num_comps = 3u,
    .num_pins = 8u,
    .num_nets = 3u,
    .num_net_entries = 8u,
    .num_polygons = 3u, /* U1 courtyard + outline + keepout */
    .num_vertices = 4u + 5u + 4u,
    .num_decoupling = 1u,
    .num_thermal = 1u,
    .num_symmetry = 1u,
    .num_diffpairs = 1u,
    .num_net_classes = 2u,
};

static void demo_add_components(placer_context_t *ctx)
{
    const coord_t u1_poly[8] = {
        -2.5f, -2.0f, 2.5f, -2.0f, 2.5f, 2.0f, -2.5f, 2.0f,
    };
    const component_desc_t u1 = {
        .x = 10.0f, .y = 10.0f, .half_w = 2.5f, .half_h = 2.0f,
        .locked = true,
        .has_orientation = true, .orientation_deg = 90.0f,
        .has_height = true, .height = 1.5f,
        .has_mass = true, .mass = 0.5f,
        .kind = COMP_KIND_IC,
        .polygon_xy = u1_poly, .polygon_n = 4u,
    };
    CHECK(placer_add_component(ctx, &u1) == 0u);

    /* No courtyard: pad-bbox fallback plus origin rebase (pads at 0 and 1.6). */
    const component_desc_t c1 = {
        .x = 20.0f, .y = 10.0f,
        .has_orientation = true, .orientation_deg = 0.0f,
        .has_height = true, .height = 0.8f,
        .has_mass = true, .mass = 0.01f,
        .kind = COMP_KIND_CAPACITOR,
    };
    CHECK(placer_add_component(ctx, &c1) == 1u);

    /* Bottom side, 270 degrees, no height/mass -> degraded through the IR. */
    const component_desc_t r1 = {
        .x = 30.0f, .y = 10.0f, .half_w = 0.9f, .half_h = 0.45f,
        .on_bottom = true,
        .has_orientation = true, .orientation_deg = 270.0f,
        .kind = COMP_KIND_RESISTOR,
    };
    CHECK(placer_add_component(ctx, &r1) == 2u);
}

static void demo_add_pins(placer_context_t *ctx)
{
    const pin_desc_t u1_pins[4] = {
        {.comp = 0u, .offset_x = -3.0f, .offset_y = -1.5f, .name = "VDD", .flags_valid = true,
         .flags = PIN_POWER, .max_current = 0.5f},
        {.comp = 0u, .offset_x = -3.0f, .offset_y = 1.5f, .name = "GND", .flags_valid = true,
         .flags = PIN_GROUND, .max_current = 0.5f},
        {.comp = 0u, .offset_x = 3.0f, .offset_y = -1.5f, .name = "SIG1", .flags_valid = true,
         .max_current = 0.5f},
        {.comp = 0u, .offset_x = 3.0f, .offset_y = 1.5f, .name = "SIG2", .flags_valid = true,
         .max_current = 0.5f},
    };
    for (uint32_t i = 0u; i < 4u; ++i) {
        CHECK(placer_add_pin(ctx, &u1_pins[i]) == (place_id_t)i);
    }

    const pin_desc_t c1_pins[2] = {
        {.comp = 1u, .offset_x = 0.0f, .offset_y = 0.0f, .name = "1", .flags_valid = true,
         .max_current = 1.0f},
        {.comp = 1u, .offset_x = 1.6f, .offset_y = 0.0f, .name = "2", .flags_valid = true,
         .flags = PIN_GROUND, .max_current = 1.0f},
    };
    CHECK(placer_add_pin(ctx, &c1_pins[0]) == 4u);
    CHECK(placer_add_pin(ctx, &c1_pins[1]) == 5u);

    /* max_current left at 0: NO_PIN_CURRENT must survive the round trip. */
    const pin_desc_t r1_pins[2] = {
        {.comp = 2u, .offset_x = 0.0f, .offset_y = -1.0f, .name = "1", .flags_valid = true},
        {.comp = 2u, .offset_x = 0.0f, .offset_y = 1.0f, .name = "2", .flags_valid = true,
         .flags = PIN_GROUND},
    };
    CHECK(placer_add_pin(ctx, &r1_pins[0]) == 6u);
    CHECK(placer_add_pin(ctx, &r1_pins[1]) == 7u);
}

static void demo_add_nets(placer_context_t *ctx)
{
    const net_desc_t vcc = {.weight = 3.0f, .net_class = 1u};
    const net_desc_t gnd = {.weight = 1.0f, .net_class = 0u};
    const net_desc_t sig = {.weight = 2.0f, .net_class = 1u};
    const place_id_t vcc_pins[2] = {0u, 4u};
    const place_id_t gnd_pins[3] = {1u, 5u, 7u};
    const place_id_t sig_pins[3] = {2u, 3u, 6u};
    CHECK(placer_add_net(ctx, &vcc, vcc_pins, 2u) == 0u);
    CHECK(placer_add_net(ctx, &gnd, gnd_pins, 3u) == 1u);
    CHECK(placer_add_net(ctx, &sig, sig_pins, 3u) == 2u);
    CHECK(placer_add_diffpair(ctx, 0u, 2u, 0.1f));
}

static void demo_add_polygons(placer_context_t *ctx)
{
    const coord_t outline[10] = {
        -5.0f, -5.0f, 35.0f, -5.0f, 35.0f, 20.0f, -5.0f, 20.0f, -5.0f, -5.0f,
    };
    const coord_t keepout[8] = {25.0f, 8.0f, 33.0f, 8.0f, 33.0f, 12.0f, 25.0f, 12.0f};
    CHECK(placer_add_polygon(ctx, POLY_KIND_BOARD_OUTLINE, outline, 5u, 0u) != PLACE_ID_NONE);
    CHECK(placer_add_polygon(ctx, POLY_KIND_KEEPOUT, keepout, 4u, 0x3u) != PLACE_ID_NONE);
}

static void demo_add_constraints(placer_context_t *ctx)
{
    CHECK(placer_add_decoupling(ctx, 0u, 1u, 0u, 5.0f, 1.0f));
    CHECK(placer_add_thermal(ctx, 0u, 0.25f, 3.0f, 40.0f));
    CHECK(placer_add_symmetry(ctx, 0u, 2u, SYM_AXIS_X, 1.0f));
}

static void demo_add_board_data(placer_context_t *ctx)
{
    coord_t *matrix = (coord_t *)arena_alloc0(&ctx->scene, sizeof(coord_t) * 4u,
                                              PLACE_CACHELINE);
    CHECK(matrix != nullptr);
    matrix[0] = 0.20f;
    matrix[1] = 0.30f;
    matrix[2] = 0.30f;
    matrix[3] = 0.20f;
    ctx->rules.class_clearance = matrix;
    ctx->rules.num_classes = 2u;
    ctx->rules.min_track_width = 0.25f;
    ctx->rules.allow_vias_under_body = false;
    ctx->stackup.has_stackup = true;
    ctx->stackup.copper_layers = 4u;
    ctx->stackup.total_thickness = 1.6f;
    ctx->stackup.ceiling_height = 10.0f;
    ctx->stackup.airflow_x = 1.0f;
    ctx->stackup.airflow_y = 0.0f;
}

static bool build_demo_scene(placer_context_t *ctx)
{
    if (!placer_context_begin(ctx, &k_demo_counts, nullptr)) {
        return false;
    }
    demo_add_components(ctx);
    demo_add_pins(ctx);
    demo_add_nets(ctx);
    demo_add_polygons(ctx);
    demo_add_constraints(ctx);
    demo_add_board_data(ctx);
    return placer_context_finalize(ctx);
}

/* ========================================================================= */
/* Deep comparison                                                           */
/* ========================================================================= */

static void check_contexts_equal(const placer_context_t *a, const placer_context_t *b)
{
    CHECK(a->comps.count == b->comps.count);
    CHECK(a->pins.count == b->pins.count);
    CHECK(a->nets.num_nets == b->nets.num_nets);
    CHECK(a->nets.num_entries == b->nets.num_entries);
    CHECK(a->degraded == b->degraded);
    CHECK(a->warnings == b->warnings);

    if (a->comps.count != b->comps.count || a->pins.count != b->pins.count) {
        return; /* the memcmp()s below would be meaningless */
    }

    const size_t ncomp = a->comps.count;
    const size_t npin = a->pins.count;
    const size_t nnet = a->nets.num_nets;
    const size_t nent = a->nets.num_entries;
    const size_t npoly = a->constraints.polygons.num_polys;
    const size_t nvert = a->constraints.polygons.num_vertices;

    check_bytes_equal(a->comps.x, b->comps.x, ncomp * sizeof(coord_t), "comps.x");
    check_bytes_equal(a->comps.y, b->comps.y, ncomp * sizeof(coord_t), "comps.y");
    check_bytes_equal(a->comps.half_w, b->comps.half_w, ncomp * sizeof(coord_t), "comps.half_w");
    check_bytes_equal(a->comps.half_h, b->comps.half_h, ncomp * sizeof(coord_t), "comps.half_h");
    check_bytes_equal(a->comps.height, b->comps.height, ncomp * sizeof(coord_t), "comps.height");
    check_bytes_equal(a->comps.mass, b->comps.mass, ncomp * sizeof(coord_t), "comps.mass");
    check_bytes_equal(a->comps.flags, b->comps.flags, ncomp * sizeof(comp_flags_t), "comps.flags");
    check_bytes_equal(a->comps.kind, b->comps.kind, ncomp * sizeof(comp_kind_t), "comps.kind");
    check_bytes_equal(a->comps.first_pin, b->comps.first_pin, ncomp * sizeof(place_id_t),
                      "comps.first_pin");
    check_bytes_equal(a->comps.pin_count, b->comps.pin_count, ncomp * sizeof(place_count_t),
                      "comps.pin_count");
    check_bytes_equal(a->comps.polygon_id, b->comps.polygon_id, ncomp * sizeof(place_id_t),
                      "comps.polygon_id");

    check_bytes_equal(a->pins.offset_x, b->pins.offset_x, npin * sizeof(coord_t), "pins.offset_x");
    check_bytes_equal(a->pins.offset_y, b->pins.offset_y, npin * sizeof(coord_t), "pins.offset_y");
    check_bytes_equal(a->pins.comp_id, b->pins.comp_id, npin * sizeof(place_id_t), "pins.comp_id");
    check_bytes_equal(a->pins.net_id, b->pins.net_id, npin * sizeof(place_id_t), "pins.net_id");
    check_bytes_equal(a->pins.flags, b->pins.flags, npin * sizeof(pin_flags_t), "pins.flags");
    check_bytes_equal(a->pins.max_current, b->pins.max_current, npin * sizeof(coord_t),
                      "pins.max_current");

    check_bytes_equal(a->nets.weights, b->nets.weights, nnet * sizeof(coord_t), "nets.weights");
    check_bytes_equal(a->nets.net_class, b->nets.net_class, nnet * sizeof(uint8_t), "nets.class");
    check_bytes_equal(a->nets.net_offsets, b->nets.net_offsets, (nnet + 1u) * sizeof(uint32_t),
                      "nets.offsets");
    check_bytes_equal(a->nets.net_to_pins, b->nets.net_to_pins, nent * sizeof(place_id_t),
                      "nets.to_pins");

    check_bytes_equal(a->constraints.polygons.vx, b->constraints.polygons.vx,
                      nvert * sizeof(coord_t), "poly.vx");
    check_bytes_equal(a->constraints.polygons.vy, b->constraints.polygons.vy,
                      nvert * sizeof(coord_t), "poly.vy");
    check_bytes_equal(a->constraints.polygons.poly_offsets, b->constraints.polygons.poly_offsets,
                      (npoly + 1u) * sizeof(uint32_t), "poly.offsets");
    check_bytes_equal(a->constraints.polygons.poly_kind, b->constraints.polygons.poly_kind,
                      npoly * sizeof(poly_kind_t), "poly.kind");
    check_bytes_equal(a->constraints.polygons.poly_layer_mask,
                      b->constraints.polygons.poly_layer_mask, npoly * sizeof(uint32_t),
                      "poly.layer_mask");

    const size_t ndec = a->constraints.decoupling.count;
    check_bytes_equal(a->constraints.decoupling.ic_comp, b->constraints.decoupling.ic_comp,
                      ndec * sizeof(place_id_t), "decoupling.ic");
    check_bytes_equal(a->constraints.decoupling.cap_comp, b->constraints.decoupling.cap_comp,
                      ndec * sizeof(place_id_t), "decoupling.cap");
    check_bytes_equal(a->constraints.decoupling.ic_pin, b->constraints.decoupling.ic_pin,
                      ndec * sizeof(place_id_t), "decoupling.pin");
    check_bytes_equal(a->constraints.decoupling.max_dist_sq,
                      b->constraints.decoupling.max_dist_sq, ndec * sizeof(coord_t),
                      "decoupling.max_dist_sq");
    check_bytes_equal(a->constraints.decoupling.weight, b->constraints.decoupling.weight,
                      ndec * sizeof(coord_t), "decoupling.weight");
    if (ndec > 0u) {
        check_bytes_equal(a->constraints.decoupling.first_by_comp,
                          b->constraints.decoupling.first_by_comp,
                          (ncomp + 1u) * sizeof(uint32_t), "decoupling.first_by_comp");
    }

    const size_t nth = a->constraints.thermal.count;
    check_bytes_equal(a->constraints.thermal.comp, b->constraints.thermal.comp,
                      nth * sizeof(place_id_t), "thermal.comp");
    check_bytes_equal(a->constraints.thermal.power_w, b->constraints.thermal.power_w,
                      nth * sizeof(coord_t), "thermal.power");
    check_bytes_equal(a->constraints.thermal.exclusion_radius,
                      b->constraints.thermal.exclusion_radius, nth * sizeof(coord_t),
                      "thermal.radius");
    check_bytes_equal(a->constraints.thermal.r_theta_ja, b->constraints.thermal.r_theta_ja,
                      nth * sizeof(coord_t), "thermal.r_theta");
    if (nth > 0u) {
        check_bytes_equal(a->constraints.thermal.first_by_comp,
                          b->constraints.thermal.first_by_comp,
                          (ncomp + 1u) * sizeof(uint32_t), "thermal.first_by_comp");
    }

    const size_t nsym = a->constraints.symmetry.count;
    check_bytes_equal(a->constraints.symmetry.comp_a, b->constraints.symmetry.comp_a,
                      nsym * sizeof(place_id_t), "symmetry.a");
    check_bytes_equal(a->constraints.symmetry.comp_b, b->constraints.symmetry.comp_b,
                      nsym * sizeof(place_id_t), "symmetry.b");
    check_bytes_equal(a->constraints.symmetry.axis, b->constraints.symmetry.axis,
                      nsym * sizeof(sym_axis_t), "symmetry.axis");
    check_bytes_equal(a->constraints.symmetry.weight, b->constraints.symmetry.weight,
                      nsym * sizeof(coord_t), "symmetry.weight");
    if (nsym > 0u) {
        check_bytes_equal(a->constraints.symmetry.first_by_comp,
                          b->constraints.symmetry.first_by_comp,
                          (ncomp + 1u) * sizeof(uint32_t), "symmetry.first_by_comp");
    }

    const size_t ndp = a->constraints.diffpairs.count;
    check_bytes_equal(a->constraints.diffpairs.net_p, b->constraints.diffpairs.net_p,
                      ndp * sizeof(place_id_t), "diffpair.p");
    check_bytes_equal(a->constraints.diffpairs.net_n, b->constraints.diffpairs.net_n,
                      ndp * sizeof(place_id_t), "diffpair.n");
    check_bytes_equal(a->constraints.diffpairs.max_skew_mm, b->constraints.diffpairs.max_skew_mm,
                      ndp * sizeof(coord_t), "diffpair.skew");

    CHECK(a->constraints.present == b->constraints.present);
    CHECK(a->rules.num_classes == b->rules.num_classes);
    if (a->rules.num_classes > 0u) {
        const size_t ncls = a->rules.num_classes;
        check_bytes_equal(a->rules.class_clearance, b->rules.class_clearance,
                          ncls * ncls * sizeof(coord_t), "rules.class_clearance");
    }

    CHECK(a->rules.allow_vias_under_body == b->rules.allow_vias_under_body);
    CHECK(a->rules.min_track_width == b->rules.min_track_width);
    CHECK(a->stackup.allowed_sides == b->stackup.allowed_sides);
    CHECK(a->stackup.copper_layers == b->stackup.copper_layers);
    CHECK(a->stackup.has_stackup == b->stackup.has_stackup);
    CHECK(a->stackup.ceiling_height == b->stackup.ceiling_height);
    CHECK(a->stackup.airflow_x == b->stackup.airflow_x);
    CHECK(a->stackup.airflow_y == b->stackup.airflow_y);
    CHECK(a->grid.step_fine == b->grid.step_fine);
    CHECK(a->grid.step_coarse == b->grid.step_coarse);
}

/* ========================================================================= */
/* A. round trip                                                             */
/* ========================================================================= */

#define TMP_DIR DODPLACE_TMP_DIR

static void test_round_trip(void)
{
    (void)printf("-- scene IR round trip\n");

    placer_context_t source;
    CHECK(build_demo_scene(&source));
    CHECK(source.errors == 0u);
    CHECK(source.warnings == 6u); /* NO_3D_HEIGHT, NO_MASS, NO_PIN_CURRENT,
                                   * COURTYARD_BBOX, NO_MAX_LENGTH, NO_SEGREGATION */
    CHECK(source.degraded == (DEGRADED_NO_3D_HEIGHT | DEGRADED_NO_MASS |
                              DEGRADED_NO_PIN_CURRENT | DEGRADED_COURTYARD_BBOX |
                              DEGRADED_NO_MAX_LENGTH | DEGRADED_NO_SEGREGATION));

    validation_report_t vreport;
    CHECK(placer_context_validate(&source, &vreport));

    const char *path = TMP_DIR "/round_trip.bin";
    CHECK(scene_save(&source, path));

    placer_context_t loaded;
    scene_io_report_t io;
    CHECK(scene_load(&loaded, path, &io));
    CHECK(io.errors == 0u);
    CHECK(io.sections_skipped == 1u); /* the JSON tail */
    CHECK(loaded.errors == 0u);

    check_contexts_equal(&source, &loaded);

    /* The loaded scene must validate too, and produce the same identity output. */
    CHECK(placer_context_validate(&loaded, &vreport));
    CHECK(vreport.errors == 0u);

    placer_context_destroy(&loaded);
    placer_context_destroy(&source);
}

/* ========================================================================= */
/* B. golden fixture                                                         */
/* ========================================================================= */

static void test_fixture(void)
{
    (void)printf("-- golden fixture\n");

    placer_context_t fixture;
    scene_io_report_t io;
    if (!scene_load(&fixture, DODPLACE_FIXTURE_PATH, &io)) {
        (void)printf("FAIL %s: cannot load fixture %s: %s\n", __FILE__,
                     DODPLACE_FIXTURE_PATH, io.message);
        g_failures++;
        return;
    }

    placer_context_t reference;
    CHECK(build_demo_scene(&reference));
    check_contexts_equal(&reference, &fixture);

    placer_context_destroy(&fixture);
    placer_context_destroy(&reference);
}

/* ========================================================================= */
/* C. rejection cases                                                        */
/* ========================================================================= */

static uint8_t *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (f == nullptr) {
        return nullptr;
    }
    if (fseek(f, 0L, SEEK_END) != 0) {
        (void)fclose(f);
        return nullptr;
    }
    const long n = ftell(f);
    if (n <= 0 || fseek(f, 0L, SEEK_SET) != 0) {
        (void)fclose(f);
        return nullptr;
    }
    uint8_t *data = (uint8_t *)malloc((size_t)n);
    if (data == nullptr || fread(data, 1u, (size_t)n, f) != (size_t)n) {
        free(data);
        (void)fclose(f);
        return nullptr;
    }
    (void)fclose(f);
    *size = (size_t)n;
    return data;
}

static bool write_file(const char *path, const uint8_t *data, size_t size)
{
    FILE *f = fopen(path, "wb");
    if (f == nullptr) {
        return false;
    }
    const bool ok = fwrite(data, 1u, size, f) == size;
    if (fclose(f) != 0) {
        return false;
    }
    return ok;
}

static uint16_t read_u16_le(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t read_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void write_u16_le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void write_u32_le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

/* Byte offset of section table entry `i`, field `field_offset`. */
static size_t section_field_offset(uint32_t i, uint32_t field_offset)
{
    return PLACE_SCENE_HEADER_SIZE + (size_t)i * PLACE_SCENE_SECTION_ENTRY_SIZE + field_offset;
}

static uint32_t g_rejections_run;

static void expect_rejected(const uint8_t *data, size_t size, const char *what)
{
    g_rejections_run += 1u;
    const char *path = TMP_DIR "/reject.bin";
    if (!write_file(path, data, size)) {
        (void)printf("FAIL %s: cannot stage '%s'\n", __FILE__, what);
        g_failures++;
        return;
    }
    placer_context_t ctx;
    scene_io_report_t io;
    const bool loaded = scene_load(&ctx, path, &io);
    if (loaded) {
        (void)printf("FAIL %s: '%s' was accepted but must be rejected\n", __FILE__, what);
        g_failures++;
        placer_context_destroy(&ctx);
        return;
    }
    if (ctx.scene.base != nullptr) {
        (void)printf("FAIL %s: '%s' left a half-built context behind\n", __FILE__, what);
        g_failures++;
    }
    if (io.errors == 0u) {
        (void)printf("FAIL %s: '%s' failed without reporting an error\n", __FILE__, what);
        g_failures++;
    }
    placer_context_destroy(&ctx);
}

static void test_rejections(void)
{
    (void)printf("-- malformed scene files\n");

    const char *path = TMP_DIR "/reject_source.bin";
    placer_context_t source;
    CHECK(build_demo_scene(&source));
    CHECK(scene_save(&source, path));
    placer_context_destroy(&source);

    size_t size = 0u;
    uint8_t *pristine = read_file(path, &size);
    if (pristine == nullptr) {
        (void)printf("FAIL %s: cannot read staged scene\n", __FILE__);
        g_failures++;
        return;
    }
    uint8_t *buf = (uint8_t *)malloc(size);
    CHECK(buf != nullptr);
    if (buf == nullptr) {
        free(pristine);
        return;
    }

    /* The staged file must actually be the IR version we claim to produce. */
    CHECK(memcmp(pristine, PLACE_SCENE_MAGIC, 4u) == 0);
    CHECK(read_u16_le(pristine + 4u) == PLACE_SCENE_VERSION_MAJOR);
    CHECK(read_u32_le(pristine + 12u) == PLACE_SCENE_HEADER_SIZE);

#define RESET_BUFFER() memcpy(buf, pristine, size)
#define PATCH16(off, val) RESET_BUFFER(), write_u16_le(buf + (off), (uint16_t)(val))
#define PATCH32(off, val) RESET_BUFFER(), write_u32_le(buf + (off), (uint32_t)(val))

    /* 1. bad magic */
    RESET_BUFFER();
    buf[0] = 'X';
    expect_rejected(buf, size, "bad magic");

    /* 2. unsupported major version */
    PATCH16(4u, 99u);
    expect_rejected(buf, size, "unsupported major version");

    /* 3. truncated inside the header */
    RESET_BUFFER();
    expect_rejected(buf, 64u, "truncated header");

    /* 4. truncated inside the payload */
    RESET_BUFFER();
    expect_rejected(buf, size - 8u, "truncated payload");

    /* 5. unaligned section offset */
    PATCH32(section_field_offset(0u, 4u), read_u32_le(pristine + section_field_offset(0u, 4u)) + 1u);
    expect_rejected(buf, size, "unaligned section offset");

    /* 6. declared count disagrees with the section table */
    PATCH32(24u, 999u); /* counts[SCENE_FIELD_COMPS] */
    expect_rejected(buf, size, "count mismatch");

    /* 7. required section replaced by an unknown kind */
    PATCH32(section_field_offset(0u, 0u), 9999u);
    expect_rejected(buf, size, "missing required section");

    /* 8. wrong element size for a section */
    PATCH32(section_field_offset(0u, 12u), 8u);
    expect_rejected(buf, size, "wrong element size");

    /* 9. duplicate section kind */
    PATCH32(section_field_offset(1u, 0u), read_u32_le(pristine + section_field_offset(0u, 0u)));
    expect_rejected(buf, size, "duplicate section kind");

    /* 10. unknown degraded bit in the board config */
    {
        const uint32_t nsec = read_u32_le(pristine + 16u);
        size_t cfg_off = 0u;
        bool found = false;
        for (uint32_t i = 0u; i < nsec; ++i) {
            if (read_u32_le(pristine + section_field_offset(i, 0u)) ==
                (uint32_t)SCENE_SECTION_BOARD_CONFIG) {
                cfg_off = read_u32_le(pristine + section_field_offset(i, 4u));
                found = true;
                break;
            }
        }
        CHECK(found);
        if (found) {
            PATCH32(cfg_off + offsetof(scene_board_config_t, degraded_mask), 0x80000000u);
            expect_rejected(buf, size, "unknown degraded bit");
        }
    }

    /* 11. board configuration section missing entirely */
    {
        const uint32_t nsec = read_u32_le(pristine + 16u);
        for (uint32_t i = 0u; i < nsec; ++i) {
            if (read_u32_le(pristine + section_field_offset(i, 0u)) ==
                (uint32_t)SCENE_SECTION_BOARD_CONFIG) {
                RESET_BUFFER();
                write_u32_le(buf + section_field_offset(i, 0u), 9999u);
                expect_rejected(buf, size, "board config missing");
                break;
            }
        }
    }

    /* 12. structurally valid file, but the scene itself is inconsistent: pin 0
     *     declared as belonging to component 2 breaks the grouping that
     *     finalize() enforces. Exercises the finalize/validate path, not just
     *     the header parser. */
    {
        const uint32_t nsec = read_u32_le(pristine + 16u);
        for (uint32_t i = 0u; i < nsec; ++i) {
            if (read_u32_le(pristine + section_field_offset(i, 0u)) ==
                (uint32_t)SCENE_SECTION_PIN_COMP_ID) {
                const size_t off = read_u32_le(pristine + section_field_offset(i, 4u));
                PATCH32(off, 2u);
                expect_rejected(buf, size, "pin grouping broken");
                break;
            }
        }
    }

    /* The rejection suite must actually have run every case. */
    CHECK(g_rejections_run == 12u);
    (void)printf("   %u rejection cases exercised\n", (unsigned)g_rejections_run);

#undef PATCH16
#undef PATCH32
#undef RESET_BUFFER

    free(buf);
    free(pristine);
}

/* ========================================================================= */
/* Entry point                                                               */
/* ========================================================================= */

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--emit-fixture") == 0) {
        placer_context_t ctx;
        if (!build_demo_scene(&ctx)) {
            (void)fprintf(stderr, "cannot build the demo scene\n");
            return 1;
        }
        const bool ok = scene_save(&ctx, argv[2]);
        placer_context_destroy(&ctx);
        if (!ok) {
            (void)fprintf(stderr, "cannot write '%s'\n", argv[2]);
            return 1;
        }
        (void)printf("wrote %s\n", argv[2]);
        return 0;
    }

    test_round_trip();
    test_fixture();
    test_rejections();

    if (g_failures == 0) {
        (void)printf("test_scene_io: all checks passed\n");
    } else {
        (void)printf("test_scene_io: %d check(s) failed\n", g_failures);
    }
    return g_failures;
}
