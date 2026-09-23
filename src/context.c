#include "place/context.h"
#include "context_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Reserved polygon slots so finalize can synthesise a board outline from the
 * component bounding box when Edge.Cuts data is absent. */
#define PLACE_FALLBACK_POLYGON_SLOTS 1u
#define PLACE_FALLBACK_VERTEX_SLOTS  8u

/* Head room added to every estimate to absorb allocator padding and the
 * derived CSR indices built during finalize. */
#define PLACE_ARENA_SLACK_BYTES 4096u

/* ========================================================================= */
/* Small helpers                                                             */
/* ========================================================================= */

size_t ctx_align_up(size_t value, size_t alignment)
{
    return (value + (alignment - 1u)) & ~(alignment - 1u);
}

/*
 * Records a degraded-mode fallback. A category is counted once, which keeps
 * the invariant `warnings == popcount(degraded)` true by construction.
 */
void ctx_mark_degraded(placer_context_t *ctx, uint32_t bit)
{
    if (ctx == nullptr || (ctx->degraded & bit) != 0u) {
        return;
    }
    ctx->degraded |= bit;
    ctx->warnings += 1u;
}

static bool is_digit_ascii(char c)
{
    return c >= '0' && c <= '9';
}

static char lower_ascii(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return (char)(c - 'A' + 'a');
    }
    return c;
}

static bool contains_ci(const char *hay, const char *needle)
{
    if (hay == nullptr || needle == nullptr) {
        return false;
    }
    const size_t n = strlen(needle);
    if (n == 0u) {
        return false;
    }
    for (const char *p = hay; *p != '\0'; ++p) {
        size_t i = 0u;
        while (i < n && p[i] != '\0' && lower_ascii(p[i]) == lower_ascii(needle[i])) {
            i++;
        }
        if (i == n) {
            return true;
        }
    }
    return false;
}

static bool starts_ci(const char *s, const char *prefix)
{
    if (s == nullptr || prefix == nullptr) {
        return false;
    }
    for (size_t i = 0u; prefix[i] != '\0'; ++i) {
        if (s[i] == '\0' || lower_ascii(s[i]) != lower_ascii(prefix[i])) {
            return false;
        }
    }
    return true;
}

static bool ends_ci(const char *s, const char *suffix)
{
    if (s == nullptr || suffix == nullptr) {
        return false;
    }
    const size_t ls = strlen(s);
    const size_t lx = strlen(suffix);
    if (lx > ls) {
        return false;
    }
    return starts_ci(s + (ls - lx), suffix);
}

/*
 * Degraded-mode electrical-role inference (block 2 fallback). Ground is tested
 * before power because "VSS" would otherwise match a rail pattern.
 */
pin_flags_t ctx_infer_pin_flags(const char *name)
{
    static const char *const ground_marks[] = {"gnd", "vss"};
    static const char *const clock_marks[] = {"clk", "clock", "xtal", "osc"};
    static const char *const rail_prefixes[] = {
        "vcc", "vdd", "vbus", "vbat", "vin", "vcore", "vddio",
    };

    if (name == nullptr) {
        return 0u;
    }

    for (size_t i = 0u; i < sizeof(ground_marks) / sizeof(ground_marks[0]); ++i) {
        if (contains_ci(name, ground_marks[i])) {
            return (pin_flags_t)PIN_GROUND;
        }
    }
    for (size_t i = 0u; i < sizeof(clock_marks) / sizeof(clock_marks[0]); ++i) {
        if (contains_ci(name, clock_marks[i])) {
            return (pin_flags_t)PIN_CLOCK;
        }
    }
    for (size_t i = 0u; i < sizeof(rail_prefixes) / sizeof(rail_prefixes[0]); ++i) {
        if (starts_ci(name, rail_prefixes[i])) {
            return (pin_flags_t)PIN_POWER;
        }
    }
    if (name[0] == '+' && is_digit_ascii(name[1])) {
        return (pin_flags_t)PIN_POWER; /* +3V3, +5V ... */
    }
    if (is_digit_ascii(name[0]) && contains_ci(name, "v")) {
        return (pin_flags_t)PIN_POWER; /* 3V3, 1V8, 5V ... */
    }
    if (ends_ci(name, "_p") || ends_ci(name, "-p")) {
        return (pin_flags_t)PIN_DIFF_P;
    }
    if (ends_ci(name, "_n") || ends_ci(name, "-n")) {
        return (pin_flags_t)PIN_DIFF_N;
    }
    return 0u; /* plain signal */
}

/* Round to the nearest multiple of 90 degrees and fold into 0..3. */
unsigned ctx_snap_orientation(coord_t degrees)
{
    const coord_t quarters = degrees / 90.0f;
    int q = (int)((quarters >= 0.0f) ? quarters + 0.5f : quarters - 0.5f);
    q %= 4;
    if (q < 0) {
        q += 4;
    }
    return (unsigned)q;
}


size_t ctx_estimate_array(size_t element_size, size_t count)
{
    return ctx_align_up(element_size * count, PLACE_CACHELINE);
}

/* ========================================================================= */
/* Validation report                                                         */
/* ========================================================================= */

void placer_options_defaults(placer_options_t *opt)
{
    if (opt == nullptr) {
        return;
    }
    memset(opt, 0, sizeof *opt);
    opt->grid_fine = 0.1f;
    opt->grid_coarse = 0.5f;
    opt->courtyard_fallback_margin = 0.25f;
    opt->global_clearance = 0.2f;
    opt->courtyard_clearance = 0.2f;
    opt->allowed_sides = SIDE_BOTH;
}

/* ========================================================================= */
/* Sizing estimates                                                          */
/* ========================================================================= */

size_t placer_context_estimate_scene_bytes(const scene_counts_t *counts)
{
    if (counts == nullptr) {
        return 0u;
    }

    const size_t ncomp = counts->num_comps;
    const size_t npin = counts->num_pins;
    const size_t nnet = counts->num_nets;
    const size_t nent = counts->num_net_entries;
    const size_t npoly = (size_t)counts->num_polygons + PLACE_FALLBACK_POLYGON_SLOTS;
    const size_t nvert = (size_t)counts->num_vertices + PLACE_FALLBACK_VERTEX_SLOTS;
    const size_t nidx = ncomp + 1u; /* per-component CSR index arrays */

    size_t bytes = 0u;

    /* Components: x, y, half_w, half_h, height, mass, flags, first_pin,
     * pin_count, polygon_id. */
    bytes += ctx_estimate_array(sizeof(coord_t), ncomp) * 6u;
    bytes += ctx_estimate_array(sizeof(comp_flags_t), ncomp);
    bytes += ctx_estimate_array(sizeof(place_id_t), ncomp) * 2u;
    bytes += ctx_estimate_array(sizeof(place_count_t), ncomp);

    /* Pins: offset_x, offset_y, max_current, comp_id, net_id, flags. */
    bytes += ctx_estimate_array(sizeof(coord_t), npin) * 3u;
    bytes += ctx_estimate_array(sizeof(place_id_t), npin) * 2u;
    bytes += ctx_estimate_array(sizeof(pin_flags_t), npin);

    /* Net CSR + per-net attributes. */
    bytes += ctx_estimate_array(sizeof(uint32_t), nnet + 1u);
    bytes += ctx_estimate_array(sizeof(place_id_t), nent);
    bytes += ctx_estimate_array(sizeof(coord_t), nnet);
    bytes += ctx_estimate_array(sizeof(uint8_t), nnet);

    /* Sparse constraint tables, plus their CSR index when non-empty. */
    if (counts->num_decoupling > 0u) {
        bytes += ctx_estimate_array(sizeof(place_id_t), counts->num_decoupling) * 3u;
        bytes += ctx_estimate_array(sizeof(coord_t), counts->num_decoupling) * 2u;
        bytes += ctx_estimate_array(sizeof(uint32_t), nidx);
    }
    if (counts->num_thermal > 0u) {
        bytes += ctx_estimate_array(sizeof(place_id_t), counts->num_thermal);
        bytes += ctx_estimate_array(sizeof(coord_t), counts->num_thermal) * 3u;
        bytes += ctx_estimate_array(sizeof(uint32_t), nidx);
    }
    if (counts->num_symmetry > 0u) {
        bytes += ctx_estimate_array(sizeof(place_id_t), counts->num_symmetry) * 2u;
        bytes += ctx_estimate_array(sizeof(sym_axis_t), counts->num_symmetry);
        bytes += ctx_estimate_array(sizeof(coord_t), counts->num_symmetry);
        bytes += ctx_estimate_array(sizeof(uint32_t), nidx);
    }
    if (counts->num_diffpairs > 0u) {
        bytes += ctx_estimate_array(sizeof(place_id_t), counts->num_diffpairs) * 2u;
        bytes += ctx_estimate_array(sizeof(coord_t), counts->num_diffpairs);
    }

    /* Polygon pool: vx, vy, offsets, kind, layer mask. */
    bytes += ctx_estimate_array(sizeof(coord_t), nvert) * 2u;
    bytes += ctx_estimate_array(sizeof(uint32_t), npoly + 1u);
    bytes += ctx_estimate_array(sizeof(poly_kind_t), npoly);
    bytes += ctx_estimate_array(sizeof(uint32_t), npoly);

    /* Optional net-class clearance matrix (num_classes^2). */
    if (counts->num_net_classes > 0u) {
        const size_t ncls = (size_t)counts->num_net_classes;
        bytes += ctx_estimate_array(sizeof(coord_t), ncls * ncls);
    }

    return bytes + PLACE_ARENA_SLACK_BYTES;
}

size_t placer_context_estimate_scratch_bytes(const scene_counts_t *counts)
{
    /*
     * The scratch arena holds finalize's CSR cursors and then the whole solver
     * state, which is dominated by two per-pin tables:
     *   rotation tables  8 bytes per pin and per orientation, four of them
     *   ratsnest         4 coordinates + a net id per segment, one per pin
     * plus roughly 128 bytes per component (clusters, best-layout snapshot,
     * grid entries) and a bounded pair of spatial grids.
     */
    const size_t grid_budget = 64u * 1024u; /* both grids, cell count capped */
    size_t bytes = PLACE_ARENA_SLACK_BYTES * 4u + grid_budget;
    if (counts != nullptr) {
        bytes += ctx_estimate_array(sizeof(coord_t), (size_t)counts->num_pins * 4u) * 8u;
        bytes += ctx_estimate_array(sizeof(coord_t), (size_t)counts->num_pins) * 5u;
        bytes += ctx_estimate_array(sizeof(coord_t), (size_t)counts->num_comps * 32u);
        /* the spatial grids' item lists: exact, and larger than one entry per
         * part because a footprint spans however many cells its box covers */
        bytes += ctx_estimate_array(sizeof(uint32_t), (size_t)counts->num_comps * 16u);
        bytes += ctx_estimate_array(sizeof(uint32_t), (size_t)counts->num_pins * 8u);
        bytes += ctx_estimate_array(sizeof(uint32_t), (size_t)counts->num_nets + 1u);
    }
    return bytes;
}

/* ========================================================================= */
/* Pass 1 - allocation                                                       */
/* ========================================================================= */

#define ALLOC_ARR(ptr, T, n)                                                              \
    do {                                                                                  \
        (ptr) = (T *)arena_alloc(&ctx->scene, sizeof(T) * (size_t)(n), PLACE_CACHELINE);   \
        if ((ptr) == nullptr) {                                                           \
            return false;                                                                 \
        }                                                                                 \
    } while (0)

#define ALLOC_ARR0(ptr, T, n)                                                             \
    do {                                                                                  \
        (ptr) = (T *)arena_alloc0(&ctx->scene, sizeof(T) * (size_t)(n), PLACE_CACHELINE);  \
        if ((ptr) == nullptr) {                                                           \
            return false;                                                                 \
        }                                                                                 \
    } while (0)

/*
 * Pre-allocates every array of the scene from the scene arena, in the order the
 * solver traverses them, so hot data ends up contiguous. All allocations are
 * cache-line aligned: a 32-byte AVX2 load of x[i..i+8] never straddles a line,
 * and no array shares a line with the next one.
 */
static bool alloc_scene_arrays(placer_context_t *ctx)
{
    const scene_counts_t *c = &ctx->ingest.counts;
    components_soa_t *cs = &ctx->comps;
    pins_soa_t *ps = &ctx->pins;
    netlist_csr_t *ns = &ctx->nets;
    constraints_table_t *ct = &ctx->constraints;
    polygon_pool_t *pp = &ct->polygons;

    /* --- components ------------------------------------------------------- */
    ALLOC_ARR(cs->x, coord_t, c->num_comps);
    ALLOC_ARR(cs->y, coord_t, c->num_comps);
    ALLOC_ARR(cs->half_w, coord_t, c->num_comps);
    ALLOC_ARR(cs->half_h, coord_t, c->num_comps);
    ALLOC_ARR(cs->height, coord_t, c->num_comps);
    ALLOC_ARR(cs->mass, coord_t, c->num_comps);
    ALLOC_ARR(cs->flags, comp_flags_t, c->num_comps);
    ALLOC_ARR0(cs->kind, comp_kind_t, c->num_comps);
    ALLOC_ARR(cs->first_pin, place_id_t, c->num_comps);
    ALLOC_ARR(cs->pin_count, place_count_t, c->num_comps);
    ALLOC_ARR(cs->polygon_id, place_id_t, c->num_comps);
    cs->count = 0u;
    cs->capacity = c->num_comps;

    /* --- pins ------------------------------------------------------------- */
    ALLOC_ARR(ps->offset_x, coord_t, c->num_pins);
    ALLOC_ARR(ps->offset_y, coord_t, c->num_pins);
    ALLOC_ARR(ps->comp_id, place_id_t, c->num_pins);
    ALLOC_ARR(ps->net_id, place_id_t, c->num_pins);
    ALLOC_ARR(ps->flags, pin_flags_t, c->num_pins);
    ALLOC_ARR(ps->max_current, coord_t, c->num_pins);
    ALLOC_ARR(ps->half_x, coord_t, c->num_pins);
    ALLOC_ARR(ps->half_y, coord_t, c->num_pins);
    ps->count = 0u;
    ps->capacity = c->num_pins;

    /* --- net CSR ---------------------------------------------------------- */
    ALLOC_ARR0(ns->net_offsets, uint32_t, c->num_nets + 1u); /* counters, then prefix sum */
    ALLOC_ARR(ns->net_to_pins, place_id_t, c->num_net_entries);
    ALLOC_ARR(ns->weights, coord_t, c->num_nets);
    ALLOC_ARR(ns->net_class, uint8_t, c->num_nets);
    ns->num_nets = 0u;
    ns->num_entries = 0u;

    /* --- polygons (+1 polygon / +8 vertices for a synthetic outline) ------- */
    const uint32_t poly_cap = c->num_polygons + PLACE_FALLBACK_POLYGON_SLOTS;
    const uint32_t vert_cap = c->num_vertices + PLACE_FALLBACK_VERTEX_SLOTS;
    ALLOC_ARR(pp->vx, coord_t, vert_cap);
    ALLOC_ARR(pp->vy, coord_t, vert_cap);
    ALLOC_ARR0(pp->poly_offsets, uint32_t, (size_t)poly_cap + 1u);
    ALLOC_ARR(pp->poly_kind, poly_kind_t, poly_cap);
    ALLOC_ARR(pp->poly_layer_mask, uint32_t, poly_cap);
    pp->num_polys = 0u;
    pp->num_vertices = 0u;

    /* --- sparse constraint tables ----------------------------------------- */
    ct->decoupling.count = 0u;
    ct->decoupling.capacity = c->num_decoupling;
    ct->decoupling.first_by_comp = nullptr;
    if (c->num_decoupling > 0u) {
        ALLOC_ARR(ct->decoupling.ic_comp, place_id_t, c->num_decoupling);
        ALLOC_ARR(ct->decoupling.cap_comp, place_id_t, c->num_decoupling);
        ALLOC_ARR(ct->decoupling.ic_pin, place_id_t, c->num_decoupling);
        ALLOC_ARR(ct->decoupling.max_dist_sq, coord_t, c->num_decoupling);
        ALLOC_ARR(ct->decoupling.weight, coord_t, c->num_decoupling);
    }

    ct->thermal.count = 0u;
    ct->thermal.capacity = c->num_thermal;
    ct->thermal.first_by_comp = nullptr;
    if (c->num_thermal > 0u) {
        ALLOC_ARR(ct->thermal.comp, place_id_t, c->num_thermal);
        ALLOC_ARR(ct->thermal.power_w, coord_t, c->num_thermal);
        ALLOC_ARR(ct->thermal.exclusion_radius, coord_t, c->num_thermal);
        ALLOC_ARR(ct->thermal.r_theta_ja, coord_t, c->num_thermal);
    }

    ct->symmetry.count = 0u;
    ct->symmetry.capacity = c->num_symmetry;
    ct->symmetry.first_by_comp = nullptr;
    if (c->num_symmetry > 0u) {
        ALLOC_ARR(ct->symmetry.comp_a, place_id_t, c->num_symmetry);
        ALLOC_ARR(ct->symmetry.comp_b, place_id_t, c->num_symmetry);
        ALLOC_ARR(ct->symmetry.axis, sym_axis_t, c->num_symmetry);
        ALLOC_ARR(ct->symmetry.weight, coord_t, c->num_symmetry);
    }

    ct->diffpairs.count = 0u;
    ct->diffpairs.capacity = c->num_diffpairs;
    if (c->num_diffpairs > 0u) {
        ALLOC_ARR(ct->diffpairs.net_p, place_id_t, c->num_diffpairs);
        ALLOC_ARR(ct->diffpairs.net_n, place_id_t, c->num_diffpairs);
        ALLOC_ARR(ct->diffpairs.max_skew_mm, coord_t, c->num_diffpairs);
    }

    /* Optional net-class clearance matrix, arena-owned like every other array. */
    if (c->num_net_classes > 0u && c->num_net_classes <= UINT8_MAX) {
        const size_t ncls = (size_t)c->num_net_classes;
        ALLOC_ARR(ctx->rules.class_clearance, coord_t, ncls * ncls);
        ctx->rules.num_classes = (uint8_t)c->num_net_classes;
    }

    ct->present = 0u;
    return true;
}

bool placer_context_begin(placer_context_t *ctx, const scene_counts_t *counts,
                          const placer_options_t *opt)
{
    if (ctx == nullptr || counts == nullptr) {
        return false;
    }
    memset(ctx, 0, sizeof *ctx);

    /* Overlay caller options on the documented defaults: 0 means "default". */
    placer_options_t merged;
    placer_options_defaults(&merged);
    if (opt != nullptr) {
        if (opt->grid_origin_x != 0.0f) {
            merged.grid_origin_x = opt->grid_origin_x;
        }
        if (opt->grid_origin_y != 0.0f) {
            merged.grid_origin_y = opt->grid_origin_y;
        }
        if (opt->grid_fine > 0.0f) {
            merged.grid_fine = opt->grid_fine;
        }
        if (opt->grid_coarse > 0.0f) {
            merged.grid_coarse = opt->grid_coarse;
        }
        if (opt->courtyard_fallback_margin > 0.0f) {
            merged.courtyard_fallback_margin = opt->courtyard_fallback_margin;
        }
        if (opt->global_clearance > 0.0f) {
            merged.global_clearance = opt->global_clearance;
        }
        if (opt->courtyard_clearance > 0.0f) {
            merged.courtyard_clearance = opt->courtyard_clearance;
        }
        if (opt->allowed_sides != 0u) {
            merged.allowed_sides = opt->allowed_sides;
        }
        merged.disable_mask = opt->disable_mask;
        merged.scene_capacity = opt->scene_capacity;
        merged.scratch_capacity = opt->scratch_capacity;
    }

    ctx->ingest.counts = *counts;
    ctx->ingest.opt = merged;
    ctx->ingest.last_pin_comp = 0u;
    ctx->ingest.has_pins = false;
    ctx->ingest.finalized = false;

    /* Clamp an inconsistent declaration instead of over-allocating on it. */
    if (ctx->ingest.counts.num_net_entries > ctx->ingest.counts.num_pins) {
        ctx->errors += 1u;
        ctx->ingest.counts.num_net_entries = ctx->ingest.counts.num_pins;
    }
    if (ctx->ingest.counts.num_net_classes > UINT8_MAX) {
        ctx->errors += 1u;
        ctx->ingest.counts.num_net_classes = 0u;
    }

    /* Board-level defaults; callers may overwrite these fields directly
     * between begin() and finalize() to inject optional board data. */
    ctx->grid.origin_x = merged.grid_origin_x;
    ctx->grid.origin_y = merged.grid_origin_y;
    ctx->grid.step_fine = merged.grid_fine;
    ctx->grid.step_coarse = merged.grid_coarse;
    ctx->rules.global_clearance = merged.global_clearance;
    ctx->rules.courtyard_clearance = merged.courtyard_clearance;
    ctx->rules.min_track_width = 0.2f;
    ctx->rules.class_clearance = nullptr;
    ctx->rules.num_classes = 0u;
    ctx->rules.allow_vias_under_body = true;
    ctx->stackup.allowed_sides = merged.allowed_sides;
    ctx->stackup.copper_layers = 0u;
    ctx->stackup.has_stackup = false;
    ctx->stackup.total_thickness = 0.0f;
    ctx->stackup.ceiling_height = 0.0f;
    ctx->stackup.airflow_x = 0.0f;
    ctx->stackup.airflow_y = 0.0f;

    const bool auto_scene = (merged.scene_capacity == 0u);
    const bool auto_scratch = (merged.scratch_capacity == 0u);
    size_t scene_cap = auto_scene ? placer_context_estimate_scene_bytes(counts)
                                  : merged.scene_capacity;
    const size_t scratch_cap = auto_scratch ? placer_context_estimate_scratch_bytes(counts)
                                            : merged.scratch_capacity;

    if (!arena_init(&ctx->scene, scene_cap)) {
        ctx->errors += 1u;
        return false;
    }

    /* The estimate is a strict mirror of alloc_scene_arrays; if it ever drifts,
     * auto-sizing doubles the arena and retries rather than failing. Explicit
     * capacities are honoured as-is: the caller owns that decision. */
    for (unsigned attempt = 0u; attempt < 6u; ++attempt) {
        if (alloc_scene_arrays(ctx)) {
            if (!arena_init(&ctx->scratch, scratch_cap)) {
                arena_destroy(&ctx->scene);
                ctx->errors += 1u;
                return false;
            }
            return true;
        }
        if (!auto_scene) {
            break;
        }
        scene_cap *= 2u;
        arena_destroy(&ctx->scene);
        if (!arena_init(&ctx->scene, scene_cap)) {
            break;
        }
    }

    arena_destroy(&ctx->scene);
    ctx->errors += 1u;
    return false;
}

void placer_context_destroy(placer_context_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }
    arena_destroy(&ctx->scene);
    arena_destroy(&ctx->scratch);
    memset(ctx, 0, sizeof *ctx);
}

/*
 * Bulk ingestion path (scene.bin loader). The caller certifies that every array
 * pre-allocated by begin() has been filled in place; `counts` becomes the new
 * logical size of the scene and the ceiling that finalize() checks against.
 * Derived data is deliberately NOT adopted: finalize() recomputes it, so the
 * CSR and the per-component indices can never disagree with the raw arrays.
 */
bool placer_context_adopt(placer_context_t *ctx, const scene_counts_t *counts)
{
    if (ctx == nullptr || counts == nullptr) {
        return false;
    }
    if (ctx->ingest.finalized || ctx->scene.base == nullptr) {
        if (ctx != nullptr) {
            ctx->errors += 1u;
        }
        return false;
    }

    /* Nothing may exceed what begin() reserved. */
    if (counts->num_comps > ctx->comps.capacity ||
        counts->num_pins > ctx->pins.capacity ||
        counts->num_nets > ctx->ingest.counts.num_nets ||
        counts->num_net_entries > ctx->ingest.counts.num_net_entries ||
        counts->num_polygons > ctx->ingest.counts.num_polygons ||
        counts->num_vertices > ctx->ingest.counts.num_vertices ||
        counts->num_decoupling > ctx->constraints.decoupling.capacity ||
        counts->num_thermal > ctx->constraints.thermal.capacity ||
        counts->num_symmetry > ctx->constraints.symmetry.capacity ||
        counts->num_diffpairs > ctx->constraints.diffpairs.capacity ||
        counts->num_net_classes > UINT8_MAX ||
        (counts->num_net_classes > 0u && ctx->rules.class_clearance == nullptr)) {
        ctx->errors += 1u;
        return false;
    }

    ctx->comps.count = counts->num_comps;
    ctx->pins.count = counts->num_pins;
    ctx->nets.num_nets = counts->num_nets;
    ctx->nets.num_entries = 0u; /* rebuilt by finalize from pins.net_id */
    ctx->constraints.decoupling.count = counts->num_decoupling;
    ctx->constraints.thermal.count = counts->num_thermal;
    ctx->constraints.symmetry.count = counts->num_symmetry;
    ctx->constraints.diffpairs.count = counts->num_diffpairs;
    ctx->constraints.polygons.num_polys = counts->num_polygons;
    ctx->constraints.polygons.num_vertices = counts->num_vertices;
    ctx->rules.num_classes = (uint8_t)counts->num_net_classes;

    ctx->ingest.counts = *counts;
    ctx->ingest.has_pins = counts->num_pins > 0u;
    ctx->ingest.last_pin_comp = 0u;
    return true;
}

/* ========================================================================= */
/* Pass 2 - fill                                                             */
/* ========================================================================= */

place_id_t placer_add_component(placer_context_t *ctx, const component_desc_t *desc)
{
    if (ctx == nullptr || desc == nullptr || ctx->ingest.finalized) {
        if (ctx != nullptr) {
            ctx->errors += 1u;
        }
        return PLACE_ID_NONE;
    }
    if (ctx->comps.count >= ctx->ingest.counts.num_comps) {
        ctx->errors += 1u;
        return PLACE_ID_NONE;
    }

    /* The exact courtyard polygon is pushed first: a capacity failure here must
     * not consume a component slot. */
    place_id_t polygon_id = PLACE_ID_NONE;
    if (desc->polygon_xy != nullptr && desc->polygon_n >= 3u) {
        polygon_id = placer_add_polygon(ctx, POLY_KIND_COURTYARD, desc->polygon_xy,
                                        desc->polygon_n, 0u);
        if (polygon_id == PLACE_ID_NONE) {
            return PLACE_ID_NONE;
        }
    }

    const place_id_t id = ctx->comps.count;

    comp_flags_t flags = 0u;
    if (desc->has_orientation) {
        flags |= (comp_flags_t)ctx_snap_orientation(desc->orientation_deg);
    } else {
        /* Fallback: nominal footprint orientation, rotations left free. */
        ctx_mark_degraded(ctx, DEGRADED_NO_ROTATION_DATA);
    }
    if (desc->on_bottom) {
        flags |= COMP_SIDE_BOTTOM;
    }
    if (desc->locked) {
        flags |= COMP_LOCKED;
    }
    if (desc->rot_fixed) {
        flags |= COMP_ROT_FIXED;
    }
    if (polygon_id != PLACE_ID_NONE) {
        flags |= COMP_POLY_COURTYARD;
    }

    coord_t height = 0.0f;
    if (desc->has_height && desc->height > 0.0f) {
        height = desc->height;
        flags |= COMP_HAS_HEIGHT;
    } else {
        ctx_mark_degraded(ctx, DEGRADED_NO_3D_HEIGHT);
    }

    coord_t mass = 0.0f;
    if (desc->has_mass && desc->mass > 0.0f) {
        mass = desc->mass;
        flags |= COMP_HAS_MASS;
    } else {
        ctx_mark_degraded(ctx, DEGRADED_NO_MASS);
    }

    ctx->comps.x[id] = desc->x;
    ctx->comps.y[id] = desc->y;
    /* May stay 0: finalize derives the AABB from the polygon or the pad bbox. */
    ctx->comps.half_w[id] = desc->half_w;
    ctx->comps.half_h[id] = desc->half_h;
    ctx->comps.height[id] = height;
    ctx->comps.mass[id] = mass;
    ctx->comps.flags[id] = flags;
    ctx->comps.kind[id] = (desc->kind <= COMP_KIND_OTHER) ? desc->kind
                                                         : COMP_KIND_UNKNOWN;
    ctx->comps.first_pin[id] = 0u; /* prefix-computed in finalize */
    ctx->comps.pin_count[id] = 0u;
    ctx->comps.polygon_id[id] = polygon_id;
    ctx->comps.count = id + 1u;
    return id;
}

place_id_t placer_add_pin(placer_context_t *ctx, const pin_desc_t *desc)
{
    if (ctx == nullptr || desc == nullptr || ctx->ingest.finalized) {
        if (ctx != nullptr) {
            ctx->errors += 1u;
        }
        return PLACE_ID_NONE;
    }
    if (desc->comp >= ctx->comps.count) {
        ctx->errors += 1u;
        return PLACE_ID_NONE;
    }
    if (ctx->pins.count >= ctx->ingest.counts.num_pins) {
        ctx->errors += 1u;
        return PLACE_ID_NONE;
    }

    /*
     * Ingestion contract: the pins of a component are pushed contiguously, in
     * ascending component order (the order a footprint-by-footprint KiCad walk
     * produces). first_pin/pin_count then become a prefix sum instead of a
     * counting sort, and the solver keeps a branch-free pin range per part.
     */
    if (ctx->ingest.has_pins && desc->comp < ctx->ingest.last_pin_comp) {
        ctx->errors += 1u;
        return PLACE_ID_NONE;
    }
    if (ctx->comps.pin_count[desc->comp] == UINT16_MAX) {
        ctx->errors += 1u;
        return PLACE_ID_NONE;
    }

    const place_id_t id = ctx->pins.count;

    pin_flags_t flags = desc->flags;
    if (!desc->flags_valid) {
        /* Fallback: infer the electrical role from the pin name, and remember
         * that the value is a heuristic. */
        flags = (pin_flags_t)((unsigned)ctx_infer_pin_flags(desc->name) | PIN_INFERRED);
        ctx_mark_degraded(ctx, DEGRADED_NO_PIN_ELEC);
    }

    ctx->pins.offset_x[id] = desc->offset_x;
    ctx->pins.offset_y[id] = desc->offset_y;
    ctx->pins.comp_id[id] = desc->comp;
    ctx->pins.net_id[id] = PLACE_ID_NONE; /* assigned by placer_connect */
    ctx->pins.flags[id] = flags;
    if (desc->max_current > 0.0f) {
        ctx->pins.max_current[id] = desc->max_current;
    } else {
        ctx->pins.max_current[id] = 0.0f;
        ctx_mark_degraded(ctx, DEGRADED_NO_PIN_CURRENT);
    }

    ctx->pins.count = id + 1u;
    ctx->comps.pin_count[desc->comp] =
        (place_count_t)((unsigned)ctx->comps.pin_count[desc->comp] + 1u);
    ctx->ingest.last_pin_comp = desc->comp;
    ctx->ingest.has_pins = true;
    return id;
}

place_id_t placer_add_net(placer_context_t *ctx, const net_desc_t *desc,
                          const place_id_t *pin_ids, uint32_t pin_count)
{
    if (ctx == nullptr || ctx->ingest.finalized) {
        if (ctx != nullptr) {
            ctx->errors += 1u;
        }
        return PLACE_ID_NONE;
    }
    if (ctx->nets.num_nets >= ctx->ingest.counts.num_nets) {
        ctx->errors += 1u;
        return PLACE_ID_NONE;
    }

    const place_id_t id = ctx->nets.num_nets;

    coord_t weight = 1.0f;
    if (desc != nullptr && desc->weight > 0.0f) {
        weight = desc->weight;
    } else {
        /* Fallback: uniform net weight 1.0. */
        ctx_mark_degraded(ctx, DEGRADED_NO_NET_WEIGHTS);
    }

    uint8_t net_class = 0u;
    if (desc != nullptr && desc->net_class != 0xFFu) {
        net_class = desc->net_class;
    } else {
        /* Fallback: everything in the default class. */
        ctx_mark_degraded(ctx, DEGRADED_NO_NET_CLASSES);
    }

    ctx->nets.weights[id] = weight;
    ctx->nets.net_class[id] = net_class;
    ctx->nets.num_nets = id + 1u;

    if (pin_ids != nullptr) {
        for (uint32_t i = 0u; i < pin_count; ++i) {
            (void)placer_connect(ctx, pin_ids[i], id); /* errors counted inside */
        }
    }
    return id;
}

bool placer_connect(placer_context_t *ctx, place_id_t pin, place_id_t net)
{
    if (ctx == nullptr) {
        return false;
    }
    if (pin >= ctx->pins.count || net >= ctx->nets.num_nets) {
        ctx->errors += 1u;
        return false;
    }
    if (ctx->pins.net_id[pin] != PLACE_ID_NONE) {
        /* A pin belongs to at most one net: this keeps the reverse adjacency
         * trivially available as pins.net_id. */
        ctx->errors += 1u;
        return false;
    }
    ctx->pins.net_id[pin] = net;
    return true;
}

place_id_t placer_add_polygon(placer_context_t *ctx, poly_kind_t kind,
                              const coord_t *xy, uint32_t vertex_count,
                              uint32_t layer_mask)
{
    if (ctx == nullptr || xy == nullptr || vertex_count < 3u || ctx->ingest.finalized) {
        if (ctx != nullptr) {
            ctx->errors += 1u;
        }
        return PLACE_ID_NONE;
    }

    polygon_pool_t *pp = &ctx->constraints.polygons;
    const uint32_t poly_cap = ctx->ingest.counts.num_polygons + PLACE_FALLBACK_POLYGON_SLOTS;
    const uint32_t vert_cap = ctx->ingest.counts.num_vertices + PLACE_FALLBACK_VERTEX_SLOTS;
    if (pp->num_polys >= poly_cap || (pp->num_vertices + vertex_count) > vert_cap) {
        ctx->errors += 1u;
        return PLACE_ID_NONE;
    }

    const place_id_t id = pp->num_polys;
    for (uint32_t i = 0u; i < vertex_count; ++i) {
        pp->vx[pp->num_vertices + i] = xy[2u * i];
        pp->vy[pp->num_vertices + i] = xy[2u * i + 1u];
    }
    pp->num_vertices += vertex_count;
    pp->num_polys = id + 1u;
    pp->poly_kind[id] = kind;
    pp->poly_layer_mask[id] = layer_mask;
    pp->poly_offsets[id + 1u] = pp->num_vertices;
    ctx->constraints.present |= CT_PRESENT_POLYGONS;
    return id;
}

bool placer_add_decoupling(placer_context_t *ctx, place_id_t ic_comp,
                           place_id_t cap_comp, place_id_t ic_pin,
                           coord_t max_dist_mm, coord_t weight)
{
    if (ctx == nullptr || ctx->ingest.finalized) {
        if (ctx != nullptr) {
            ctx->errors += 1u;
        }
        return false;
    }
    constraint_decoupling_t *t = &ctx->constraints.decoupling;
    if (ic_comp >= ctx->comps.count || cap_comp >= ctx->comps.count ||
        (ic_pin != PLACE_ID_NONE && ic_pin >= ctx->pins.count) ||
        !(max_dist_mm > 0.0f) || !(weight > 0.0f)) {
        ctx->errors += 1u;
        return false;
    }
    /* Rows must be grouped by the primary key so finalize can build the CSR
     * index with a counting pass instead of a sort. */
    if (t->count > 0u && ic_comp < t->ic_comp[t->count - 1u]) {
        ctx->errors += 1u;
        return false;
    }
    if (t->count >= t->capacity) {
        ctx->errors += 1u;
        return false;
    }

    const uint32_t r = t->count;
    t->ic_comp[r] = ic_comp;
    t->cap_comp[r] = cap_comp;
    t->ic_pin[r] = ic_pin;
    t->max_dist_sq[r] = max_dist_mm * max_dist_mm;
    t->weight[r] = weight;
    t->count = r + 1u;
    return true;
}

bool placer_add_thermal(placer_context_t *ctx, place_id_t comp, coord_t power_w,
                        coord_t exclusion_radius_mm, coord_t r_theta_ja)
{
    if (ctx == nullptr || ctx->ingest.finalized) {
        if (ctx != nullptr) {
            ctx->errors += 1u;
        }
        return false;
    }
    constraint_thermal_t *t = &ctx->constraints.thermal;
    if (comp >= ctx->comps.count || power_w < 0.0f || !(exclusion_radius_mm > 0.0f) ||
        r_theta_ja < 0.0f) {
        ctx->errors += 1u;
        return false;
    }
    if (t->count > 0u && comp < t->comp[t->count - 1u]) {
        ctx->errors += 1u;
        return false;
    }
    if (t->count >= t->capacity) {
        ctx->errors += 1u;
        return false;
    }

    const uint32_t r = t->count;
    t->comp[r] = comp;
    t->power_w[r] = power_w;
    t->exclusion_radius[r] = exclusion_radius_mm;
    t->r_theta_ja[r] = r_theta_ja;
    t->count = r + 1u;
    return true;
}

bool placer_add_symmetry(placer_context_t *ctx, place_id_t comp_a, place_id_t comp_b,
                         sym_axis_t axis, coord_t weight)
{
    if (ctx == nullptr || ctx->ingest.finalized) {
        if (ctx != nullptr) {
            ctx->errors += 1u;
        }
        return false;
    }
    constraint_symmetry_t *t = &ctx->constraints.symmetry;
    if (comp_a >= ctx->comps.count || comp_b >= ctx->comps.count || comp_a == comp_b ||
        axis > SYM_FIXED || !(weight > 0.0f)) {
        ctx->errors += 1u;
        return false;
    }
    if (t->count > 0u && comp_a < t->comp_a[t->count - 1u]) {
        ctx->errors += 1u;
        return false;
    }
    if (t->count >= t->capacity) {
        ctx->errors += 1u;
        return false;
    }

    const uint32_t r = t->count;
    t->comp_a[r] = comp_a;
    t->comp_b[r] = comp_b;
    t->axis[r] = axis;
    t->weight[r] = weight;
    t->count = r + 1u;
    return true;
}

bool placer_add_diffpair(placer_context_t *ctx, place_id_t net_p, place_id_t net_n,
                         coord_t max_skew_mm)
{
    if (ctx == nullptr || ctx->ingest.finalized) {
        if (ctx != nullptr) {
            ctx->errors += 1u;
        }
        return false;
    }
    diffpair_table_t *t = &ctx->constraints.diffpairs;
    if (net_p >= ctx->nets.num_nets || net_n >= ctx->nets.num_nets || net_p == net_n ||
        max_skew_mm < 0.0f) {
        ctx->errors += 1u;
        return false;
    }
    if (t->count >= t->capacity) {
        ctx->errors += 1u;
        return false;
    }

    const uint32_t r = t->count;
    t->net_p[r] = net_p;
    t->net_n[r] = net_n;
    t->max_skew_mm[r] = max_skew_mm;
    t->count = r + 1u;
    return true;
}

/* ========================================================================= */
/* Pass 3 - derived data, fallbacks, consistency                             */
/* ========================================================================= */

/*
 * first_pin becomes the prefix sum of pin_count, and every pin must sit in the
 * range of the component it declares. Zero-pin components (logos, mounting
 * holes) are legal: their range is empty.
 */
static bool finalize_pin_ranges(placer_context_t *ctx)
{
    uint32_t running = 0u;
    for (uint32_t c = 0u; c < ctx->comps.count; ++c) {
        ctx->comps.first_pin[c] = running;
        running += (uint32_t)ctx->comps.pin_count[c];
    }
    if (running != ctx->pins.count) {
        return false;
    }

    for (uint32_t c = 0u; c < ctx->comps.count; ++c) {
        const uint32_t begin = ctx->comps.first_pin[c];
        const uint32_t end = begin + (uint32_t)ctx->comps.pin_count[c];
        for (uint32_t p = begin; p < end; ++p) {
            if (ctx->pins.comp_id[p] != c) {
                return false;
            }
        }
    }
    return true;
}

/*
 * Net CSR by counting sort over pins.net_id: order-independent ingestion, one
 * O(P + N) pass, no permanent extra memory (the cursor lives in scratch).
 */
static bool build_net_csr(placer_context_t *ctx)
{
    netlist_csr_t *ns = &ctx->nets;
    if (ns->num_nets == 0u) {
        ns->num_entries = 0u;
        return true;
    }

    /* net_offsets was zeroed at allocation; fill slot n+1 with the pin count
     * of net n, then prefix-sum. */
    for (uint32_t p = 0u; p < ctx->pins.count; ++p) {
        const place_id_t net = ctx->pins.net_id[p];
        if (net != PLACE_ID_NONE) {
            if (net >= ns->num_nets) {
                return false;
            }
            ns->net_offsets[net + 1u] += 1u;
        }
    }
    for (uint32_t n = 0u; n < ns->num_nets; ++n) {
        ns->net_offsets[n + 1u] += ns->net_offsets[n];
    }
    const uint32_t entries = ns->net_offsets[ns->num_nets];
    if (entries > ctx->ingest.counts.num_net_entries) {
        return false;
    }
    ns->num_entries = entries;

    arena_mark_t mark = arena_mark(&ctx->scratch);
    uint32_t *cursor = ARENA_ARRAY(&ctx->scratch, uint32_t, (size_t)ns->num_nets);
    if (cursor == nullptr) {
        arena_rewind(&ctx->scratch, mark);
        return false;
    }
    for (uint32_t n = 0u; n < ns->num_nets; ++n) {
        cursor[n] = ns->net_offsets[n];
    }
    for (uint32_t p = 0u; p < ctx->pins.count; ++p) {
        const place_id_t net = ctx->pins.net_id[p];
        if (net != PLACE_ID_NONE) {
            ns->net_to_pins[cursor[net]] = p;
            cursor[net] += 1u;
        }
    }
    arena_rewind(&ctx->scratch, mark);
    return true;
}

/* AABB of one polygon, in its own coordinate frame. */
static bool finalize_board_outline(placer_context_t *ctx)
{
    polygon_pool_t *pool = &ctx->constraints.polygons;
    if (constraints_find_polygon(pool, POLY_KIND_BOARD_OUTLINE) != PLACE_ID_NONE) {
        return true;
    }
    if (ctx->comps.count == 0u) {
        return false;
    }

    coord_t min_x = ctx->comps.x[0] - ctx->comps.half_w[0];
    coord_t max_x = ctx->comps.x[0] + ctx->comps.half_w[0];
    coord_t min_y = ctx->comps.y[0] - ctx->comps.half_h[0];
    coord_t max_y = ctx->comps.y[0] + ctx->comps.half_h[0];
    for (uint32_t c = 1u; c < ctx->comps.count; ++c) {
        const coord_t lo_x = ctx->comps.x[c] - ctx->comps.half_w[c];
        const coord_t hi_x = ctx->comps.x[c] + ctx->comps.half_w[c];
        const coord_t lo_y = ctx->comps.y[c] - ctx->comps.half_h[c];
        const coord_t hi_y = ctx->comps.y[c] + ctx->comps.half_h[c];
        if (lo_x < min_x) {
            min_x = lo_x;
        }
        if (hi_x > max_x) {
            max_x = hi_x;
        }
        if (lo_y < min_y) {
            min_y = lo_y;
        }
        if (hi_y > max_y) {
            max_y = hi_y;
        }
    }

    const coord_t rect[10] = {
        min_x, min_y,
        max_x, min_y,
        max_x, max_y,
        min_x, max_y,
        min_x, min_y,
    };
    if (placer_add_polygon(ctx, POLY_KIND_BOARD_OUTLINE, rect, 5u, 0u) == PLACE_ID_NONE) {
        return false;
    }
    ctx_mark_degraded(ctx, DEGRADED_BOARD_OUTLINE_BBOX);
    return true;
}

/*
 * Builds the per-component CSR index of a sparse table: first[key] ..
 * first[key+1] delimits the rows whose primary key is `key`. Counting pass over
 * already-grouped rows, O(rows + num_comps), one array, no sorting.
 */
static bool build_index_by_key(placer_context_t *ctx, const place_id_t *keys,
                               uint32_t rows, uint32_t **first_out)
{
    const uint32_t ncomp = ctx->comps.count;
    uint32_t *first = ARENA_ARRAY0(&ctx->scene, uint32_t, (size_t)ncomp + 1u);
    if (first == nullptr) {
        return false;
    }
    for (uint32_t r = 0u; r < rows; ++r) {
        const place_id_t k = keys[r];
        if (k >= ncomp) {
            return false;
        }
        first[k + 1u] += 1u;
    }
    for (uint32_t c = 0u; c < ncomp; ++c) {
        first[c + 1u] += first[c];
    }
    *first_out = first;
    return true;
}

/*
 * Optional tables degrade instead of failing: if their index cannot be
 * allocated the rows are dropped (count = 0) and the feature is marked
 * degraded, so the solver simply stops applying it.
 */
static void finalize_constraint_indices(placer_context_t *ctx)
{
    constraints_table_t *ct = &ctx->constraints;
    const uint32_t disabled = ctx->ingest.opt.disable_mask;

    if (ct->decoupling.count > 0u) {
        if (!build_index_by_key(ctx, ct->decoupling.ic_comp, ct->decoupling.count,
                                &ct->decoupling.first_by_comp)) {
            ct->decoupling.count = 0u;
        }
    }
    if (ct->thermal.count > 0u) {
        if (!build_index_by_key(ctx, ct->thermal.comp, ct->thermal.count,
                                &ct->thermal.first_by_comp)) {
            ct->thermal.count = 0u;
        }
    }
    if (ct->symmetry.count > 0u) {
        if (!build_index_by_key(ctx, ct->symmetry.comp_a, ct->symmetry.count,
                                &ct->symmetry.first_by_comp)) {
            ct->symmetry.count = 0u;
        }
    }

    /* Presence bits describe what the solver can actually use. */
    ct->present = 0u;
    if (ct->decoupling.count > 0u) {
        ct->present |= CT_PRESENT_DECOUPLING;
    }
    if (ct->thermal.count > 0u) {
        ct->present |= CT_PRESENT_THERMAL;
    }
    if (ct->symmetry.count > 0u) {
        ct->present |= CT_PRESENT_SYMMETRY;
    }
    if (ct->diffpairs.count > 0u) {
        ct->present |= CT_PRESENT_DIFFPAIRS;
    }
    if (ct->polygons.num_polys > 0u) {
        ct->present |= CT_PRESENT_POLYGONS;
    }

    /* Raise one bit per optional feature that is absent or explicitly
     * disabled, so the solver can pick its fallback strategy with one mask
     * test. */
    if (ct->decoupling.count == 0u || (disabled & OPT_DISABLE_DECOUPLING) != 0u) {
        ctx_mark_degraded(ctx, DEGRADED_NO_DECOUPLING);
    }
    if (ct->thermal.count == 0u || (disabled & OPT_DISABLE_THERMAL) != 0u) {
        ctx_mark_degraded(ctx, DEGRADED_NO_THERMAL);
    }
    if (ct->symmetry.count == 0u || (disabled & OPT_DISABLE_SYMMETRY) != 0u) {
        ctx_mark_degraded(ctx, DEGRADED_NO_SYMMETRY);
    }
    if (ct->diffpairs.count == 0u || (disabled & OPT_DISABLE_DIFFPAIRS) != 0u) {
        ctx_mark_degraded(ctx, DEGRADED_NO_DIFFPAIRS);
    }
    if (constraints_find_polygon(&ct->polygons, POLY_KIND_KEEPOUT) == PLACE_ID_NONE ||
        (disabled & OPT_DISABLE_KEEPOUTS) != 0u) {
        ctx_mark_degraded(ctx, DEGRADED_NO_KEEPOUTS);
    }
}

/* Board-level fallbacks (blocks 4/5/7/8/9). */
static void finalize_board_defaults(placer_context_t *ctx)
{
    if (!ctx->stackup.has_stackup || ctx->stackup.copper_layers == 0u) {
        ctx_mark_degraded(ctx, DEGRADED_NO_STACKUP);
    }
    if (ctx->rules.class_clearance == nullptr || ctx->rules.num_classes == 0u) {
        ctx_mark_degraded(ctx, DEGRADED_NO_CLEARANCE_MATRIX);
    }
    if (!(ctx->stackup.ceiling_height > 0.0f)) {
        ctx_mark_degraded(ctx, DEGRADED_NO_CEILING);
    }
    if (ctx->stackup.airflow_x == 0.0f && ctx->stackup.airflow_y == 0.0f) {
        ctx_mark_degraded(ctx, DEGRADED_NO_AIRFLOW);
    }
    if (ctx->rules.allow_vias_under_body) {
        ctx_mark_degraded(ctx, DEGRADED_NO_VIA_RESTRICTION);
    }
    if (!(ctx->rules.min_track_width > 0.0f)) {
        ctx->rules.min_track_width = 0.2f;
    }
    /* Phase 1 has no data model for length bounds or analog/power
     * segregation: report them as unavailable rather than silently ignoring
     * the corresponding matrix rows. */
    ctx_mark_degraded(ctx, DEGRADED_NO_MAX_LENGTH);
    ctx_mark_degraded(ctx, DEGRADED_NO_SEGREGATION);
}

bool placer_context_finalize(placer_context_t *ctx)
{
    if (ctx == nullptr) {
        return false;
    }
    if (ctx->ingest.finalized) {
        return ctx->errors == 0u;
    }

    /* Mandatory blocks: components, pins, non-empty connectivity. */
    if (ctx->comps.count == 0u) {
        ctx->errors += 1u;
    }
    if (ctx->pins.count == 0u) {
        ctx->errors += 1u;
    }
    if (ctx->ingest.counts.num_nets == 0u) {
        ctx->errors += 1u;
    }
    if (ctx->comps.count > 0u && !finalize_pin_ranges(ctx)) {
        ctx->errors += 1u;
    }
    if (!build_net_csr(ctx)) {
        ctx->errors += 1u;
    }
    if (ctx->nets.num_entries == 0u) {
        /* Documented contract: an empty connectivity graph is a failure. */
        ctx->errors += 1u;
    }

    if (ctx->comps.count > 0u) {
        ctx_finalize_courtyards(ctx);
        if (!finalize_board_outline(ctx)) {
            ctx->errors += 1u;
        }
    }

    finalize_constraint_indices(ctx);
    finalize_board_defaults(ctx);

    /* The solver starts with an empty scratch arena. */
    arena_reset(&ctx->scratch);
    ctx->ingest.finalized = true;
    return ctx->errors == 0u;
}

/* ========================================================================= */
/* Validation                                                                */
/* ========================================================================= */

