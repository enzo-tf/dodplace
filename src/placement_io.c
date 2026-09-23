/*
 * placement_io.c - the result file: a 32-byte header, then one 12-byte record
 * per component in component-id order.
 */
#include "place/io.h"
#include "io_internal.h"

#include <stdio.h>
#include <string.h>

bool placement_save(const char *path, const placement_entry_t *entries, uint32_t count)
{
    if (path == nullptr || (entries == nullptr && count > 0u)) {
        return false;
    }

    placement_header_t header;
    memset(&header, 0, sizeof header);
    memcpy(header.magic, PLACE_RESULT_MAGIC, 4u);
    header.version_major = PLACE_SCENE_VERSION_MAJOR;
    header.version_minor = PLACE_SCENE_VERSION_MINOR;
    header.flags = SCENE_FLAG_LITTLE_ENDIAN;
    header.count = count;
    header.entry_size = (uint32_t)sizeof(placement_entry_t);

    FILE *f = fopen(path, "wb");
    if (f == nullptr) {
        return false;
    }
    bool ok = io_write_exact(f, &header, sizeof header) &&
              io_write_exact(f, entries, (size_t)count * sizeof(placement_entry_t));
    if (fclose(f) != 0) {
        ok = false;
    }
    return ok;
}

bool placement_load(const char *path, placement_entry_t *entries, uint32_t capacity,
                    uint32_t *out_count)
{
    if (path == nullptr || out_count == nullptr) {
        return false;
    }
    *out_count = 0u;

    FILE *f = fopen(path, "rb");
    if (f == nullptr) {
        return false;
    }
    placement_header_t header;
    bool ok = io_read_exact(f, &header, sizeof header) &&
              memcmp(header.magic, PLACE_RESULT_MAGIC, 4u) == 0 &&
              header.version_major == PLACE_SCENE_VERSION_MAJOR &&
              header.entry_size == (uint32_t)sizeof(placement_entry_t);
    if (!ok) {
        (void)fclose(f);
        return false;
    }
    *out_count = header.count;
    if (entries == nullptr) {
        ok = true; /* count probe only */
    } else if (capacity < header.count) {
        ok = false; /* buffer too small; the caller can grow and retry */
    } else {
        ok = io_read_exact(f, entries, (size_t)header.count * sizeof(placement_entry_t));
    }
    (void)fclose(f);
    return ok;
}
