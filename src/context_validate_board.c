/*
 * context_validate_board.c - the board itself: polygons and outline, the clearance grid,
rules and stackup.
 */
#include "place/context.h"
#include "context_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void ctx_validate_board(const placer_context_t *ctx, validation_report_t *report)
{
    /* --- polygons and board outline --------------------------------------- */
    const constraints_table_t *ct = &ctx->constraints;
    const polygon_pool_t *pp = &ct->polygons;
    if (pp->num_polys == 0u) {
        ctx_report_add(report, true, "no polygon in the scene: board outline is missing");
    } else if (pp->vx == nullptr || pp->vy == nullptr || pp->poly_offsets == nullptr ||
               pp->poly_kind == nullptr || pp->poly_layer_mask == nullptr) {
        ctx_report_add(report, true, "polygon pool is partially allocated");
    } else {
        if (pp->poly_offsets[0] != 0u ||
            pp->poly_offsets[pp->num_polys] != pp->num_vertices) {
            ctx_report_add(report, true, "polygon offsets do not cover the vertex pool");
        }
        for (uint32_t p = 0u; p < pp->num_polys; ++p) {
            if (pp->poly_offsets[p + 1u] < pp->poly_offsets[p]) {
                ctx_report_add(report, true, "polygon offsets are not monotonic");
                break;
            }
            if ((pp->poly_offsets[p + 1u] - pp->poly_offsets[p]) < 3u) {
                ctx_report_add(report, true, "polygon %u has fewer than 3 vertices",
                           (unsigned)p);
            }
        }
        if (constraints_find_polygon(pp, POLY_KIND_BOARD_OUTLINE) == PLACE_ID_NONE) {
            ctx_report_add(report, true, "board outline polygon is missing");
        }
    }

    /* --- grid, rules, stackup --------------------------------------------- */
    if (!(ctx->grid.step_fine > 0.0f) || !(ctx->grid.step_coarse > 0.0f)) {
        ctx_report_add(report, true, "placement grid steps must be positive");
    }
    if (!(ctx->rules.global_clearance > 0.0f)) {
        ctx_report_add(report, true, "global clearance must be positive");
    }
    if (ctx->rules.courtyard_clearance < 0.0f) {
        ctx_report_add(report, true, "courtyard clearance cannot be negative");
    }
    if (ctx->rules.class_clearance == nullptr && ctx->rules.num_classes != 0u) {
        ctx_report_add(report, true, "net classes are declared without a clearance matrix");
    }
    if (ctx->rules.class_clearance != nullptr) {
        if (ctx->rules.num_classes == 0u) {
            ctx_report_add(report, true, "clearance matrix is present without net classes");
        } else {
            const size_t ncls = (size_t)ctx->rules.num_classes;
            for (size_t i = 0u; i < ncls * ncls; ++i) {
                if (!(ctx->rules.class_clearance[i] > 0.0f)) {
                    ctx_report_add(report, true,
                               "clearance matrix entry %zu is not positive", i);
                    break;
                }
            }
        }
    }
    if (ctx->stackup.allowed_sides == 0u) {
        ctx_report_add(report, true, "no placement side is allowed");
    }
}
