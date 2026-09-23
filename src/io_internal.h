/*
 * io_internal.h - what the io modules share.
 *
 * io.c was one file of a thousand lines; it is now split by direction and by
 * concern (common helpers, section tables, scene save, scene load, placement).
 * Everything here used to be file-local.
 *
 * Invariants:
 *   - a reader never allocates: every destination comes from the caller's
 *     context, and a failure leaves `report` describing what was wrong;
 *   - `fail` and `set_message` keep the first message: the first fault is the
 *     one that explains the rest;
 *   - counts_to_fields and fields_to_counts are exact inverses.
 */
#ifndef PLACE_IO_INTERNAL_H
#define PLACE_IO_INTERNAL_H

#include "place/io.h"

#include <stdio.h>

#define PLACE_MAX_EXPECTED_SECTIONS 96u

/* Header flag: the file is little-endian, as every writer we have emits. */
enum : uint32_t {
    SCENE_FLAG_LITTLE_ENDIAN = 0x00000001u
};

bool io_host_is_little_endian(void);

/* One entry of the header's section table. */
typedef struct {
    char     magic[4];
    uint16_t version_major;
    uint16_t version_minor;
    uint32_t flags;         /* bit 0: little-endian marker */
    uint32_t header_size;
    uint32_t section_count;
    uint32_t reserved0;
    uint32_t counts[SCENE_FIELD_COUNT];
    uint32_t num_net_classes;
    uint32_t reserved1[13];
    uint64_t reserved_tail;
} scene_header_t;

typedef struct {
    uint32_t kind;
    uint32_t offset;
    uint32_t count;
    uint32_t elem_size;
    uint64_t reserved0;
    uint64_t reserved1;
} scene_section_t;

size_t io_align_up(size_t value, size_t alignment);
void   io_set_message(scene_io_report_t *report, const char *text);
void   io_fail(scene_io_report_t *report, const char *text);
bool   io_read_exact(FILE *f, void *dst, size_t bytes);
bool   io_write_exact(FILE *f, const void *src, size_t bytes);
bool   io_write_padding(FILE *f, size_t bytes);
void   io_counts_to_fields(const scene_counts_t *c, uint32_t out[SCENE_FIELD_COUNT]);
void   io_fields_to_counts(const uint32_t in[SCENE_FIELD_COUNT], scene_counts_t *c);
bool   io_validate_board_config(const scene_board_config_t *cfg, scene_io_report_t *report);
uint32_t io_popcount32(uint32_t v);

typedef struct {
    char     magic[4];
    uint16_t version_major;
    uint16_t version_minor;
    uint32_t flags;
    uint32_t count;
    uint32_t entry_size;
    uint32_t reserved0;
    uint32_t reserved1;
    uint32_t reserved2;
} placement_header_t;

/* One section the loader expects to find, and where it goes. */
typedef struct {
    uint32_t    kind;
    uint32_t    count;
    uint32_t    elem_size;
    void       *dest;
    const char *name;
    bool        required;
} io_expected_t;

/* One section the writer will emit. */
typedef struct {
    uint32_t    kind;
    uint32_t    count;
    uint32_t    elem_size;
    const void *src;
} io_save_t;

bool io_build_expected_sections(const placer_context_t *ctx, const scene_counts_t *c,
                                io_expected_t *out, uint32_t max, uint32_t *n_out);
bool io_build_save_sections(const placer_context_t *ctx, io_save_t *out, uint32_t max,
                            uint32_t *n_out);
void io_config_from_context(const placer_context_t *ctx, scene_board_config_t *cfg);
const scene_section_t *io_find_section(const scene_section_t *table, uint32_t nsec,
                                      uint32_t kind);

#endif /* PLACE_IO_INTERNAL_H */
