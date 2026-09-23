/*
 * arena.h - bump-pointer arena allocator for the dodplace engine.
 *
 * Design contract
 * ---------------
 *  - The whole placement scene lives in at most two contiguous `malloc` blocks:
 *      * `scene`   : persistent, filled during ingestion, read by the solver.
 *      * `scratch` : transient, reset between solver passes/iterations.
 *  - No allocation happens inside the solver loop: every array is pre-sized
 *    during ingestion and handed out once.
 *  - Zero fragmentation by construction (bump pointer, never freed
 *    individually). `arena_reset` rewinds in O(1) without touching memory.
 *
 * Alignment
 * ---------
 * Every allocation returned by `arena_alloc` is aligned to the requested
 * power-of-two boundary. The arena base itself is aligned to PLACE_CACHELINE
 * (64 B) by over-allocating and adjusting the pointer manually, which is
 * portable everywhere (no posix_memalign / aligned_alloc / _aligned_malloc
 * differences) and keeps the hot SoA arrays on cache-line boundaries so a
 * vector load never straddles two lines.
 */
#ifndef PLACE_ARENA_H
#define PLACE_ARENA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Cache-line / SIMD alignment for arena allocations. 64 B is the AVX-512
 * cache line and a safe sub-multiple of the 128 B Apple Silicon line. */
#define PLACE_CACHELINE 64u

/* Restore point returned by arena_mark, consumed by arena_rewind. */
typedef struct {
    size_t used;
} arena_mark_t;

typedef struct {
    uint8_t *base;     /* aligned to PLACE_CACHELINE; null when uninitialised */
    size_t   capacity; /* usable bytes starting at base */
    size_t   used;     /* bump offset; allocations never move backwards */
    void    *raw;      /* original malloc pointer, released by arena_destroy */
} arena_t;

/*
 * Initialise `a` with `capacity` usable bytes. Returns false on invalid
 * arguments or allocation failure. On failure `a` is left zeroed.
 */
[[nodiscard]] bool arena_init(arena_t *a, size_t capacity);

/* Release the backing block. Safe on zeroed or already-destroyed arenas. */
void arena_destroy(arena_t *a);

/*
 * Rewind to offset 0, keeping the backing block. The solver uses this between
 * passes: allocations are never freed individually, the whole scratch arena is
 * recycled at once. Contents are deliberately NOT zeroed (speed); use
 * arena_alloc0 when zero-initialised memory is required.
 */
void arena_reset(arena_t *a);

/*
 * Allocate `size` bytes aligned to `align` (power of two, >= 1).
 * `align == 0` means PLACE_CACHELINE. Returns nullptr when `align` is not a
 * power of two or the arena is exhausted - the engine degrades instead of
 * aborting, so callers must check. A zero-sized request reserves one byte and
 * still returns a unique, dereferenceable pointer.
 */
[[nodiscard]] void *arena_alloc(arena_t *a, size_t size, size_t align);

/* Same as arena_alloc but zero-fills the returned block. */
[[nodiscard]] void *arena_alloc0(arena_t *a, size_t size, size_t align);

[[nodiscard]] arena_mark_t arena_mark(const arena_t *a);

/*
 * Restore the bump offset recorded by arena_mark. Marks are LIFO by design;
 * a mark from the future (used > current used) is ignored.
 */
void arena_rewind(arena_t *a, arena_mark_t mark);

[[nodiscard]] size_t arena_remaining(const arena_t *a);

#define ARENA_NEW(a, T)       ((T *)arena_alloc0((a), sizeof(T), alignof(T)))
#define ARENA_ARRAY(a, T, n)  ((T *)arena_alloc((a), sizeof(T) * (size_t)(n), alignof(T)))
#define ARENA_ARRAY0(a, T, n) ((T *)arena_alloc0((a), sizeof(T) * (size_t)(n), alignof(T)))

#endif /* PLACE_ARENA_H */
