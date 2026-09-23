/*
 * spatial_grid.h - the uniform grid both the overlap count and the crossing
 * count build over the board.
 *
 * Two passes: one counts, per cell, how many item registrations it will hold;
 * a prefix sum turns that into offsets; a second pass scatters the items. An
 * item is registered in every cell its box covers, and a pair is examined in
 * the first cell of the intersection of the two ranges (cell_owns_pair) - the
 * only rule that counts a pair exactly once whatever their sizes.
 */
#ifndef PLACE_SPATIAL_GRID_H
#define PLACE_SPATIAL_GRID_H

#include "solver_internal.h"

/* At most this many cells per grid: the cell size grows on a large board
 * rather than the bookkeeping arrays, so the arena need stays predictable. */
#define GRID_MAX_CELLS 4096u

/* A cell holding more segments than this is skipped: the pair loop inside it
 * would dominate the whole evaluation for no useful signal. */
#define GRID_MAX_ITEMS_PER_CELL 32u

/* Conflicts are what legalisation exists to fix, so its grid registers far more
 * per cell: an LED array packs dozens of parts into one cell. */
#define GRID_CONFLICT_MAX_PER_CELL 512u

/* A net with more pins than this is a rail, not a route. */
#define SOLVER_MST_MAX_PINS 48u

/* How many cells a walked segment is registered in, at most. */
#define GRID_MAX_CELLS_PER_ITEM 8u

typedef struct {
    uint32_t cx0, cy0, cx1, cy1;
} cell_range_t;

void     grid_init(solver_t *s, solver_grid_t *g, coord_t cell, uint32_t items, uint32_t budget,
                   uint32_t max_per_cell);
cell_range_t grid_range(const solver_grid_t *g, coord_t x0, coord_t y0, coord_t x1, coord_t y1);
void     grid_begin(solver_grid_t *g);
void     grid_prefix(solver_grid_t *g);
void     grid_scatter(solver_grid_t *g, uint32_t item, const cell_range_t *r, uint32_t home_cell);
uint32_t grid_cell_of(const solver_grid_t *g, coord_t px, coord_t py);

/* The cells a segment passes through, written to `cells_out`; returns how many. */
uint32_t grid_cells_for_segment(const solver_grid_t *g, coord_t x1, coord_t y1, coord_t x2,
                                coord_t y2, uint32_t *cells_out);
bool     cell_owns_pair(const solver_grid_t *g, uint32_t i, uint32_t j, uint32_t cell);

#endif /* PLACE_SPATIAL_GRID_H */
