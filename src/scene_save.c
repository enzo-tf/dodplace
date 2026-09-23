/*
 * scene_save.c - writing a scene back to disk.
 *
 * The writer emits the section table it will write first, then the payloads,
 * then the JSON tail. The reader rebuilds the same table from the counts, so
 * the two sides agree on order, offsets and padding by construction - the
 * golden fixture in ir/ is what proves it byte for byte.
 */
#include "place/io.h"
#include "io_internal.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

/* The tail every scene carries: who wrote it, and which IR it speaks. */
static const char PLACE_STATIC_TAIL[] = "{\"generator\":\"dodplace\",\"ir\":\"1.0\"}";

void io_config_from_context(const placer_context_t *ctx, scene_board_config_t *cfg)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->grid_origin_x = ctx->grid.origin_x;
    cfg->grid_origin_y = ctx->grid.origin_y;
    cfg->grid_fine = ctx->grid.step_fine;
    cfg->grid_coarse = ctx->grid.step_coarse;
    cfg->courtyard_fallback_margin = ctx->ingest.opt.courtyard_fallback_margin;
    cfg->global_clearance = ctx->rules.global_clearance;
    cfg->courtyard_clearance = ctx->rules.courtyard_clearance;
    cfg->min_track_width = ctx->rules.min_track_width;
    cfg->total_thickness = ctx->stackup.total_thickness;
    cfg->ceiling_height = ctx->stackup.ceiling_height;
    cfg->airflow_x = ctx->stackup.airflow_x;
    cfg->airflow_y = ctx->stackup.airflow_y;
    cfg->allowed_sides = ctx->stackup.allowed_sides;
    cfg->copper_layers = ctx->stackup.copper_layers;
    cfg->has_stackup = ctx->stackup.has_stackup ? 1u : 0u;
    cfg->allow_vias_under_body = ctx->rules.allow_vias_under_body ? 1u : 0u;
    cfg->disable_mask = ctx->ingest.opt.disable_mask;
    cfg->degraded_mask = ctx->degraded;
}

/* ========================================================================= */
/* scene_save                                                                */
/* ========================================================================= */

bool scene_save(const placer_context_t *ctx, const char *path)
{
    if (ctx == nullptr || path == nullptr || !ctx->ingest.finalized) {
        return false;
    }
    if (ctx->rules.num_classes > 0u && ctx->rules.class_clearance == nullptr) {
        return false;
    }

    io_save_t sections[PLACE_MAX_EXPECTED_SECTIONS + 1u];
    uint32_t nsec = 0u;
    if (!io_build_save_sections(ctx, sections, PLACE_MAX_EXPECTED_SECTIONS, &nsec)) {
        return false;
    }

    /* Self-describing tail: also exercises the loader's ignored-section path. */
    sections[nsec].kind = SCENE_SECTION_JSON_TAIL;
    sections[nsec].count = (uint32_t)(sizeof PLACE_STATIC_TAIL - 1u);
    sections[nsec].elem_size = 1u;
    sections[nsec].src = PLACE_STATIC_TAIL;
    nsec += 1u;

    /* Append the board config as the last payload section. */
    scene_board_config_t board_cfg;
    io_config_from_context(ctx, &board_cfg);
    sections[nsec].kind = SCENE_SECTION_BOARD_CONFIG;
    sections[nsec].count = 1u;
    sections[nsec].elem_size = (uint32_t)sizeof board_cfg;
    sections[nsec].src = &board_cfg;
    nsec += 1u;

    scene_header_t header;
    memset(&header, 0, sizeof header);
    memcpy(header.magic, PLACE_SCENE_MAGIC, 4u);
    header.version_major = PLACE_SCENE_VERSION_MAJOR;
    header.version_minor = PLACE_SCENE_VERSION_MINOR;
    header.flags = SCENE_FLAG_LITTLE_ENDIAN;
    header.header_size = PLACE_SCENE_HEADER_SIZE;
    header.section_count = nsec;
    scene_counts_t counts;
    memset(&counts, 0, sizeof counts);
    counts.num_comps = ctx->comps.count;
    counts.num_pins = ctx->pins.count;
    counts.num_nets = ctx->nets.num_nets;
    counts.num_net_entries = ctx->nets.num_entries;
    counts.num_polygons = ctx->constraints.polygons.num_polys;
    counts.num_vertices = ctx->constraints.polygons.num_vertices;
    counts.num_decoupling = ctx->constraints.decoupling.count;
    counts.num_thermal = ctx->constraints.thermal.count;
    counts.num_symmetry = ctx->constraints.symmetry.count;
    counts.num_diffpairs = ctx->constraints.diffpairs.count;
    counts.num_net_classes = ctx->rules.num_classes;
    io_counts_to_fields(&counts, header.counts);
    header.num_net_classes = counts.num_net_classes;

    scene_section_t table[PLACE_MAX_EXPECTED_SECTIONS + 1u];
    size_t offset = io_align_up((size_t)PLACE_SCENE_HEADER_SIZE +
                                 (size_t)nsec * PLACE_SCENE_SECTION_ENTRY_SIZE,
                             PLACE_SCENE_ALIGNMENT);
    for (uint32_t i = 0u; i < nsec; ++i) {
        memset(&table[i], 0, sizeof table[i]);
        table[i].kind = sections[i].kind;
        table[i].count = sections[i].count;
        table[i].elem_size = sections[i].elem_size;
        const size_t bytes = (size_t)sections[i].count * (size_t)sections[i].elem_size;
        if (offset > (size_t)LONG_MAX) {
            return false;
        }
        table[i].offset = (uint32_t)offset;
        offset = io_align_up(offset + bytes, PLACE_SCENE_ALIGNMENT);
    }

    FILE *f = fopen(path, "wb");
    if (f == nullptr) {
        return false;
    }
    bool ok = io_write_exact(f, &header, sizeof header) &&
              io_write_exact(f, table, (size_t)nsec * sizeof table[0]);
    size_t cursor = (size_t)PLACE_SCENE_HEADER_SIZE + (size_t)nsec * sizeof table[0];
    for (uint32_t i = 0u; ok && i < nsec; ++i) {
        const size_t bytes = (size_t)sections[i].count * (size_t)sections[i].elem_size;
        if ((size_t)table[i].offset < cursor) {
            ok = false;
            break;
        }
        ok = io_write_padding(f, (size_t)table[i].offset - cursor) &&
             io_write_exact(f, sections[i].src, bytes);
        cursor = (size_t)table[i].offset + bytes;
    }
    if (fclose(f) != 0) {
        ok = false;
    }
    return ok;
}

/* ========================================================================= */
/* scene_load                                                                */
/* ========================================================================= */


