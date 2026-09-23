/*
 * test_assign.c - the exact bijection.
 *
 * The reason this exists at all is the three-cycle: a cost matrix where the
 * optimal permutation is a rotation and *every* single transposition of the
 * starting arrangement is worse. A pairwise search cannot leave that point, the
 * Hungarian method cannot miss it. The second test checks the solver against
 * brute force on a matrix small enough to enumerate.
 */
#include "assign_hungarian.h"
#include "solver_internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_failures;

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            (void)printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                                \
        }                                                                \
    } while (0)

static coord_t solve(const coord_t *cost, uint32_t n, uint32_t *assign)
{
    coord_t u[8];
    coord_t v[8];
    uint32_t p[8];
    uint32_t way[8];
    coord_t minv[8];
    uint8_t used[8];
    assign_hungarian(cost, n, assign, u, v, p, way, minv, used);
    coord_t total = 0.0f;
    for (uint32_t i = 0u; i < n; ++i) {
        total += cost[i * n + assign[i]];
    }
    return total;
}

static void test_the_three_cycle_no_pairwise_search_can_reach(void)
{
    /* Identity: 1+1+1 = 3. The rotation: 0+0+2 = 2. Every transposition of the
     * identity: 0+3+1 = 4, 1+0+3 = 4, 1+1+2 = 4. */
    const coord_t cost[9] = {1.0f, 0.0f, 1.0f, 3.0f, 1.0f, 0.0f, 2.0f, 3.0f, 1.0f};
    uint32_t assign[3] = {0u, 0u, 0u};
    CHECK(solve(cost, 3u, assign) < 2.01f);
    CHECK(assign[0] == 1u);
    CHECK(assign[1] == 2u);
    CHECK(assign[2] == 0u);
}

static void test_against_brute_force(void)
{
    /* A fixed, deliberately awkward 5x5: some rows are cheap on one column only,
     * which is what makes greedy tie itself in knots. */
    coord_t cost[25];
    uint32_t state = 12345u;
    for (uint32_t i = 0u; i < 25u; ++i) {
        state = state * 1103515245u + 12345u;
        cost[i] = (coord_t)((state >> 16) % 97u);
    }
    uint32_t assign[5] = {0u, 0u, 0u, 0u, 0u};
    const coord_t got = solve(cost, 5u, assign);

    uint32_t perm[5] = {0u, 1u, 2u, 3u, 4u};
    coord_t best = 1e30f;
    uint32_t count = 0u;
    do {
        coord_t total = 0.0f;
        for (uint32_t i = 0u; i < 5u; ++i) {
            total += cost[i * 5u + perm[i]];
        }
        if (total < best) {
            best = total;
        }
        /* next permutation, in place */
        uint32_t i = 4u;
        while (i > 0u && perm[i - 1u] >= perm[i]) {
            i -= 1u;
        }
        if (i == 0u) {
            break;
        }
        uint32_t j = 4u;
        while (perm[j] <= perm[i - 1u]) {
            j -= 1u;
        }
        const uint32_t swap = perm[i - 1u];
        perm[i - 1u] = perm[j];
        perm[j] = swap;
        for (uint32_t lo = i, hi = 4u; lo < hi; ++lo, --hi) {
            const uint32_t t = perm[lo];
            perm[lo] = perm[hi];
            perm[hi] = t;
        }
        count += 1u;
    } while (true);
    CHECK(count == 119u); /* every other ordering */
    CHECK(fabsf(got - best) < 1e-3f);
}

int main(void)
{
    test_the_three_cycle_no_pairwise_search_can_reach();
    test_against_brute_force();
    if (g_failures == 0) {
        (void)printf("test_assign: all checks passed\n");
    } else {
        (void)printf("test_assign: %d check(s) failed\n", g_failures);
    }
    return g_failures;
}
