/*
 * context_courtyard.c - the courtyard every component must have.
 *
 * An explicit AABB is trusted as centred on the origin. Otherwise the exact
 * courtyard polygon gives one, and only a component with neither falls back to
 * the bounding box of its pads plus the configured margin. The margin is for a
 * box guessed from pads; adding it to a real courtyard invents overlaps between
 * parts the designer placed correctly - an LED array on a 2.41 mm pitch has
 * 2.20 mm courtyards, and a 0.25 mm pad on each one turns a legal array into
 * four hundred collisions.
 */
#include "place/context.h"
#include "context_internal.h"

#include <math.h>

void ctx_polygon_bbox(const polygon_pool_t *pool, place_id_t poly,
                         coord_t *min_x, coord_t *min_y,
                         coord_t *max_x, coord_t *max_y)
{
    const uint32_t begin = pool->poly_offsets[poly];
    const uint32_t end = pool->poly_offsets[poly + 1u];
    *min_x = pool->vx[begin];
    *max_x = pool->vx[begin];
    *min_y = pool->vy[begin];
    *max_y = pool->vy[begin];
    for (uint32_t v = begin + 1u; v < end; ++v) {
        if (pool->vx[v] < *min_x) {
            *min_x = pool->vx[v];
        }
        if (pool->vx[v] > *max_x) {
            *max_x = pool->vx[v];
        }
        if (pool->vy[v] < *min_y) {
            *min_y = pool->vy[v];
        }
        if (pool->vy[v] > *max_y) {
            *max_y = pool->vy[v];
        }
    }
}

/*
 * Courtyard fallback (block 1, mandatory field).
 *
 * A component whose courtyard AABB was not supplied gets one from its exact
 * polygon when available, else from the bounding box of its pads plus
 * `courtyard_fallback_margin` (0.25 mm). Derived AABBs are re-centred: the
 * component origin and its pin offsets are rebased so that (x, y) is the true
 * geometric centre of the courtyard while the pads keep their absolute
 * positions. Explicit AABBs are trusted as centred on the origin and left
 * untouched.
 */
void ctx_finalize_courtyards(placer_context_t *ctx)
{
    const coord_t margin = ctx->ingest.opt.courtyard_fallback_margin;
    const polygon_pool_t *pool = &ctx->constraints.polygons;
    bool used_pad_bbox = false;

    for (uint32_t c = 0u; c < ctx->comps.count; ++c) {
        if (ctx->comps.half_w[c] > 0.0f && ctx->comps.half_h[c] > 0.0f) {
            continue; /* explicit AABB, centred on the origin by contract */
        }

        coord_t min_x = 0.0f;
        coord_t min_y = 0.0f;
        coord_t max_x = 0.0f;
        coord_t max_y = 0.0f;
        /* The margin is for a box guessed from pads. An exact courtyard polygon
         * is the manufacturer's own keepout: inflating it invents overlaps
         * between parts the designer placed correctly - an LED array on a
         * 2.41 mm pitch has 2.20 mm courtyards, and a 0.25 mm pad on each one
         * turns a legal array into 480 collisions. */
        coord_t extent_margin = 0.0f;

        if ((ctx->comps.flags[c] & COMP_POLY_COURTYARD) != 0u) {
            ctx_polygon_bbox(pool, ctx->comps.polygon_id[c], &min_x, &min_y, &max_x, &max_y);
            /*
             * A footprint's courtyard does not always enclose its own copper: a
             * SOP-8 body courtyard can be shorter than the pads that stick out
             * of it. Measured on r10: keeping the pad centres inside this box
             * takes KiCad's shorting_items from 15 to 3 and its mask bridges
             * from 20 to 3, at a cost of 4.4 m of wirelength - a trade the
             * board's electrical integrity wins, because a placement that
             * shorts pads is not a placement. The pads' real half extents are
             * in the scene (pins.half_x/half_y) for the multi-box model that
             * would pay neither cost.
             */
            const uint32_t begin = ctx->comps.first_pin[c];
            const uint32_t end = begin + (uint32_t)ctx->comps.pin_count[c];
            for (uint32_t p = begin; p < end; ++p) {
                const coord_t px = ctx->pins.offset_x[p];
                const coord_t py = ctx->pins.offset_y[p];
                min_x = (px < min_x) ? px : min_x;
                max_x = (px > max_x) ? px : max_x;
                min_y = (py < min_y) ? py : min_y;
                max_y = (py > max_y) ? py : max_y;
            }
        } else {
            /* No courtyard polygon: the pads are all we have, plus the margin
             * the producer configured for a guessed box. */
            const uint32_t begin = ctx->comps.first_pin[c];
            const uint32_t end = begin + (uint32_t)ctx->comps.pin_count[c];
            bool first = true;
            for (uint32_t p = begin; p < end; ++p) {
                const coord_t px = ctx->pins.offset_x[p];
                const coord_t py = ctx->pins.offset_y[p];
                if (first) {
                    min_x = max_x = px;
                    min_y = max_y = py;
                    first = false;
                } else {
                    if (px < min_x) {
                        min_x = px;
                    }
                    if (px > max_x) {
                        max_x = px;
                    }
                    if (py < min_y) {
                        min_y = py;
                    }
                    if (py > max_y) {
                        max_y = py;
                    }
                }
            }
            used_pad_bbox = true;
            extent_margin = margin;
        }

        const coord_t centre_x = (min_x + max_x) * 0.5f;
        const coord_t centre_y = (min_y + max_y) * 0.5f;
        ctx->comps.half_w[c] = (max_x - min_x) * 0.5f + extent_margin;
        ctx->comps.half_h[c] = (max_y - min_y) * 0.5f + extent_margin;

        if (centre_x != 0.0f || centre_y != 0.0f) {
            /* Rebase: the pads stay put, the placement point becomes the centre. */
            ctx->comps.x[c] += centre_x;
            ctx->comps.y[c] += centre_y;
            const uint32_t begin = ctx->comps.first_pin[c];
            const uint32_t end = begin + (uint32_t)ctx->comps.pin_count[c];
            for (uint32_t p = begin; p < end; ++p) {
                ctx->pins.offset_x[p] -= centre_x;
                ctx->pins.offset_y[p] -= centre_y;
            }
        }
    }

    if (used_pad_bbox) {
        ctx_mark_degraded(ctx, DEGRADED_COURTYARD_BBOX);
    }
}

/*
 * Board outline fallback (block 4, mandatory field): the bounding box of every
 * courtyard, synthesised as a closed 5-vertex polygon.
 */
