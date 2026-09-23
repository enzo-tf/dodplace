/*
 * dct.c - see dct.h.
 */
#include "dct.h"

#include <math.h>

#ifndef DCT_PI
#define DCT_PI 3.14159265358979323846f
#endif

bool dct_axis_init(dct_axis_t *axis, solver_t *s, uint32_t n)
{
    axis->n = n;
    axis->cos_tab = SOLVER_ALLOC(s, coord_t, (size_t)n * (size_t)n);
    if (axis->cos_tab == nullptr) {
        return false;
    }
    for (uint32_t k = 0u; k < n; ++k) {
        for (uint32_t i = 0u; i < n; ++i) {
            const coord_t angle =
                DCT_PI * (coord_t)k * (coord_t)(2u * i + 1u) / (coord_t)(2u * n);
            axis->cos_tab[(size_t)k * (size_t)n + i] = cosf(angle);
        }
    }
    return true;
}

void dct_axis_forward(const dct_axis_t *axis, const coord_t *x, coord_t *out)
{
    const uint32_t n = axis->n;
    for (uint32_t k = 0u; k < n; ++k) {
        const coord_t *row = &axis->cos_tab[(size_t)k * (size_t)n];
        coord_t sum = 0.0f;
        for (uint32_t i = 0u; i < n; ++i) {
            sum += x[i] * row[i];
        }
        out[k] = 2.0f * sum;
    }
}

void dct_axis_inverse(const dct_axis_t *axis, const coord_t *x, coord_t *out)
{
    const uint32_t n = axis->n;
    const coord_t inv_n = 1.0f / (coord_t)n;
    for (uint32_t i = 0u; i < n; ++i) {
        /* X[0] carries no factor of two: it is the mean, not a cosine pair. */
        coord_t sum = 0.5f * x[0];
        for (uint32_t k = 1u; k < n; ++k) {
            sum += x[k] * axis->cos_tab[(size_t)k * (size_t)n + i];
        }
        out[i] = inv_n * sum;
    }
}
