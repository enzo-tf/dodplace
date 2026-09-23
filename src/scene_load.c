/*
 * scene_load.c - reading a scene.
 *
 * The header is read first, then the section table is checked against the
 * counts the header declares, then the board configuration (the arenas cannot
 * be sized without it), and only then the payloads. A file that fails any step
 * is rejected whole: the solver must never place a board it only half sees.
 */
#include "place/io.h"
#include "io_internal.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool scene_load(placer_context_t *ctx, const char *path, scene_io_report_t *report)
{
    if (report != nullptr) {
        scene_io_report_clear(report);
    }
    if (ctx == nullptr || path == nullptr) {
        return false;
    }
    if (!io_host_is_little_endian()) {
        io_fail(report, "host is not little-endian; the IR is little-endian only");
        return false;
    }

    memset(ctx, 0, sizeof *ctx);
    if (report != nullptr) {
        report->errors = 0u;
    }

    FILE *f = fopen(path, "rb");
    if (f == nullptr) {
        io_fail(report, "cannot open scene file");
        return false;
    }

    bool ok = false;
    scene_section_t table[PLACE_SCENE_MAX_SECTIONS];
    scene_board_config_t board_cfg;
    memset(&board_cfg, 0, sizeof board_cfg);
    memset(table, 0, sizeof table);

    if (fseek(f, 0L, SEEK_END) != 0) {
        io_fail(report, "cannot seek scene file");
        goto done;
    }
    const long size_l = ftell(f);
    if (size_l < 0) {
        io_fail(report, "cannot measure scene file");
        goto done;
    }
    const size_t file_size = (size_t)size_l;
    if (file_size < (size_t)PLACE_SCENE_HEADER_SIZE) {
        io_fail(report, "file is smaller than the scene header");
        goto done;
    }
    if (fseek(f, 0L, SEEK_SET) != 0) {
        io_fail(report, "cannot rewind scene file");
        goto done;
    }

    scene_header_t header;
    if (!io_read_exact(f, &header, sizeof header)) {
        io_fail(report, "truncated scene header");
        goto done;
    }
    if (memcmp(header.magic, PLACE_SCENE_MAGIC, 4u) != 0) {
        io_fail(report, "bad magic: not a dodplace scene file");
        goto done;
    }
    if (header.version_major != PLACE_SCENE_VERSION_MAJOR) {
        io_fail(report, "unsupported scene IR major version");
        goto done;
    }
    if (header.header_size != PLACE_SCENE_HEADER_SIZE) {
        io_fail(report, "unexpected header size");
        goto done;
    }
    if ((header.flags & SCENE_FLAG_LITTLE_ENDIAN) == 0u) {
        io_fail(report, "scene file does not declare little-endian layout");
        goto done;
    }
    if (header.section_count == 0u || header.section_count > PLACE_SCENE_MAX_SECTIONS) {
        io_fail(report, "invalid section count");
        goto done;
    }
    if (header.num_net_classes > UINT8_MAX) {
        io_fail(report, "too many net classes");
        goto done;
    }

    if (!io_read_exact(f, table, (size_t)header.section_count * sizeof table[0])) {
        io_fail(report, "truncated section table");
        goto done;
    }

    /* Structural validation of every entry, plus duplicate detection. */
    for (uint32_t i = 0u; i < header.section_count; ++i) {
        const scene_section_t *s = &table[i];
        if (s->kind == SCENE_SECTION_NONE) {
            io_fail(report, "section entry with NONE kind");
            goto done;
        }
        if ((s->offset % PLACE_SCENE_ALIGNMENT) != 0u) {
            io_fail(report, "section offset is not 64-byte aligned");
            goto done;
        }
        if (s->elem_size == 0u) {
            io_fail(report, "section with zero element size");
            goto done;
        }
        const size_t bytes = (size_t)s->count * (size_t)s->elem_size;
        if ((size_t)s->offset < (size_t)PLACE_SCENE_HEADER_SIZE ||
            bytes > file_size || (size_t)s->offset > file_size - bytes) {
            io_fail(report, "section payload falls outside the file");
            goto done;
        }
        for (uint32_t j = 0u; j < i; ++j) {
            if (table[j].kind == s->kind) {
                io_fail(report, "duplicate section kind");
                goto done;
            }
        }
    }

    /* Board configuration must be readable before the arenas are sized. */
    const scene_section_t *cfg_sec = io_find_section(table, header.section_count,
                                                  SCENE_SECTION_BOARD_CONFIG);
    if (cfg_sec == nullptr) {
        io_fail(report, "board configuration section is missing");
        goto done;
    }
    if (cfg_sec->count != 1u || cfg_sec->elem_size != (uint32_t)sizeof board_cfg) {
        io_fail(report, "board configuration section has the wrong layout");
        goto done;
    }
    if (fseek(f, (long)cfg_sec->offset, SEEK_SET) != 0 ||
        !io_read_exact(f, &board_cfg, sizeof board_cfg)) {
        io_fail(report, "cannot read board configuration");
        goto done;
    }
    if (!io_validate_board_config(&board_cfg, report)) {
        goto done;
    }

    scene_counts_t counts;
    io_fields_to_counts(header.counts, &counts);
    counts.num_net_classes = header.num_net_classes;

    placer_options_t opt;
    placer_options_defaults(&opt);
    opt.grid_origin_x = board_cfg.grid_origin_x;
    opt.grid_origin_y = board_cfg.grid_origin_y;
    opt.grid_fine = board_cfg.grid_fine;
    opt.grid_coarse = board_cfg.grid_coarse;
    opt.courtyard_fallback_margin = board_cfg.courtyard_fallback_margin;
    opt.global_clearance = board_cfg.global_clearance;
    opt.courtyard_clearance = board_cfg.courtyard_clearance;
    opt.allowed_sides = (side_mask_t)board_cfg.allowed_sides;
    opt.disable_mask = board_cfg.disable_mask;

    if (!placer_context_begin(ctx, &counts, &opt)) {
        io_fail(report, "cannot allocate the scene (counts too large?)");
        goto done;
    }

    /* Board-level scalars that live outside placer_options_t. */
    ctx->rules.min_track_width = board_cfg.min_track_width;
    ctx->rules.allow_vias_under_body = board_cfg.allow_vias_under_body != 0u;
    ctx->stackup.has_stackup = board_cfg.has_stackup != 0u;
    ctx->stackup.copper_layers = (uint8_t)board_cfg.copper_layers;
    ctx->stackup.total_thickness = board_cfg.total_thickness;
    ctx->stackup.ceiling_height = board_cfg.ceiling_height;
    ctx->stackup.airflow_x = board_cfg.airflow_x;
    ctx->stackup.airflow_y = board_cfg.airflow_y;
    ctx->ingest.opt.courtyard_fallback_margin = board_cfg.courtyard_fallback_margin;
    ctx->ingest.opt.disable_mask = board_cfg.disable_mask;

    /*
     * Restore the producer's degradation report before finalize, which will
     * OR in the bits it derives from the loaded data. warnings is rebuilt from
     * the mask so the `warnings == popcount(degraded)` invariant holds and
     * later mark_degraded() calls only count genuinely new bits.
     */
    ctx->degraded = board_cfg.degraded_mask;
    ctx->warnings = io_popcount32(ctx->degraded);

    if (counts.num_net_classes > 0u && ctx->rules.class_clearance == nullptr) {
        io_fail(report, "clearance matrix could not be allocated");
        goto done;
    }

    io_expected_t expected[PLACE_MAX_EXPECTED_SECTIONS];
    uint32_t nexp = 0u;
    if (!io_build_expected_sections(ctx, &counts, expected, PLACE_MAX_EXPECTED_SECTIONS, &nexp)) {
        io_fail(report, "internal: expected-section table overflow");
        goto done;
    }

    for (uint32_t i = 0u; i < nexp; ++i) {
        const scene_section_t *s = io_find_section(table, header.section_count, expected[i].kind);
        if (expected[i].count == 0u) {
            continue; /* not required by the declared counts */
        }
        if (expected[i].dest == nullptr) {
            continue; /* board config: already read and applied above */
        }
        if (s == nullptr) {
            if (!expected[i].required) {
                continue; /* optional: the array keeps its default */
            }
            if (report != nullptr) {
                (void)snprintf(report->message, sizeof report->message,
                               "required section '%s' is missing", expected[i].name);
                report->errors += 1u;
            }
            goto done;
        }
        if (s->count != expected[i].count) {
            if (report != nullptr) {
                (void)snprintf(report->message, sizeof report->message,
                               "section '%s' has %u elements, expected %u",
                               expected[i].name, (unsigned)s->count,
                               (unsigned)expected[i].count);
                report->errors += 1u;
            }
            goto done;
        }
        if (s->elem_size != expected[i].elem_size) {
            if (report != nullptr) {
                (void)snprintf(report->message, sizeof report->message,
                               "section '%s' has an unexpected element size", expected[i].name);
                report->errors += 1u;
            }
            goto done;
        }
        const size_t bytes = (size_t)s->count * (size_t)s->elem_size;
        if (fseek(f, (long)s->offset, SEEK_SET) != 0 ||
            !io_read_exact(f, expected[i].dest, bytes)) {
            if (report != nullptr) {
                (void)snprintf(report->message, sizeof report->message,
                               "cannot read section '%s'", expected[i].name);
                report->errors += 1u;
            }
            goto done;
        }
        if (report != nullptr) {
            report->sections_loaded += 1u;
        }
    }

    /* Unknown/ignorable sections are tolerated; count them for the report. */
    for (uint32_t i = 0u; i < header.section_count; ++i) {
        const uint32_t kind = table[i].kind;
        if (kind == SCENE_SECTION_JSON_TAIL || kind >= SCENE_SECTION_KIND_MAX) {
            if (report != nullptr) {
                report->sections_skipped += 1u;
            }
            continue;
        }
        bool known = false;
        for (uint32_t j = 0u; j < nexp; ++j) {
            if (expected[j].kind == kind) {
                known = true;
                break;
            }
        }
        if (!known && report != nullptr) {
            report->warnings += 1u;
            report->sections_skipped += 1u;
        }
    }

    if (!placer_context_adopt(ctx, &counts)) {
        io_fail(report, "scene counts are inconsistent with the reserved arrays");
        goto done;
    }

    validation_report_t vreport;
    if (!placer_context_finalize(ctx)) {
        (void)placer_context_validate(ctx, &vreport);
        if (report != nullptr) {
            report->errors += 1u;
            (void)snprintf(report->message, sizeof report->message, "%s",
                           vreport.message_count > 0u ? vreport.messages[0]
                                                      : "scene finalization failed");
        }
        goto done;
    }
    if (!placer_context_validate(ctx, &vreport)) {
        if (report != nullptr) {
            report->errors += 1u;
            (void)snprintf(report->message, sizeof report->message, "%s",
                           vreport.message_count > 0u ? vreport.messages[0]
                                                      : "scene validation failed");
        }
        goto done;
    }

    ok = true;

done:
    (void)fclose(f);
    if (!ok) {
        /* Never leave a half-built context behind. */
        placer_context_destroy(ctx);
    }
    return ok;
}

/* ========================================================================= */
/* placement.bin                                                             */
/* ========================================================================= */

