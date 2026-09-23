/*
 * assign_hungarian.c - see assign_hungarian.h.
 *
 * The classical potentials formulation: rows are added one at a time and the
 * shortest alternating path to a free column is grown, each step adjusting the
 * column potentials by the slack it found. It is exact for a square matrix of
 * any sign and needs no initial feasible solution.
 */
#include "assign_hungarian.h"

#include <math.h>
#include <string.h>

#define ASSIGN_INF 1e30f

void assign_hungarian(const coord_t *cost, uint32_t n, uint32_t *assign, coord_t *u,
                      coord_t *v, uint32_t *p, uint32_t *way, coord_t *minv, uint8_t *used)
{
    if (n == 0u) {
        return;
    }
    for (uint32_t i = 0u; i <= n; ++i) {
        u[i] = 0.0f;
        v[i] = 0.0f;
        p[i] = 0u;
        way[i] = 0u;
    }
    for (uint32_t row = 1u; row <= n; ++row) {
        p[0] = row;
        uint32_t col = 0u;
        for (uint32_t j = 0u; j <= n; ++j) {
            minv[j] = ASSIGN_INF;
            used[j] = 0u;
        }
        do {
            used[col] = 1u;
            const uint32_t here = p[col];
            coord_t delta = ASSIGN_INF;
            uint32_t next = 0u;
            for (uint32_t j = 1u; j <= n; ++j) {
                if (used[j] != 0u) {
                    continue;
                }
                const coord_t cur = cost[(here - 1u) * n + (j - 1u)] - u[here] - v[j];
                if (cur < minv[j]) {
                    minv[j] = cur;
                    way[j] = col;
                }
                if (minv[j] < delta) {
                    delta = minv[j];
                    next = j;
                }
            }
            for (uint32_t j = 0u; j <= n; ++j) {
                if (used[j] != 0u) {
                    u[p[j]] += delta;
                    v[j] -= delta;
                } else {
                    minv[j] -= delta;
                }
            }
            col = next;
        } while (p[col] != 0u);
        do {
            const uint32_t back = way[col];
            p[col] = p[back];
            col = back;
        } while (col != 0u);
    }
    for (uint32_t j = 1u; j <= n; ++j) {
        if (p[j] != 0u) {
            assign[p[j] - 1u] = j - 1u;
        }
    }
}
