/*
 * context_validate.c - the structural check run before a scene is used.
 *
 * It answers one question: can the engine read this scene without walking off
 * an array? A scene that fails here is rejected whole; a partial read is worse
 * than no read, because the solver would place a board it only half sees.
 *
 * One pass per family (context_validate_arrays/rules/board.c), in the order the
 * engine reads them, plus the invariant that ties the degradation mask to the
 * warning counter.
 */
#include "place/context.h"
#include "context_internal.h"

#include <stdarg.h>
#include <stdio.h>

void validation_report_clear(validation_report_t *report)
{
    report->errors = 0u;
    report->warnings = 0u;
    report->message_count = 0u;
    for (uint32_t i = 0u; i < PLACE_VALIDATION_MAX_MESSAGES; ++i) {
        report->messages[i][0] = '\0';
    }
}

void ctx_report_add(validation_report_t *report, bool is_error, const char *fmt, ...)
{
    if (is_error) {
        report->errors += 1u;
    } else {
        report->warnings += 1u;
    }
    if (report->message_count >= PLACE_VALIDATION_MAX_MESSAGES) {
        return;
    }
    char *dst = report->messages[report->message_count];
    va_list args;
    va_start(args, fmt);
    (void)vsnprintf(dst, PLACE_VALIDATION_MESSAGE_LEN, fmt, args);
    va_end(args);
    report->message_count += 1u;
}

static uint32_t popcount32(uint32_t v)
{
    uint32_t n = 0u;
    while (v != 0u) {
        n += v & 1u;
        v >>= 1u;
    }
    return n;
}

bool placer_context_validate(const placer_context_t *ctx, validation_report_t *report)
{
    if (ctx == nullptr || report == nullptr) {
        return false;
    }
    validation_report_clear(report);

    ctx_validate_arrays(ctx, report);
    ctx_validate_rules(ctx, report);
    ctx_validate_board(ctx, report);

    /* --- degraded-mode invariant ------------------------------------------ */
    if (popcount32(ctx->degraded) != ctx->warnings) {
        ctx_report_add(report, true,
                       "degraded mask has %u bit(s) but warnings counter is %u",
                       (unsigned)popcount32(ctx->degraded), (unsigned)ctx->warnings);
    }

    return report->errors == 0u;
}
