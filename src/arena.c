#include "place/arena.h"

#include <stdlib.h>
#include <string.h>

static bool is_power_of_two(size_t v)
{
    return v != 0u && (v & (v - 1u)) == 0u;
}

bool arena_init(arena_t *a, size_t capacity)
{
    if (a == nullptr) {
        return false;
    }

    /* Zero first: the failure contract is "left initialised and safe to
     * destroy", which must also hold for an invalid capacity. */
    a->base = nullptr;
    a->capacity = 0u;
    a->used = 0u;
    a->raw = nullptr;

    if (capacity == 0u) {
        return false;
    }

    /* Over-allocate by one cache line and align the base manually: portable
     * across POSIX/Windows and immune to aligned_alloc size-multiple rules. */
    if (capacity > SIZE_MAX - PLACE_CACHELINE) {
        return false;
    }

    void *raw = malloc(capacity + PLACE_CACHELINE);
    if (raw == nullptr) {
        return false;
    }

    const uintptr_t addr = (uintptr_t)raw;
    const uintptr_t aligned = (addr + (PLACE_CACHELINE - 1u)) & ~(uintptr_t)(PLACE_CACHELINE - 1u);

    a->raw = raw;
    a->base = (uint8_t *)aligned;
    a->capacity = capacity;
    a->used = 0u;
    return true;
}

void arena_destroy(arena_t *a)
{
    if (a == nullptr) {
        return;
    }
    free(a->raw);
    a->base = nullptr;
    a->raw = nullptr;
    a->capacity = 0u;
    a->used = 0u;
}

void arena_reset(arena_t *a)
{
    if (a == nullptr) {
        return;
    }
    a->used = 0u;
}

void *arena_alloc(arena_t *a, size_t size, size_t align)
{
    if (a == nullptr || a->base == nullptr) {
        return nullptr;
    }
    if (align == 0u) {
        align = PLACE_CACHELINE;
    }
    if (!is_power_of_two(align)) {
        return nullptr;
    }

    /* A zero-sized request still consumes one byte so that it yields a unique,
     * dereferenceable pointer instead of a one-past-the-end address. */
    if (size == 0u) {
        size = 1u;
    }

    const size_t misalign = a->used & (align - 1u);
    const size_t pad = (misalign == 0u) ? 0u : align - misalign;

    /* Overflow-free availability check: compare against the remaining bytes
     * rather than summing, so huge `size` values cannot wrap around. */
    if (pad > a->capacity - a->used) {
        return nullptr;
    }
    const size_t available = a->capacity - a->used - pad;
    if (size > available) {
        return nullptr;
    }

    uint8_t *p = a->base + a->used + pad;
    a->used += pad + size;
    return p;
}

void *arena_alloc0(arena_t *a, size_t size, size_t align)
{
    void *p = arena_alloc(a, size, align);
    if (p != nullptr && size != 0u) {
        memset(p, 0, size);
    }
    return p;
}

arena_mark_t arena_mark(const arena_t *a)
{
    arena_mark_t mark = {0u};
    if (a != nullptr) {
        mark.used = a->used;
    }
    return mark;
}

void arena_rewind(arena_t *a, arena_mark_t mark)
{
    if (a == nullptr || a->base == nullptr) {
        return;
    }
    if (mark.used <= a->used) {
        a->used = mark.used;
    }
}

size_t arena_remaining(const arena_t *a)
{
    if (a == nullptr || a->base == nullptr) {
        return 0u;
    }
    return a->capacity - a->used;
}
