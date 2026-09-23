/*
 * solver_state.c - building the solver's working tables from a scene.
 *
 * Everything is allocated once from the scratch arena: the arena is never
 * rewound mid-solve, so a per-evaluation allocation would exhaust it after a
 * few thousand moves. A failure here sets `ok` to false and the entry point
 * unwinds.
 */
#include "solver_internal.h"
#include "solver_state.h"
#include "shape.h"
#include "swap.h"
#include "spatial_grid.h"
#include "solver_bbox.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static void build_pin_tables(solver_t *s)
{
    const pins_soa_t *pins = &s->ctx->pins;
    const components_soa_t *comps = &s->ctx->comps;
    const coord_t *dx = pins->offset_x;
    const coord_t *dy = pins->offset_y;
    for (uint32_t i = 0u; i < s->npin; ++i) {
        const coord_t px = dx[i];
        const coord_t py = dy[i];
        /* The table is indexed by the engine's orientation, and holds the offset
         * for the pose KiCad draws at that index (solver_pose_orient). The pin's
         * own arriving angle is what makes the two differ, and it is a property
         * of the component, so the table can absorb it once. */
        const uint8_t arrived = (uint8_t)comp_orientation(comps->flags[pins->comp_id[i]]);
        for (uint8_t o = 0u; o < 4u; ++o) {
            const uint8_t pose = (uint8_t)((2u * (uint32_t)arrived + 4u - (uint32_t)o) & 3u);
            coord_t rx = px;
            coord_t ry = py;
            if (pose == 1u) {
                rx = -py; /* R(90) in this engine's y-down frame */
                ry = px;
            } else if (pose == 2u) {
                rx = -px;
                ry = -py;
            } else if (pose == 3u) {
                rx = py;
                ry = -px;
            }
            s->rot_dx[o * PIN_STRIDE(s) + i] = rx;
            s->rot_dy[o * PIN_STRIDE(s) + i] = ry;
        }
    }
}

/*
 * The usable area. The outline polygon is the authority; a rectangular board
 * gives its exact bounding box, and for any other shape the box is a superset
 * that the keepout pass narrows down.
 */
/*
 * The pitch of an axis: the median distance between neighbouring distinct
 * coordinates. A board laid out in rows and columns shows its own grid this
 * way, and that grid is what "stay in your lane" has to be measured against.
 */
coord_t solver_axis_pitch(coord_t *scratch, const coord_t *values, uint32_t count)
{
    if (count < 4u) {
        return 0.0f;
    }
    for (uint32_t i = 0u; i < count; ++i) {
        scratch[i] = values[i];
    }
    for (uint32_t i = 1u; i < count; ++i) { /* insertion sort: init-time only */
        const coord_t v = scratch[i];
        uint32_t j = i;
        while (j > 0u && scratch[j - 1u] > v) {
            scratch[j] = scratch[j - 1u];
            j -= 1u;
        }
        scratch[j] = v;
    }
    uint32_t gaps = 0u;
    coord_t prev = scratch[0];
    for (uint32_t i = 1u; i < count; ++i) {
        const coord_t gap = scratch[i] - prev;
        if (gap > 1e-3f) {
            scratch[gaps++] = gap;
            prev = scratch[i];
        }
    }
    if (gaps < 2u) {
        return 0.0f;
    }
    for (uint32_t i = 1u; i < gaps; ++i) {
        const coord_t v = scratch[i];
        uint32_t j = i;
        while (j > 0u && scratch[j - 1u] > v) {
            scratch[j] = scratch[j - 1u];
            j -= 1u;
        }
        scratch[j] = v;
    }
    return scratch[gaps / 2u];
}

void solver_load_input_pose(solver_t *s)
{
    const components_soa_t *comps = &s->ctx->comps;
    for (uint32_t i = 0u; i < s->ncomp; ++i) {
        s->x[i] = comps->x[i];
        s->y[i] = comps->y[i];
        s->orient[i] = (uint8_t)comp_orientation(comps->flags[i]);
    }
}

bool solver_state_init(solver_t *s, const placer_context_t *ctx,
                              const solver_options_t *opt)
{
    s->ctx = ctx;
    s->opt = opt;
    s->scratch = (arena_t *)&((placer_context_t *)ctx)->scratch;
    s->ncomp = ctx->comps.count;
    s->npin = ctx->pins.count;
    s->rng = (opt->seed == 0u) ? 1u : opt->seed;
    s->profile = getenv("DODPLACE_SOLVER_PROFILE") != nullptr;
    s->ok = true;

    s->x = SOLVER_ALLOC(s, coord_t, s->ncomp);
    s->y = SOLVER_ALLOC(s, coord_t, s->ncomp);
    s->orient = SOLVER_ALLOC(s, uint8_t, s->ncomp);
    s->side = SOLVER_ALLOC(s, uint8_t, s->ncomp);
    s->half_w = SOLVER_ALLOC(s, coord_t, s->ncomp);
    s->half_h = SOLVER_ALLOC(s, coord_t, s->ncomp);

    s->rot_dx = SOLVER_ALLOC(s, coord_t, (size_t)s->npin * 4u);
    s->rot_dy = SOLVER_ALLOC(s, coord_t, (size_t)s->npin * 4u);

    s->cluster_of = SOLVER_ALLOC(s, uint32_t, s->ncomp);
    s->member_comp = SOLVER_ALLOC(s, uint32_t, s->ncomp);
    s->member_dorient = SOLVER_ALLOC(s, uint8_t, s->ncomp);
    s->ruin_mask = SOLVER_ALLOC(s, uint8_t, s->ncomp);
    s->member_dx = SOLVER_ALLOC(s, coord_t, s->ncomp);
    s->member_dy = SOLVER_ALLOC(s, coord_t, s->ncomp);
    s->cluster_first = SOLVER_ALLOC(s, uint32_t, s->ncomp);
    s->cluster_count = SOLVER_ALLOC(s, uint32_t, s->ncomp);
    s->cluster_master = SOLVER_ALLOC(s, uint32_t, s->ncomp);
    s->movable = SOLVER_ALLOC(s, uint32_t, s->ncomp);

    s->seg_count_start = SOLVER_ALLOC(s, uint32_t, ctx->nets.num_nets + 1u);
    s->net_movable = SOLVER_ALLOC(s, uint8_t, ctx->nets.num_nets == 0u ? 1u : ctx->nets.num_nets);
    s->seg_x1 = SOLVER_ALLOC(s, coord_t, s->npin == 0u ? 1u : s->npin);
    s->seg_y1 = SOLVER_ALLOC(s, coord_t, s->npin == 0u ? 1u : s->npin);
    s->seg_x2 = SOLVER_ALLOC(s, coord_t, s->npin == 0u ? 1u : s->npin);
    s->seg_y2 = SOLVER_ALLOC(s, coord_t, s->npin == 0u ? 1u : s->npin);
    s->seg_net = SOLVER_ALLOC(s, uint32_t, s->npin == 0u ? 1u : s->npin);

    s->best_x = SOLVER_ALLOC(s, coord_t, s->ncomp);
    s->best_y = SOLVER_ALLOC(s, coord_t, s->ncomp);
    s->best_orient = SOLVER_ALLOC(s, uint8_t, s->ncomp);

    if (!s->ok) {
        return false;
    }

    for (uint32_t i = 0u; i < s->ncomp; ++i) {
        s->side[i] = comp_on_bottom(ctx->comps.flags[i]) ? 1u : 0u;
        s->cluster_of[i] = SOLVER_NO_INDEX;
    }
    solver_load_input_pose(s);
    /*
     * The anchor: where the designer put each part, and the box it may not
     * leave. The box is anisotropic because the two axes mean different things
     * - sliding along a rail is ordinary placement, hopping to the next rail
     * is a different circuit. The limits come from the board itself: the pitch
     * of its rows and columns, measured on the positions it arrived with.
     */
    s->anchor_x = SOLVER_ALLOC(s, coord_t, s->ncomp);
    s->anchor_y = SOLVER_ALLOC(s, coord_t, s->ncomp);
    if (s->anchor_x != nullptr && s->anchor_y != nullptr) {
        for (uint32_t i = 0u; i < s->ncomp; ++i) {
            s->anchor_x[i] = s->x[i];
            s->anchor_y[i] = s->y[i];
        }
    }
    /* Which parts arrive in conflict: a strict lane would forbid the very
     * moves that can fix them. Detected once, at init, with the same box test
     * the rest of the engine uses. */
    if (s->anchor_x != nullptr) {
        s->anchor_soft = SOLVER_ALLOC(s, uint8_t, s->ncomp);
        if (s->anchor_soft != nullptr) {
            for (uint32_t i = 0u; i < s->ncomp; ++i) {
                if (!solver_component_is_movable(s, i)) {
                    continue;
                }
                for (uint32_t j = 0u; j < s->ncomp; ++j) {
                    if (i == j) {
                        continue;
                    }
                    if (fabsf(s->x[i] - s->x[j]) < (s->half_w[i] + s->half_w[j]) &&
                        fabsf(s->y[i] - s->y[j]) < (s->half_h[i] + s->half_h[j])) {
                        s->anchor_soft[i] = 1u;
                        break;
                    }
                }
            }
        }
    }
    s->slip_x = opt->max_slip_x;
    s->slip_y = opt->max_slip_y;
    if (s->slip_x <= 0.0f || s->slip_y <= 0.0f) {
        coord_t *scratch = SOLVER_ALLOC(s, coord_t, (s->ncomp > 0u) ? s->ncomp : 1u);
        if (scratch != nullptr) {
            const coord_t pitch_x = solver_axis_pitch(scratch, s->x, s->ncomp);
            const coord_t pitch_y = solver_axis_pitch(scratch, s->y, s->ncomp);
            /* A wall only when the caller asks for one: on a board whose
             * incoming layout already has collisions - r10 has 74 - the lane
             * forbids the very moves that resolve them, and the result is
             * legal for nobody (KiCad's DRC went from 0 courtyard overlaps to
             * 3, and from 3 shorts to 11). Without the flag the anchor is a
             * cost, and the legaliser is free to do its job. */
            if (s->slip_x <= 0.0f) {
                s->slip_x = 1e6f;
            }
            if (s->slip_y <= 0.0f) {
                s->slip_y = 1e6f;
            }
            (void)pitch_x;
            (void)pitch_y;
        }
    }

    /* A part taller than the enclosure cannot be placed anywhere, so it is
     * frozen in place: moving it would only trade one illegal spot for another
     * and it has to keep blocking the space it occupies. */
    s->frozen = SOLVER_ALLOC(s, uint8_t, (s->ncomp > 0u) ? s->ncomp : 1u);
    if (s->frozen != nullptr && ctx->stackup.ceiling_height > 0.0f) {
        for (uint32_t i = 0u; i < s->ncomp; ++i) {
            if (ctx->comps.height[i] > ctx->stackup.ceiling_height) {
                s->frozen[i] = 1u;
                s->nfrozen += 1u;
            }
        }
    }
    s->stats.too_tall = s->nfrozen;

    /* The declared heat sources, once: the cost loops over this list rather
     * than over the thermal table, which holds a row per part including the
     * hundreds of LEDs the rule is not about. */
    {
        const constraint_thermal_t *t = &ctx->constraints.thermal;
        s->hot = SOLVER_ALLOC(s, uint32_t, (s->ncomp > 0u) ? s->ncomp : 1u);
        if (s->hot != nullptr) {
            for (uint32_t k = 0u; k < t->count; ++k) {
                const uint32_t comp = t->comp[k];
                if (comp != PLACE_ID_NONE && t->power_w[k] >= opt->thermal_min_power) {
                    s->hot[s->nhot++] = comp;
                }
            }
        }
    }

    {
        const pins_soa_t *pins = &ctx->pins;
        s->spans_sides = SOLVER_ALLOC(s, uint8_t, (s->ncomp > 0u) ? s->ncomp : 1u);
        bool any = false;
        if (s->spans_sides != nullptr) {
            for (uint32_t p = 0u; p < pins->count; ++p) {
                if ((pins->flags[p] & (PIN_PTH | PIN_NPTH)) != 0u) {
                    s->spans_sides[pins->comp_id[p]] = 1u;
                    any = true;
                }
            }
        }
        bool would_expect = false;
        for (uint32_t i = 0u; i < s->ncomp; ++i) {
            const comp_kind_t k = ctx->comps.kind[i];
            if (k == COMP_KIND_CONNECTOR || k == COMP_KIND_MECHANICAL) {
                would_expect = true;
                break;
            }
        }
        s->sides_known = any || !would_expect;
    }

    build_pin_tables(s);

    /* The copper box: the courtyard and every pad's metal, unioned in the
     * part's own frame. Zero extent when the scene carries no pad sizes, in
     * which case the courtyard box is all we know. */
    {
        const components_soa_t *c = &ctx->comps;
        const pins_soa_t *pins = &ctx->pins;
        const polygon_pool_t *pool = &ctx->constraints.polygons;
        s->copper_cx = SOLVER_ALLOC(s, coord_t, s->ncomp);
        s->copper_cy = SOLVER_ALLOC(s, coord_t, s->ncomp);
        s->copper_hw = SOLVER_ALLOC(s, coord_t, s->ncomp);
        s->copper_hh = SOLVER_ALLOC(s, coord_t, s->ncomp);
        if (s->copper_cx == nullptr || s->copper_cy == nullptr ||
            s->copper_hw == nullptr || s->copper_hh == nullptr) {
            return false;
        }
        for (uint32_t i = 0u; i < s->ncomp; ++i) {
            coord_t lo_x = 0.0f;
            coord_t hi_x = 0.0f;
            coord_t lo_y = 0.0f;
            coord_t hi_y = 0.0f;
            const place_id_t poly = c->polygon_id[i];
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
            const uint32_t begin = c->first_pin[i];
            const uint32_t end = begin + (uint32_t)c->pin_count[i];
            for (uint32_t p = begin; p < end && pins->half_x != nullptr; ++p) {
                const coord_t px = pins->offset_x[p];
                const coord_t py = pins->offset_y[p];
                const coord_t hx = pins->half_x[p];
                const coord_t hy = pins->half_y[p];
                if (end > begin && p == begin) {
                    lo_x = hi_x = px;
                    lo_y = hi_y = py;
                }
                lo_x = (px - hx < lo_x) ? px - hx : lo_x;
                hi_x = (px + hx > hi_x) ? px + hx : hi_x;
                lo_y = (py - hy < lo_y) ? py - hy : lo_y;
                hi_y = (py + hy > hi_y) ? py + hy : hi_y;
            }
            s->copper_cx[i] = (lo_x + hi_x) * 0.5f;
            s->copper_cy[i] = (lo_y + hi_y) * 0.5f;
            s->copper_hw[i] = (hi_x - lo_x) * 0.5f;
            s->copper_hh[i] = (hi_y - lo_y) * 0.5f;
        }
    }

    if (!swap_buckets_build(s)) {
        return false;
    }


    /* Which nets are rails. A net is one when any of its pins is a declared
     * ground or power pin, or when it simply has too many pins to be a trace;
     * the second test catches the LED rails on r10, whose pins carry no role. */
    if (opt->exclude_plane_nets) {
        s->net_plane = SOLVER_ALLOC(s, uint8_t, (ctx->nets.num_nets > 0u) ? ctx->nets.num_nets : 1u);
        if (s->net_plane != nullptr) {
            for (uint32_t n = 0u; n < ctx->nets.num_nets; ++n) {
                const uint32_t begin = ctx->nets.net_offsets[n];
                const uint32_t end = ctx->nets.net_offsets[n + 1u];
                if (end - begin >= opt->max_net_degree) {
                    s->net_plane[n] = 1u;
                    continue;
                }
                for (uint32_t e = begin; e < end; ++e) {
                    if ((ctx->pins.flags[ctx->nets.net_to_pins[e]] & (PIN_GROUND | PIN_POWER)) != 0u) {
                        s->net_plane[n] = 1u;
                        break;
                    }
                }
            }
        }
    }
    compute_board_bbox(s);
    solver_rebuild_extents(s);
    /* shape_build is NOT called: wiring it needs the complete copper model
     * (pads inside courtyards included), see the note in shape.c. When it was
     * called before solver_rebuild_extents it corrupted the arena; it is safe
     * here, but inert until the geometry pass is finished. */

    /* Which nets the solver can actually influence. Set before clustering, so a
     * locked master does not hide the nets its satellites reach. */
    for (uint32_t n = 0u; n < ctx->nets.num_nets; ++n) {
        uint8_t movable = 0u;
        for (uint32_t e = ctx->nets.net_offsets[n]; e < ctx->nets.net_offsets[n + 1u]; ++e) {
            const uint32_t comp = ctx->pins.comp_id[ctx->nets.net_to_pins[e]];
            if ((ctx->comps.flags[comp] & COMP_LOCKED) == 0u) {
                movable = 1u;
                break;
            }
        }
        s->net_movable[n] = movable;
    }

    return true;
}

/* ========================================================================= */
/* Entry point                                                               */
/* ========================================================================= */
