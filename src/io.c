#include "place/io.h"
#include "io_internal.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

/* ========================================================================= */
/* On-disk structures (little-endian by definition, validated at load)        */
/* ========================================================================= */



static_assert(sizeof(scene_header_t) == PLACE_SCENE_HEADER_SIZE,
              "scene header layout is part of the IR");
static_assert(sizeof(scene_section_t) == PLACE_SCENE_SECTION_ENTRY_SIZE,
              "section entry layout is part of the IR");
static_assert(sizeof(placement_header_t) == PLACE_RESULT_HEADER_SIZE,
              "result header layout is part of the IR");

/* Longest expected-section list; the real list is ~45 entries. */
#define PLACE_MAX_EXPECTED_SECTIONS 96u

/* ========================================================================= */
/* Helpers                                                                   */
/* ========================================================================= */

void scene_io_report_clear(scene_io_report_t *report)
{
    if (report == nullptr) {
        return;
    }
    memset(report, 0, sizeof *report);
}

uint32_t io_popcount32(uint32_t v)
{
    uint32_t n = 0u;
    while (v != 0u) {
        n += v & 1u;
        v >>= 1u;
    }
    return n;
}


bool io_host_is_little_endian(void)
{
    const uint16_t probe = 0x0001u;
    uint8_t bytes[2];
    memcpy(bytes, &probe, sizeof bytes);
    return bytes[0] == 0x01u;
}

size_t io_align_up(size_t value, size_t alignment)
{
    return (value + (alignment - 1u)) & ~(alignment - 1u);
}

void io_set_message(scene_io_report_t *report, const char *text)
{
    if (report == nullptr) {
        return;
    }
    (void)snprintf(report->message, sizeof report->message, "%s", text);
}

void io_fail(scene_io_report_t *report, const char *text)
{
    if (report != nullptr) {
        report->errors += 1u;
    }
    io_set_message(report, text);
}

bool io_read_exact(FILE *f, void *dst, size_t bytes)
{
    return bytes == 0u || fread(dst, 1u, bytes, f) == bytes;
}

bool io_write_exact(FILE *f, const void *src, size_t bytes)
{
    return bytes == 0u || fwrite(src, 1u, bytes, f) == bytes;
}

bool io_write_padding(FILE *f, size_t bytes)
{
    static const uint8_t zeros[PLACE_CACHELINE] = {0};
    while (bytes > 0u) {
        const size_t chunk = (bytes < sizeof zeros) ? bytes : sizeof zeros;
        if (!io_write_exact(f, zeros, chunk)) {
            return false;
        }
        bytes -= chunk;
    }
    return true;
}

void io_counts_to_fields(const scene_counts_t *c, uint32_t out[SCENE_FIELD_COUNT])
{
    out[SCENE_FIELD_COMPS] = c->num_comps;
    out[SCENE_FIELD_PINS] = c->num_pins;
    out[SCENE_FIELD_NETS] = c->num_nets;
    out[SCENE_FIELD_NET_ENTRIES] = c->num_net_entries;
    out[SCENE_FIELD_POLYGONS] = c->num_polygons;
    out[SCENE_FIELD_VERTICES] = c->num_vertices;
    out[SCENE_FIELD_DECOUPLING] = c->num_decoupling;
    out[SCENE_FIELD_THERMAL] = c->num_thermal;
    out[SCENE_FIELD_SYMMETRY] = c->num_symmetry;
    out[SCENE_FIELD_DIFFPAIRS] = c->num_diffpairs;
}

void io_fields_to_counts(const uint32_t in[SCENE_FIELD_COUNT], scene_counts_t *c)
{
    memset(c, 0, sizeof *c);
    c->num_comps = in[SCENE_FIELD_COMPS];
    c->num_pins = in[SCENE_FIELD_PINS];
    c->num_nets = in[SCENE_FIELD_NETS];
    c->num_net_entries = in[SCENE_FIELD_NET_ENTRIES];
    c->num_polygons = in[SCENE_FIELD_POLYGONS];
    c->num_vertices = in[SCENE_FIELD_VERTICES];
    c->num_decoupling = in[SCENE_FIELD_DECOUPLING];
    c->num_thermal = in[SCENE_FIELD_THERMAL];
    c->num_symmetry = in[SCENE_FIELD_SYMMETRY];
    c->num_diffpairs = in[SCENE_FIELD_DIFFPAIRS];
}

bool io_validate_board_config(const scene_board_config_t *cfg, scene_io_report_t *report)
{
    if (!(cfg->grid_fine > 0.0f) || !(cfg->grid_coarse > 0.0f)) {
        io_fail(report, "board config: grid steps must be positive");
        return false;
    }
    if (!(cfg->courtyard_fallback_margin > 0.0f)) {
        io_fail(report, "board config: courtyard fallback margin must be positive");
        return false;
    }
    if (!(cfg->global_clearance > 0.0f) || !(cfg->min_track_width > 0.0f) ||
        cfg->courtyard_clearance < 0.0f) {
        io_fail(report, "board config: invalid clearance values");
        return false;
    }
    if (cfg->allowed_sides == 0u || cfg->allowed_sides > SIDE_BOTH) {
        io_fail(report, "board config: invalid allowed sides");
        return false;
    }
    if (cfg->copper_layers > UINT8_MAX) {
        io_fail(report, "board config: too many copper layers");
        return false;
    }
    if (!(cfg->total_thickness >= 0.0f) || !(cfg->ceiling_height >= 0.0f) ||
        !(cfg->airflow_x >= 0.0f) || !(cfg->airflow_y >= 0.0f)) {
        io_fail(report, "board config: negative mechanical value");
        return false;
    }
    if ((cfg->degraded_mask & ~(uint32_t)DEGRADED_ALL_MASK) != 0u) {
        io_fail(report, "board config: unknown degraded-mode bit");
        return false;
    }
    return true;
}


