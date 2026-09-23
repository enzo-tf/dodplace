/*
 * io_sections.c - the section lists.
 *
 * The loader rebuilds the table it expects from the counts alone, and the
 * writer builds the table it will emit from the context; both go through this
 * module so the two orders can only disagree if this file is wrong. The golden
 * fixture in ir/ is what proves they do not.
 */
#include "place/io.h"
#include "io_internal.h"

#include <string.h>

static bool io_add_expected(io_expected_t *out, uint32_t *n, uint32_t max,
                         uint32_t kind, uint32_t count, uint32_t elem_size,
                         void *dest, const char *name)
{
    if (*n >= max) {
        return false;
    }
    out[*n].kind = kind;
    out[*n].count = count;
    out[*n].elem_size = elem_size;
    out[*n].dest = dest;
    out[*n].name = name;
    out[*n].required = true;
    *n += 1u;
    return true;
}

/*
 * A section added after the first release. It is written by every current
 * producer and read when present, but a file from before it existed still
 * loads: the array keeps its zero default (COMP_KIND_UNKNOWN) and the stages
 * that need the data simply find nothing to work with.
 */
static bool io_add_optional(io_expected_t *out, uint32_t *n, uint32_t max,
                         uint32_t kind, uint32_t count, uint32_t elem_size,
                         void *dest, const char *name)
{
    if (!io_add_expected(out, n, max, kind, count, elem_size, dest, name)) {
        return false;
    }
    out[*n - 1u].required = false;
    return true;
}

/*
 * The list of sections the declared counts require, in load order. A count of
 * zero means "not required"; a non-zero count must be present with exactly
 * that element count and element size.
 */
bool io_build_expected_sections(const placer_context_t *ctx, const scene_counts_t *c,
                                    io_expected_t *out, uint32_t max, uint32_t *n_out)
{
    uint32_t n = 0u;
    const constraints_table_t *ct = &ctx->constraints;

#define EXPECT(kind, count, type, member, name)                                            \
    do {                                                                                   \
        if (!io_add_expected(out, &n, max, (kind), (count), (uint32_t)sizeof(type),            \
                          (void *)(member), (name))) {                                      \
            return false;                                                                   \
        }                                                                                   \
    } while (0)

    EXPECT(SCENE_SECTION_COMP_X, c->num_comps, coord_t, ctx->comps.x, "comp.x");
    EXPECT(SCENE_SECTION_COMP_Y, c->num_comps, coord_t, ctx->comps.y, "comp.y");
    EXPECT(SCENE_SECTION_COMP_HALF_W, c->num_comps, coord_t, ctx->comps.half_w, "comp.half_w");
    EXPECT(SCENE_SECTION_COMP_HALF_H, c->num_comps, coord_t, ctx->comps.half_h, "comp.half_h");
    EXPECT(SCENE_SECTION_COMP_HEIGHT, c->num_comps, coord_t, ctx->comps.height, "comp.height");
    EXPECT(SCENE_SECTION_COMP_MASS, c->num_comps, coord_t, ctx->comps.mass, "comp.mass");
    EXPECT(SCENE_SECTION_COMP_FLAGS, c->num_comps, comp_flags_t, ctx->comps.flags, "comp.flags");
    EXPECT(SCENE_SECTION_COMP_PIN_COUNT, c->num_comps, place_count_t, ctx->comps.pin_count,
           "comp.pin_count");
    EXPECT(SCENE_SECTION_COMP_POLYGON_ID, c->num_comps, place_id_t, ctx->comps.polygon_id,
           "comp.polygon_id");
    if (!io_add_optional(out, &n, max, SCENE_SECTION_COMP_KIND, c->num_comps,
                      (uint32_t)sizeof(comp_kind_t), (void *)ctx->comps.kind, "comp.kind")) {
        return false;
    }

    if (!io_add_optional(out, &n, max, SCENE_SECTION_PIN_HALF_X, c->num_pins,
                         (uint32_t)sizeof(coord_t), (void *)ctx->pins.half_x, "pin.half_x") ||
        !io_add_optional(out, &n, max, SCENE_SECTION_PIN_HALF_Y, c->num_pins,
                         (uint32_t)sizeof(coord_t), (void *)ctx->pins.half_y, "pin.half_y")) {
        return false;
    }
    EXPECT(SCENE_SECTION_PIN_OFFSET_X, c->num_pins, coord_t, ctx->pins.offset_x, "pin.offset_x");
    EXPECT(SCENE_SECTION_PIN_OFFSET_Y, c->num_pins, coord_t, ctx->pins.offset_y, "pin.offset_y");
    EXPECT(SCENE_SECTION_PIN_COMP_ID, c->num_pins, place_id_t, ctx->pins.comp_id, "pin.comp_id");
    EXPECT(SCENE_SECTION_PIN_NET_ID, c->num_pins, place_id_t, ctx->pins.net_id, "pin.net_id");
    EXPECT(SCENE_SECTION_PIN_FLAGS, c->num_pins, pin_flags_t, ctx->pins.flags, "pin.flags");
    EXPECT(SCENE_SECTION_PIN_MAX_CURRENT, c->num_pins, coord_t, ctx->pins.max_current,
           "pin.max_current");

    EXPECT(SCENE_SECTION_NET_WEIGHT, c->num_nets, coord_t, ctx->nets.weights, "net.weight");
    EXPECT(SCENE_SECTION_NET_CLASS, c->num_nets, uint8_t, ctx->nets.net_class, "net.class");

    EXPECT(SCENE_SECTION_POLY_VX, c->num_vertices, coord_t, ct->polygons.vx, "poly.vx");
    EXPECT(SCENE_SECTION_POLY_VY, c->num_vertices, coord_t, ct->polygons.vy, "poly.vy");
    EXPECT(SCENE_SECTION_POLY_OFFSETS, (c->num_polygons > 0u) ? (c->num_polygons + 1u) : 0u,
           uint32_t, ct->polygons.poly_offsets, "poly.offsets");
    EXPECT(SCENE_SECTION_POLY_KIND, c->num_polygons, poly_kind_t, ct->polygons.poly_kind,
           "poly.kind");
    EXPECT(SCENE_SECTION_POLY_LAYER_MASK, c->num_polygons, uint32_t, ct->polygons.poly_layer_mask,
           "poly.layer_mask");

    EXPECT(SCENE_SECTION_DECOUPLING_IC, c->num_decoupling, place_id_t, ct->decoupling.ic_comp,
           "decoupling.ic");
    EXPECT(SCENE_SECTION_DECOUPLING_CAP, c->num_decoupling, place_id_t, ct->decoupling.cap_comp,
           "decoupling.cap");
    EXPECT(SCENE_SECTION_DECOUPLING_PIN, c->num_decoupling, place_id_t, ct->decoupling.ic_pin,
           "decoupling.pin");
    EXPECT(SCENE_SECTION_DECOUPLING_MAX_DIST_SQ, c->num_decoupling, coord_t,
           ct->decoupling.max_dist_sq, "decoupling.max_dist_sq");
    EXPECT(SCENE_SECTION_DECOUPLING_WEIGHT, c->num_decoupling, coord_t, ct->decoupling.weight,
           "decoupling.weight");

    EXPECT(SCENE_SECTION_THERMAL_COMP, c->num_thermal, place_id_t, ct->thermal.comp, "thermal.comp");
    EXPECT(SCENE_SECTION_THERMAL_POWER, c->num_thermal, coord_t, ct->thermal.power_w,
           "thermal.power");
    EXPECT(SCENE_SECTION_THERMAL_RADIUS, c->num_thermal, coord_t, ct->thermal.exclusion_radius,
           "thermal.radius");
    EXPECT(SCENE_SECTION_THERMAL_R_THETA, c->num_thermal, coord_t, ct->thermal.r_theta_ja,
           "thermal.r_theta");

    EXPECT(SCENE_SECTION_SYMMETRY_A, c->num_symmetry, place_id_t, ct->symmetry.comp_a, "symmetry.a");
    EXPECT(SCENE_SECTION_SYMMETRY_B, c->num_symmetry, place_id_t, ct->symmetry.comp_b, "symmetry.b");
    EXPECT(SCENE_SECTION_SYMMETRY_AXIS, c->num_symmetry, sym_axis_t, ct->symmetry.axis,
           "symmetry.axis");
    EXPECT(SCENE_SECTION_SYMMETRY_WEIGHT, c->num_symmetry, coord_t, ct->symmetry.weight,
           "symmetry.weight");

    EXPECT(SCENE_SECTION_DIFFPAIR_P, c->num_diffpairs, place_id_t, ct->diffpairs.net_p, "diffpair.p");
    EXPECT(SCENE_SECTION_DIFFPAIR_N, c->num_diffpairs, place_id_t, ct->diffpairs.net_n, "diffpair.n");
    EXPECT(SCENE_SECTION_DIFFPAIR_SKEW, c->num_diffpairs, coord_t, ct->diffpairs.max_skew_mm,
           "diffpair.skew");

    EXPECT(SCENE_SECTION_CLEARANCE_MATRIX, 0u, coord_t, ctx->rules.class_clearance, "clearance");
    EXPECT(SCENE_SECTION_BOARD_CONFIG, 0u, scene_board_config_t, nullptr, "board_config");

#undef EXPECT

    /* The clearance matrix and the board config are handled explicitly: their
     * count is not part of the SoA counts and the config has no destination
     * array. Fix up their entries here. */
    for (uint32_t i = 0u; i < n; ++i) {
        if (out[i].kind == SCENE_SECTION_CLEARANCE_MATRIX) {
            const uint32_t ncls = c->num_net_classes;
            if (ctx->rules.class_clearance == nullptr) {
                return false;
            }
            out[i].count = ncls * ncls;
        } else if (out[i].kind == SCENE_SECTION_BOARD_CONFIG) {
            out[i].count = 1u;
        }
    }

    *n_out = n;
    return true;
}


static bool io_add_save(io_save_t *out, uint32_t *n, uint32_t max,
                     uint32_t kind, uint32_t count, const void *src, uint32_t elem_size)
{
    if (count == 0u) {
        return true; /* empty tables are simply not written */
    }
    if (*n >= max) {
        return false;
    }
    out[*n].kind = kind;
    out[*n].count = count;
    out[*n].elem_size = elem_size;
    out[*n].src = src;
    *n += 1u;
    return true;
}

bool io_build_save_sections(const placer_context_t *ctx, io_save_t *out,
                                uint32_t max, uint32_t *n_out)
{
    uint32_t n = 0u;
    const scene_counts_t *c = &ctx->ingest.counts;
    const constraints_table_t *ct = &ctx->constraints;
    const uint32_t ncomp = ctx->comps.count;
    const uint32_t npin = ctx->pins.count;
    const uint32_t nnet = ctx->nets.num_nets;
    const uint32_t npoly = ct->polygons.num_polys;
    const uint32_t nvert = ct->polygons.num_vertices;

#define ADD(kind, count, src, type)                                                        \
    do {                                                                                   \
        if (!io_add_save(out, &n, max, (kind), (count), (src), (uint32_t)sizeof(type))) {      \
            return false;                                                                   \
        }                                                                                   \
    } while (0)

    ADD(SCENE_SECTION_COMP_X, ncomp, ctx->comps.x, coord_t);
    ADD(SCENE_SECTION_COMP_Y, ncomp, ctx->comps.y, coord_t);
    ADD(SCENE_SECTION_COMP_HALF_W, ncomp, ctx->comps.half_w, coord_t);
    ADD(SCENE_SECTION_COMP_HALF_H, ncomp, ctx->comps.half_h, coord_t);
    ADD(SCENE_SECTION_COMP_HEIGHT, ncomp, ctx->comps.height, coord_t);
    ADD(SCENE_SECTION_COMP_MASS, ncomp, ctx->comps.mass, coord_t);
    ADD(SCENE_SECTION_COMP_FLAGS, ncomp, ctx->comps.flags, comp_flags_t);
    ADD(SCENE_SECTION_COMP_PIN_COUNT, ncomp, ctx->comps.pin_count, place_count_t);
    ADD(SCENE_SECTION_COMP_POLYGON_ID, ncomp, ctx->comps.polygon_id, place_id_t);
    ADD(SCENE_SECTION_COMP_KIND, ncomp, ctx->comps.kind, comp_kind_t);

    ADD(SCENE_SECTION_PIN_OFFSET_X, npin, ctx->pins.offset_x, coord_t);
    ADD(SCENE_SECTION_PIN_OFFSET_Y, npin, ctx->pins.offset_y, coord_t);
    ADD(SCENE_SECTION_PIN_COMP_ID, npin, ctx->pins.comp_id, place_id_t);
    ADD(SCENE_SECTION_PIN_NET_ID, npin, ctx->pins.net_id, place_id_t);
    ADD(SCENE_SECTION_PIN_FLAGS, npin, ctx->pins.flags, pin_flags_t);
    ADD(SCENE_SECTION_PIN_MAX_CURRENT, npin, ctx->pins.max_current, coord_t);
    /* Same order as the Python SECTION_DEFS: the two writers have to agree
     * byte for byte, and the golden fixture is what proves it. */
    ADD(SCENE_SECTION_PIN_HALF_X, npin, ctx->pins.half_x, coord_t);
    ADD(SCENE_SECTION_PIN_HALF_Y, npin, ctx->pins.half_y, coord_t);

    ADD(SCENE_SECTION_NET_WEIGHT, nnet, ctx->nets.weights, coord_t);
    ADD(SCENE_SECTION_NET_CLASS, nnet, ctx->nets.net_class, uint8_t);

    ADD(SCENE_SECTION_POLY_VX, nvert, ct->polygons.vx, coord_t);
    ADD(SCENE_SECTION_POLY_VY, nvert, ct->polygons.vy, coord_t);
    ADD(SCENE_SECTION_POLY_OFFSETS, (npoly > 0u) ? (npoly + 1u) : 0u, ct->polygons.poly_offsets,
        uint32_t);
    ADD(SCENE_SECTION_POLY_KIND, npoly, ct->polygons.poly_kind, poly_kind_t);
    ADD(SCENE_SECTION_POLY_LAYER_MASK, npoly, ct->polygons.poly_layer_mask, uint32_t);

    ADD(SCENE_SECTION_DECOUPLING_IC, ct->decoupling.count, ct->decoupling.ic_comp, place_id_t);
    ADD(SCENE_SECTION_DECOUPLING_CAP, ct->decoupling.count, ct->decoupling.cap_comp, place_id_t);
    ADD(SCENE_SECTION_DECOUPLING_PIN, ct->decoupling.count, ct->decoupling.ic_pin, place_id_t);
    ADD(SCENE_SECTION_DECOUPLING_MAX_DIST_SQ, ct->decoupling.count, ct->decoupling.max_dist_sq,
        coord_t);
    ADD(SCENE_SECTION_DECOUPLING_WEIGHT, ct->decoupling.count, ct->decoupling.weight, coord_t);

    ADD(SCENE_SECTION_THERMAL_COMP, ct->thermal.count, ct->thermal.comp, place_id_t);
    ADD(SCENE_SECTION_THERMAL_POWER, ct->thermal.count, ct->thermal.power_w, coord_t);
    ADD(SCENE_SECTION_THERMAL_RADIUS, ct->thermal.count, ct->thermal.exclusion_radius, coord_t);
    ADD(SCENE_SECTION_THERMAL_R_THETA, ct->thermal.count, ct->thermal.r_theta_ja, coord_t);

    ADD(SCENE_SECTION_SYMMETRY_A, ct->symmetry.count, ct->symmetry.comp_a, place_id_t);
    ADD(SCENE_SECTION_SYMMETRY_B, ct->symmetry.count, ct->symmetry.comp_b, place_id_t);
    ADD(SCENE_SECTION_SYMMETRY_AXIS, ct->symmetry.count, ct->symmetry.axis, sym_axis_t);
    ADD(SCENE_SECTION_SYMMETRY_WEIGHT, ct->symmetry.count, ct->symmetry.weight, coord_t);

    ADD(SCENE_SECTION_DIFFPAIR_P, ct->diffpairs.count, ct->diffpairs.net_p, place_id_t);
    ADD(SCENE_SECTION_DIFFPAIR_N, ct->diffpairs.count, ct->diffpairs.net_n, place_id_t);
    ADD(SCENE_SECTION_DIFFPAIR_SKEW, ct->diffpairs.count, ct->diffpairs.max_skew_mm, coord_t);

#undef ADD

    if (ctx->rules.class_clearance != nullptr && ctx->rules.num_classes > 0u) {
        const uint32_t ncls = ctx->rules.num_classes;
        if (!io_add_save(out, &n, max, SCENE_SECTION_CLEARANCE_MATRIX, ncls * ncls,
                      ctx->rules.class_clearance, (uint32_t)sizeof(coord_t))) {
            return false;
        }
    }

    /* Board config: derived from the context's authoritative fields by
     * scene_save, which appends it as the last payload section. */
    if (c->num_net_classes != ctx->rules.num_classes) {
        /* counts and the actual matrix must agree */
        return false;
    }

    *n_out = n;
    return true;
}

