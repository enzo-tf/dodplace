/*
 * io_table.c - the section table.
 *
 * The header names every section with its offset, element size and count; the
 * loader rebuilds the table it expects from the counts and walks the file in
 * that order. This module owns the lookup and the structural checks that make
 * a malformed table an error rather than a walk off the end of the file.
 */
#include "place/io.h"
#include "io_internal.h"

#include <stdio.h>
#include <string.h>

const scene_section_t *io_find_section(const scene_section_t *table, uint32_t nsec,
                                           uint32_t kind)
{
    for (uint32_t i = 0u; i < nsec; ++i) {
        if (table[i].kind == kind) {
            return &table[i];
        }
    }
    return nullptr;
}
