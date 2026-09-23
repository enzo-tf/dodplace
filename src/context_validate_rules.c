/*
 * context_validate_rules.c - the sparse constraint tables: decoupling, thermal,
symmetry, differential pairs.
 */
#include "place/context.h"
#include "context_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void ctx_validate_rules(const placer_context_t *ctx, validation_report_t *report)
{
    /* --- sparse constraint tables ----------------------------------------- */
    const constraints_table_t *ct = &ctx->constraints;
    /* The pin tables the constraint rows index into; a row may only name a pin
     * that exists, and an absent table means every row that does is an error. */
    const bool pins_ok = ctx->pins.offset_x != nullptr && ctx->pins.offset_y != nullptr &&
                         ctx->pins.comp_id != nullptr && ctx->pins.net_id != nullptr &&
                         ctx->pins.flags != nullptr && ctx->pins.max_current != nullptr;
    if (ct->decoupling.count > 0u) {
        const constraint_decoupling_t *t = &ct->decoupling;
        if (t->ic_comp == nullptr || t->cap_comp == nullptr || t->ic_pin == nullptr ||
            t->max_dist_sq == nullptr || t->weight == nullptr) {
            ctx_report_add(report, true, "decoupling table is partially allocated");
        } else {
            for (uint32_t r = 0u; r < t->count; ++r) {
                if (t->ic_comp[r] >= ctx->comps.count ||
                    t->cap_comp[r] >= ctx->comps.count) {
                    ctx_report_add(report, true, "decoupling row %u has an invalid component",
                               (unsigned)r);
                }
                if (r > 0u && t->ic_comp[r] < t->ic_comp[r - 1u]) {
                    ctx_report_add(report, true, "decoupling rows are not grouped by component");
                }
                if (!(t->max_dist_sq[r] > 0.0f) || !(t->weight[r] > 0.0f)) {
                    ctx_report_add(report, true, "decoupling row %u has invalid values",
                               (unsigned)r);
                }
                if (t->ic_pin[r] != PLACE_ID_NONE && pins_ok &&
                    t->ic_pin[r] >= ctx->pins.count) {
                    ctx_report_add(report, true, "decoupling row %u has an invalid pin",
                               (unsigned)r);
                }
            }
            if (t->first_by_comp == nullptr) {
                ctx_report_add(report, true, "decoupling CSR index is missing");
            } else {
                for (uint32_t c = 0u; c < ctx->comps.count; ++c) {
                    if (t->first_by_comp[c + 1u] < t->first_by_comp[c]) {
                        ctx_report_add(report, true, "decoupling CSR index is not monotonic");
                        break;
                    }
                }
                if (t->first_by_comp[0] != 0u ||
                    t->first_by_comp[ctx->comps.count] != t->count) {
                    ctx_report_add(report, true, "decoupling CSR index does not cover the rows");
                }
            }
        }
    }
    if (ct->thermal.count > 0u) {
        const constraint_thermal_t *t = &ct->thermal;
        if (t->comp == nullptr || t->power_w == nullptr ||
            t->exclusion_radius == nullptr || t->r_theta_ja == nullptr) {
            ctx_report_add(report, true, "thermal table is partially allocated");
        } else {
            for (uint32_t r = 0u; r < t->count; ++r) {
                if (t->comp[r] >= ctx->comps.count) {
                    ctx_report_add(report, true, "thermal row %u has an invalid component",
                               (unsigned)r);
                }
                if (r > 0u && t->comp[r] < t->comp[r - 1u]) {
                    ctx_report_add(report, true, "thermal rows are not grouped by component");
                }
                if (!(t->exclusion_radius[r] > 0.0f) || t->power_w[r] < 0.0f ||
                    t->r_theta_ja[r] < 0.0f) {
                    ctx_report_add(report, true, "thermal row %u has invalid values",
                               (unsigned)r);
                }
            }
            if (t->first_by_comp == nullptr) {
                ctx_report_add(report, true, "thermal CSR index is missing");
            }
        }
    }
    if (ct->symmetry.count > 0u) {
        const constraint_symmetry_t *t = &ct->symmetry;
        if (t->comp_a == nullptr || t->comp_b == nullptr || t->axis == nullptr ||
            t->weight == nullptr) {
            ctx_report_add(report, true, "symmetry table is partially allocated");
        } else {
            for (uint32_t r = 0u; r < t->count; ++r) {
                if (t->comp_a[r] >= ctx->comps.count ||
                    t->comp_b[r] >= ctx->comps.count || t->comp_a[r] == t->comp_b[r]) {
                    ctx_report_add(report, true, "symmetry row %u has invalid components",
                               (unsigned)r);
                }
                if (r > 0u && t->comp_a[r] < t->comp_a[r - 1u]) {
                    ctx_report_add(report, true, "symmetry rows are not grouped by component");
                }
                if (t->axis[r] > SYM_FIXED || !(t->weight[r] > 0.0f)) {
                    ctx_report_add(report, true, "symmetry row %u has invalid values",
                               (unsigned)r);
                }
            }
            if (t->first_by_comp == nullptr) {
                ctx_report_add(report, true, "symmetry CSR index is missing");
            }
        }
    }
    if (ct->diffpairs.count > 0u) {
        const diffpair_table_t *t = &ct->diffpairs;
        if (t->net_p == nullptr || t->net_n == nullptr || t->max_skew_mm == nullptr) {
            ctx_report_add(report, true, "diffpair table is partially allocated");
        } else {
            for (uint32_t r = 0u; r < t->count; ++r) {
                if (t->net_p[r] >= ctx->nets.num_nets ||
                    t->net_n[r] >= ctx->nets.num_nets || t->net_p[r] == t->net_n[r]) {
                    ctx_report_add(report, true, "diffpair row %u has invalid nets",
                               (unsigned)r);
                }
                if (t->max_skew_mm[r] < 0.0f) {
                    ctx_report_add(report, true, "diffpair row %u has a negative skew",
                               (unsigned)r);
                }
            }
        }
    }
}
