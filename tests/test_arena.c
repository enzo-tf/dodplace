/*
 * test_arena.c - arena allocator invariants.
 *
 * Zero-dependency test harness: CHECK records failures, main() returns the
 * failure count so CTest reports a non-zero exit code on regression.
 */
#include "place/arena.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdint.h>

static int g_failures;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            (void)printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
            g_failures++;                                                 \
        }                                                                 \
    } while (0)

static bool aligned_to(const void *p, size_t align)
{
    return ((uintptr_t)p % (uintptr_t)align) == 0u;
}

static void test_init_invalid(void)
{
    arena_t a;
    CHECK(!arena_init(nullptr, 1024u));
    CHECK(!arena_init(&a, 0u));
    /* Failed init must leave a zeroed, safe-to-destroy arena. */
    arena_destroy(&a);
}

static void test_base_alignment_and_oom(void)
{
    arena_t a;
    CHECK(arena_init(&a, 4096u));
    CHECK(a.base != nullptr);
    CHECK(aligned_to(a.base, PLACE_CACHELINE));
    CHECK(a.capacity == 4096u);
    CHECK(a.used == 0u);
    CHECK(arena_remaining(&a) == 4096u);

    /* Every allocation honours the requested alignment and never overlaps. */
    void *p1 = arena_alloc(&a, 1u, 1u);
    void *p2 = arena_alloc(&a, 1u, PLACE_CACHELINE);
    void *p3 = arena_alloc(&a, 1u, PLACE_CACHELINE);
    CHECK(p1 != nullptr && p2 != nullptr && p3 != nullptr);
    CHECK(aligned_to(p2, PLACE_CACHELINE));
    CHECK(aligned_to(p3, PLACE_CACHELINE));
    CHECK((uintptr_t)p3 - (uintptr_t)p2 == PLACE_CACHELINE);

    /* Arena exhaustion degrades to nullptr; state stays consistent. */
    const size_t used_before = a.used;
    CHECK(arena_alloc(&a, 1u << 20, PLACE_CACHELINE) == nullptr);
    CHECK(a.used == used_before);

    /* Overflow-safe bound check: a huge size must not wrap around. */
    CHECK(arena_alloc(&a, SIZE_MAX, PLACE_CACHELINE) == nullptr);
    CHECK(a.used == used_before);

    /* Non power-of-two alignment is rejected. */
    CHECK(arena_alloc(&a, 8u, 48u) == nullptr);
    CHECK(a.used == used_before);

    /* Zero-sized requests still yield unique dereferenceable pointers. */
    void *z0 = arena_alloc(&a, 0u, 4u);
    void *z1 = arena_alloc(&a, 0u, 4u);
    CHECK(z0 != nullptr && z1 != nullptr && z0 != z1);
    *(uint8_t *)z0 = 0xAAu;

    arena_destroy(&a);
}

static void test_alloc0(void)
{
    arena_t a;
    CHECK(arena_init(&a, 1024u));
    uint8_t *buf = ARENA_ARRAY0(&a, uint8_t, 64u);
    CHECK(buf != nullptr);
    bool all_zero = true;
    for (size_t i = 0; i < 64u; ++i) {
        all_zero = all_zero && buf[i] == 0u;
    }
    CHECK(all_zero);
    arena_destroy(&a);
}

static void test_mark_rewind_and_reset(void)
{
    arena_t a;
    CHECK(arena_init(&a, 1024u));

    arena_mark_t m0 = arena_mark(&a);
    void *first = arena_alloc(&a, 128u, PLACE_CACHELINE);
    CHECK(first != nullptr);

    arena_mark_t m1 = arena_mark(&a);
    void *second = arena_alloc(&a, 128u, PLACE_CACHELINE);
    CHECK(second != nullptr);
    CHECK(second != first);

    arena_rewind(&a, m1);
    void *second_again = arena_alloc(&a, 128u, PLACE_CACHELINE);
    CHECK(second_again == second);

    arena_rewind(&a, m0);
    void *first_again = arena_alloc(&a, 128u, PLACE_CACHELINE);
    CHECK(first_again == first);

    /* A stale (future) mark is ignored rather than corrupting the arena. */
    arena_mark_t future = {a.used + 64u};
    arena_rewind(&a, future);
    CHECK(a.used == 128u);

    arena_reset(&a);
    CHECK(a.used == 0u);
    CHECK(arena_remaining(&a) == a.capacity);
    CHECK(arena_alloc(&a, 128u, PLACE_CACHELINE) == first);

    arena_destroy(&a);
    /* Destruction is idempotent. */
    arena_destroy(&a);
    CHECK(a.base == nullptr && a.raw == nullptr);
    CHECK(arena_alloc(&a, 16u, 4u) == nullptr);
}

static void test_typed_macros(void)
{
    arena_t a;
    CHECK(arena_init(&a, 4096u));

    struct probe {
        double d;
        uint32_t w;
    };

    struct probe *p = ARENA_NEW(&a, struct probe);
    CHECK(p != nullptr);
    CHECK(aligned_to(p, alignof(struct probe)));
    CHECK(p->d == 0.0 && p->w == 0u);

    float *vec = ARENA_ARRAY(&a, float, 100u);
    CHECK(vec != nullptr);
    CHECK(arena_remaining(&a) <= 4096u);

    /* The SoA arrays of the engine are all cache-line aligned. */
    float *hot = (float *)arena_alloc(&a, sizeof(float) * 100u, PLACE_CACHELINE);
    CHECK(hot != nullptr && aligned_to(hot, PLACE_CACHELINE));

    arena_destroy(&a);
}

int main(void)
{
    test_init_invalid();
    test_base_alignment_and_oom();
    test_alloc0();
    test_mark_rewind_and_reset();
    test_typed_macros();

    if (g_failures == 0) {
        (void)printf("test_arena: all checks passed\n");
    } else {
        (void)printf("test_arena: %d check(s) failed\n", g_failures);
    }
    return g_failures;
}
