/*
 * spatial_query.c - the questions asked of the grid.
 *
 * grid_range turns a box into the cell range it covers; grid_cells_for_segment
 * walks a segment's cells; cell_owns_pair names the single cell in which a pair
 * is examined, which is what keeps every pair counted exactly once.
 */
#include "spatial_grid.h"

#include <math.h>

cell_range_t grid_range(const solver_grid_t *g, coord_t x0, coord_t y0, coord_t x1,
                               coord_t y1)
{
    coord_t fx0 = (x0 - g->origin_x) / g->cell;
    coord_t fy0 = (y0 - g->origin_y) / g->cell;
    coord_t fx1 = (x1 - g->origin_x) / g->cell;
    coord_t fy1 = (y1 - g->origin_y) / g->cell;
    if (fx0 < 0.0f) {
        fx0 = 0.0f;
    }
    if (fy0 < 0.0f) {
        fy0 = 0.0f;
    }
    const coord_t max_cx = (coord_t)(g->cols - 1u);
    const coord_t max_cy = (coord_t)(g->rows - 1u);
    if (fx1 > max_cx) {
        fx1 = max_cx;
    }
    if (fy1 > max_cy) {
        fy1 = max_cy;
    }
    cell_range_t r;
    r.cx0 = (uint32_t)fx0;
    r.cy0 = (uint32_t)fy0;
    r.cx1 = (uint32_t)fx1;
    r.cy1 = (uint32_t)fy1;
    return r;
}

bool cell_owns_pair(const solver_grid_t *g, uint32_t i, uint32_t j, uint32_t cell)
{
    const uint32_t ix0 = (g->cx0[i] > g->cx0[j]) ? g->cx0[i] : g->cx0[j];
    const uint32_t iy0 = (g->cy0[i] > g->cy0[j]) ? g->cy0[i] : g->cy0[j];
    return cell == iy0 * g->cols + ix0;
}

