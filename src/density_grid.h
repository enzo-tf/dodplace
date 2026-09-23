/*
 * density_grid.h - the ePlace density model: bins, bins, Poisson, force.
 *
 * The heuristic the solver used to spread parts was a pairwise 1/d^2 repulsion:
 * O(n^2) per iteration, blind to anything but the two parts, and with no notion
 * of how much room there is. ePlace replaces it with a field. The board is cut
 * into bins, each bin's occupancy is the share of its area the parts cover, and
 * the potential solves
 *
 *     div(grad phi) = -rho          (no-flux boundary, so nothing leaks out)
 *
 * whose gradient pushes a part away from its crowded neighbours and towards the
 * holes - the one force that knows the whole board at once, for the price of a
 * transform. Dense *and* uniform both give a flat field: what moves a part is
 * the *difference* in occupancy around it, which is exactly what the pairwise
 * heuristic could not see.
 */
#ifndef PLACE_DENSITY_GRID_H
#define PLACE_DENSITY_GRID_H

#include "dct.h"
#include "solver_internal.h"

typedef struct {
    uint32_t nx;
    uint32_t ny;
    coord_t  cell_x;
    coord_t  cell_y;
    coord_t  origin_x;
    coord_t  origin_y;
    coord_t *rho;  /* occupancy, area covered / bin area */
    coord_t *phi;  /* potential */
    coord_t *ex;   /* electric field, -d phi / d x */
    coord_t *ey;
    coord_t *line; /* one row or column of work */
    dct_axis_t axis_x;
    dct_axis_t axis_y;
} density_grid_t;

/* Bins across the board's own bounding box; n must be at least 4. */
bool density_grid_init(solver_t *s, density_grid_t *g, uint32_t nx, uint32_t ny);

/* rho <- the parts' courtyards, by overlap area. Locked parts charge too: they
 * hold their ground, and the room they take is not available to anyone else. */
void density_rasterise(const solver_t *s, density_grid_t *g);

/* rho -> phi -> (ex, ey). Exact, one transform each way, no iteration. */
void density_solve(density_grid_t *g);

/* The separable 2-D transform, in place. Used by the solver and by its test. */
void density_transform_2d(density_grid_t *g, coord_t *grid, bool forward);

/* The field at a point, bilinear on the bins. */
void density_force_at(const density_grid_t *g, coord_t x, coord_t y, coord_t *fx,
                      coord_t *fy);

#endif /* PLACE_DENSITY_GRID_H */
