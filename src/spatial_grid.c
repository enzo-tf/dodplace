/*
 * spatial_grid.c - see spatial_grid.h.
 */
#include "spatial_grid.h"

#include <math.h>
#include <string.h>

/* At most this many cells per grid: the cell size grows on a large board
 * rather than the bookkeeping arrays, so the arena need stays predictable. */
#define GRID_MAX_CELLS 4096u

/* A cell holding more segments than this is skipped: the pair loop inside it
 * would dominate the whole evaluation for no useful signal. */
#define GRID_MAX_ITEMS_PER_CELL 32u

/* Conflicts are what legalisation exists to fix, so its grid registers far more
 * per cell: an LED array packs dozens of parts into one cell and skipping them
 * would hide exactly the collisions that matter. */
#define GRID_CONFLICT_MAX_PER_CELL 512u

/* A net with more pins than this is a rail, not a route: its MST would cost
 * O(k^2) per evaluation and its crossings say nothing useful. */
#define SOLVER_MST_MAX_PINS 48u

/*
 * `items` is how many parts the grid must track; `budget` is how many
 * (part, cell) registrations the item list can hold. The arena is never rewound
 * mid-solve, so both are allocated once here: a registration list grown per
 * evaluation would exhaust it after a few thousand moves.
 */
void grid_init(solver_t *s, solver_grid_t *g, coord_t cell, uint32_t items,
                      uint32_t budget, uint32_t max_per_cell)
{
    coord_t span_x = s->board_max_x - s->board_min_x;
    coord_t span_y = s->board_max_y - s->board_min_y;
    if (cell <= 0.0f) {
        cell = 4.0f;
    }
    {
        const coord_t area = (span_x > 0.0f ? span_x : 1.0f) * (span_y > 0.0f ? span_y : 1.0f);
        const coord_t min_cell = sqrtf(area / (coord_t)GRID_MAX_CELLS);
        if (cell < min_cell) {
            cell = min_cell;
        }
    }
    if (span_x < cell) {
        span_x = cell;
    }
    if (span_y < cell) {
        span_y = cell;
    }
    g->cell = cell;
    g->origin_x = s->board_min_x - cell;
    g->origin_y = s->board_min_y - cell;
    g->cols = (uint32_t)(span_x / cell) + 3u;
    g->rows = (uint32_t)(span_y / cell) + 3u;
    const size_t cells = (size_t)g->cols * (size_t)g->rows;
    g->item_budget = budget;
    g->max_per_cell = max_per_cell;
    g->start = SOLVER_ALLOC(s, uint32_t, cells + 1u);
    g->cursor = SOLVER_ALLOC(s, uint32_t, cells);
    g->items = SOLVER_ALLOC(s, uint32_t, (budget > 0u) ? budget : 1u);
    g->home = SOLVER_ALLOC(s, uint32_t, (items > 0u) ? items : 1u);
    g->cx0 = SOLVER_ALLOC(s, uint32_t, (items > 0u) ? items : 1u);
    g->cy0 = SOLVER_ALLOC(s, uint32_t, (items > 0u) ? items : 1u);
    g->cx1 = SOLVER_ALLOC(s, uint32_t, (items > 0u) ? items : 1u);
    g->cy1 = SOLVER_ALLOC(s, uint32_t, (items > 0u) ? items : 1u);
    if (g->start == nullptr || g->cursor == nullptr || g->items == nullptr ||
        g->home == nullptr || g->cx0 == nullptr || g->cy0 == nullptr ||
        g->cx1 == nullptr || g->cy1 == nullptr) {
        s->ok = false;
    }
}


void grid_begin(solver_grid_t *g)
{
    const size_t cells = (size_t)g->cols * (size_t)g->rows;
    memset(g->start, 0, (cells + 1u) * sizeof(uint32_t));
    /* the counting pass uses cursor as its per-cell occupancy counter, so it
     * must start from zero - not from the offsets the previous pass left */
    memset(g->cursor, 0, cells * sizeof(uint32_t));
}

void grid_prefix(solver_grid_t *g)
{
    const size_t cells = (size_t)g->cols * (size_t)g->rows;
    for (size_t i = 0u; i < cells; ++i) {
        g->start[i + 1u] += g->start[i];
        g->cursor[i] = g->start[i];
    }
}

void grid_scatter(solver_grid_t *g, uint32_t item, const cell_range_t *r,
                         uint32_t home_cell)
{
    g->home[item] = home_cell;
    g->cx0[item] = r->cx0;
    g->cy0[item] = r->cy0;
    g->cx1[item] = r->cx1;
    g->cy1[item] = r->cy1;
    for (uint32_t cy = r->cy0; cy <= r->cy1; ++cy) {
        for (uint32_t cx = r->cx0; cx <= r->cx1; ++cx) {
            const uint32_t cell = cy * g->cols + cx;
            /* never past the slots the counting pass reserved: a bounding box
             * can cover more cells than the estimate assumed */
            if (g->cursor[cell] < g->start[cell + 1u]) {
                g->items[g->cursor[cell]++] = item;
            }
        }
    }
}

/*
 * The cells a segment is registered in, walked along its length rather than
 * across its bounding box: sampling the box would put a diagonal segment in
 * cells it never enters, and miss the ones it does. The step is bounded so a
 * segment spanning the whole board cannot flood the grid.
 */
uint32_t grid_cell_of(const solver_grid_t *g, coord_t px, coord_t py);

uint32_t grid_cell_of(const solver_grid_t *g, coord_t px, coord_t py);

uint32_t grid_cells_for_segment(const solver_grid_t *g, coord_t x1, coord_t y1,
                                       coord_t x2, coord_t y2, uint32_t *cells_out)
{
    const coord_t dx = x2 - x1;
    const coord_t dy = y2 - y1;
    const coord_t length = sqrtf(dx * dx + dy * dy);
    uint32_t steps = 1u;
    if (g->cell > 0.0f) {
        steps = (uint32_t)(length / g->cell) + 1u;
    }
    if (steps > GRID_MAX_CELLS_PER_ITEM) {
        steps = GRID_MAX_CELLS_PER_ITEM;
    }
    uint32_t n = 0u;
    for (uint32_t k = 0u; k < steps; ++k) {
        const coord_t t = (coord_t)k / (coord_t)(steps > 1u ? steps - 1u : 1u);
        const uint32_t cell = grid_cell_of(g, x1 + dx * t, y1 + dy * t);
        bool seen = false;
        for (uint32_t q = 0u; q < n; ++q) {
            if (cells_out[q] == cell) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            cells_out[n++] = cell;
        }
    }
    return n;
}

uint32_t grid_cell_of(const solver_grid_t *g, coord_t px, coord_t py)
{
    coord_t fx = (px - g->origin_x) / g->cell;
    coord_t fy = (py - g->origin_y) / g->cell;
    if (fx < 0.0f) {
        fx = 0.0f;
    }
    if (fy < 0.0f) {
        fy = 0.0f;
    }
    const uint32_t cx = (uint32_t)fx < g->cols ? (uint32_t)fx : g->cols - 1u;
    const uint32_t cy = (uint32_t)fy < g->rows ? (uint32_t)fy : g->rows - 1u;
    return cy * g->cols + cx;
}

/* ========================================================================= */
/* Ratsnest and crossings                                                    */
/* ========================================================================= */

/*
 * True when `cell` is the first cell of the intersection of the two entries'
 * ranges - the single place where the pair must be counted.
 */
