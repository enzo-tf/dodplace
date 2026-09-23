/*
 * context_validate_arrays.c - the per-array structural checks.
 *
 * Arenas, components, pins and the net CSR: everything the engine will index
 * into. A failure here is fatal to the whole scene - a partial read is worse
 * than no read, because the solver would place a board it only half sees.
 */
#include "place/context.h"
#include "context_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static bool comps_arrays_present(const placer_context_t *ctx)
{
    return ctx->comps.x != nullptr && ctx->comps.y != nullptr &&
           ctx->comps.half_w != nullptr && ctx->comps.half_h != nullptr &&
           ctx->comps.height != nullptr && ctx->comps.mass != nullptr &&
           ctx->comps.flags != nullptr && ctx->comps.kind != nullptr &&
           ctx->comps.first_pin != nullptr &&
           ctx->comps.pin_count != nullptr && ctx->comps.polygon_id != nullptr;
}

static bool pins_arrays_present(const placer_context_t *ctx)
{
    return ctx->pins.offset_x != nullptr && ctx->pins.offset_y != nullptr &&
           ctx->pins.comp_id != nullptr && ctx->pins.net_id != nullptr &&
           ctx->pins.flags != nullptr && ctx->pins.max_current != nullptr;
}

static bool nets_arrays_present(const placer_context_t *ctx)
{
    return ctx->nets.net_offsets != nullptr && ctx->nets.net_to_pins != nullptr &&
           ctx->nets.weights != nullptr && ctx->nets.net_class != nullptr;
}

static void validate_arenas(const placer_context_t *ctx, validation_report_t *report)
{
    if (ctx->scene.base == nullptr || ctx->scene.capacity == 0u) {
        ctx_report_add(report, true, "scene arena is not initialised");
    } else if ((uintptr_t)ctx->scene.base % PLACE_CACHELINE != 0u) {
        ctx_report_add(report, true, "scene arena base is not %u-byte aligned",
                       (unsigned)PLACE_CACHELINE);
    }
    if (ctx->scratch.base == nullptr || ctx->scratch.capacity == 0u) {
        ctx_report_add(report, true, "scratch arena is not initialised");
    }
    if (ctx->errors > 0u) {
        ctx_report_add(report, true, "context reports %u ingestion error(s)",
                       (unsigned)ctx->errors);
    }
}

static void validate_components(const placer_context_t *ctx, validation_report_t *report,
                                bool comps_ok)
{
    if (!comps_ok) {
        ctx_report_add(report, true, "component arrays are missing");
        return;
    }
    uint32_t pins_seen = 0u;
    for (uint32_t c = 0u; c < ctx->comps.count; ++c) {
        if (!(ctx->comps.half_w[c] > 0.0f) || !(ctx->comps.half_h[c] > 0.0f)) {
            ctx_report_add(report, true, "component %u has a non-positive courtyard",
                           (unsigned)c);
        }
        if (ctx->comps.polygon_id[c] != PLACE_ID_NONE &&
            ctx->comps.polygon_id[c] >= ctx->constraints.polygons.num_polys) {
            ctx_report_add(report, true, "component %u references an invalid courtyard polygon",
                           (unsigned)c);
        }
        if (ctx->comps.first_pin[c] != pins_seen) {
            ctx_report_add(report, true, "component %u: first_pin is not a prefix sum",
                           (unsigned)c);
        }
        pins_seen += (uint32_t)ctx->comps.pin_count[c];
    }
    if (pins_seen != ctx->pins.count) {
        ctx_report_add(report, true, "pin_count total (%u) does not match pins.count (%u)",
                       (unsigned)pins_seen, (unsigned)ctx->pins.count);
    }
}

static void validate_pins(const placer_context_t *ctx, validation_report_t *report, bool comps_ok,
                          bool pins_ok)
{
    if (ctx->pins.count > 0u && !pins_ok) {
        ctx_report_add(report, true, "pin arrays are missing");
        return;
    }
    if (ctx->pins.count > 0u && comps_ok && pins_ok) {
        for (uint32_t p = 0u; p < ctx->pins.count; ++p) {
            if (ctx->pins.comp_id[p] >= ctx->comps.count) {
                ctx_report_add(report, true, "pin %u is outside the range of component %u",
                               (unsigned)p, (unsigned)ctx->pins.comp_id[p]);
                break; /* one message is enough: the table is unusable either way */
            }
        }
    }
}

static void validate_nets(const placer_context_t *ctx, validation_report_t *report, bool pins_ok,
                          bool nets_ok)
{
    if (ctx->nets.num_nets == 0u) {
        ctx_report_add(report, true, "netlist is empty: connectivity is mandatory");
        return;
    }
    if (!nets_ok) {
        ctx_report_add(report, true, "net arrays are missing");
        return;
    }
    if (ctx->nets.net_offsets[0] != 0u) {
        ctx_report_add(report, true, "net CSR does not start at 0");
    }
    for (uint32_t n = 0u; n < ctx->nets.num_nets; ++n) {
        if (ctx->nets.net_offsets[n + 1u] < ctx->nets.net_offsets[n]) {
            ctx_report_add(report, true, "net CSR offsets are not monotonic");
            break;
        }
        if (!(ctx->nets.weights[n] > 0.0f)) {
            ctx_report_add(report, true, "net %u has a non-positive weight", (unsigned)n);
            break;
        }
    }
    if (ctx->nets.net_offsets[ctx->nets.num_nets] != ctx->nets.num_entries) {
        ctx_report_add(report, true, "net CSR size (%u) differs from num_entries (%u)",
                       (unsigned)ctx->nets.net_offsets[ctx->nets.num_nets],
                       (unsigned)ctx->nets.num_entries);
    }
    if (ctx->nets.num_entries > ctx->pins.count) {
        ctx_report_add(report, true, "net CSR has more entries than pins");
        return;
    }
    if (!pins_ok) {
        return;
    }
    for (uint32_t n = 0u; n < ctx->nets.num_nets; ++n) {
        for (uint32_t e = ctx->nets.net_offsets[n]; e < ctx->nets.net_offsets[n + 1u]; ++e) {
            const place_id_t pin = ctx->nets.net_to_pins[e];
            if (pin >= ctx->pins.count) {
                ctx_report_add(report, true, "net %u references pin %u out of range",
                               (unsigned)n, (unsigned)pin);
                return;
            }
            if (ctx->pins.net_id[pin] != n) {
                ctx_report_add(report, true, "net %u lists pin %u which claims another net",
                               (unsigned)n, (unsigned)pin);
                return;
            }
        }
    }
}

void ctx_validate_arrays(const placer_context_t *ctx, validation_report_t *report)
{
    validate_arenas(ctx, report);

    /* --- components ------------------------------------------------------- */
    const bool comps_ok = comps_arrays_present(ctx);
    if (ctx->comps.count == 0u) {
        ctx_report_add(report, true, "no components in the scene");
        return; /* every later check indexes the component table */
    }
    validate_components(ctx, report, comps_ok);

    /* --- pins and pin->component CSR -------------------------------------- */
    const bool pins_ok = pins_arrays_present(ctx);
    validate_pins(ctx, report, comps_ok, pins_ok);

    /* --- net CSR ---------------------------------------------------------- */
    validate_nets(ctx, report, pins_ok, nets_arrays_present(ctx));
}
