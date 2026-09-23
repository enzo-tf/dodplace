/*
 * context_internal.h - the pieces the context modules share.
 *
 * context.c was one file of seventeen hundred lines; it is now split by
 * concern (building, finalising, validation, courtyards), and what used to be
 * file-local helpers live here.
 *
 * Invariants:
 *   - every helper here is pure or writes only to the context it is given;
 *   - `report_add` never allocates: a validation report has fixed storage and
 *     a failure to record is itself recorded as an error;
 *   - the estimate functions mirror alloc_scene_arrays exactly; when they
 *     drift, the arena sizing doubles and retries rather than failing.
 */
#ifndef PLACE_CONTEXT_INTERNAL_H
#define PLACE_CONTEXT_INTERNAL_H

#include "place/context.h"

size_t      ctx_align_up(size_t value, size_t alignment);
void        ctx_mark_degraded(placer_context_t *ctx, uint32_t bit);
pin_flags_t ctx_infer_pin_flags(const char *name);
unsigned    ctx_snap_orientation(coord_t degrees);
size_t      ctx_estimate_array(size_t element_size, size_t count);
void        ctx_report_add(validation_report_t *report, bool is_error, const char *fmt, ...);
void        ctx_polygon_bbox(const polygon_pool_t *pool, place_id_t poly, coord_t *min_x,
                             coord_t *min_y, coord_t *max_x, coord_t *max_y);

/* The validation passes, one per family (context_validate_*.c). */
void        ctx_validate_arrays(const placer_context_t *ctx, validation_report_t *report);
void        ctx_validate_rules(const placer_context_t *ctx, validation_report_t *report);
void        ctx_validate_board(const placer_context_t *ctx, validation_report_t *report);

/* Fills in the courtyard AABB of every component that did not supply one. */
void        ctx_finalize_courtyards(placer_context_t *ctx);

#endif /* PLACE_CONTEXT_INTERNAL_H */
