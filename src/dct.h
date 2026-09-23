/*
 * dct.h - the cosine transform the density solver is built on.
 *
 * ePlace spreads a placement by solving a Poisson equation for the density
 * field. On a uniform grid with no-flux boundaries the Laplacian is diagonal in
 * the DCT-II basis, so the solve is one transform, one division per coefficient
 * and one inverse transform - no iterations, no convergence criterion, and the
 * result is exact to float precision.
 *
 * The transform here is the direct separable one: the grid is 64x64 bins, so a
 * row costs 64 multiply-adds per coefficient and the whole 2-D transform about
 * 0.5 Mflop - small enough that a FFT-based O(N log N) DCT would only add code
 * to optimise. The tables are built once per solve, not per iteration.
 */
#ifndef PLACE_DCT_H
#define PLACE_DCT_H

#include "solver_internal.h"

typedef struct {
    uint32_t n;
    coord_t *cos_tab; /* n*n: cos(pi k (2i+1) / (2n)), row-major by k */
} dct_axis_t;

/* One axis of length n, tables from the scratch arena. */
bool dct_axis_init(dct_axis_t *axis, solver_t *s, uint32_t n);

/* X[k] = 2 sum_i x[i] cos(pi k (2i+1) / (2n)) */
void dct_axis_forward(const dct_axis_t *axis, const coord_t *x, coord_t *out);

/* The inverse of the above: the DCT-III pair, X[0] halved. */
void dct_axis_inverse(const dct_axis_t *axis, const coord_t *x, coord_t *out);

#endif /* PLACE_DCT_H */
