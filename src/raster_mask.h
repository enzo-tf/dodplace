/*
 * raster_mask.h - discrete occupancy of the board.
 *
 * One bit per cell, 1 = blocked. A cell (cx, cy) covers
 *   [origin_x + cx*cell, origin_x + (cx+1)*cell) x [same in y)
 * and a box is stamped over the cells whose CENTRE falls inside it.
 *
 * Invariants:
 *   - the grid is anchored on the board outline (edge_*), never on the
 *     margin-inset box: a cell that does not exist reads as blocked, and the
 *     band between the margin and the edge is exactly where the last-resort
 *     searches have to look;
 *   - `bits` is cols*rows bits, rounded up to whole words, allocated from the
 *     scratch arena; the module never allocates;
 *   - a clear answer from raster_mask_box_clear is a proof of clearance for
 *     the stamped obstacles; a blocked answer only means "test exactly".
 */
#ifndef PLACE_RASTER_MASK_H
#define PLACE_RASTER_MASK_H

#include "solver_internal.h"

typedef struct {
    uint32_t  cols;
    uint32_t  rows;
    coord_t   cell;
    coord_t   origin_x;
    coord_t   origin_y;
    uint64_t *bits;
    size_t    words;
    coord_t   ceiling;   /* mm above the board; 0 = no height test */
} raster_mask_t;

/* Sizes and allocates the grid for this solver's board. False on failure. */
bool raster_mask_init(raster_mask_t *m, const solver_t *s, coord_t cell);

/* Blocks every cell a box's centre-sampling covers. */
void raster_mask_stamp_box(raster_mask_t *m, coord_t lo_x, coord_t lo_y, coord_t hi_x,
                           coord_t hi_y);

/*
 * True when a part `height` tall fits here: no blocked cell inside the box, and
 * the part clears the enclosure. A part taller than the ceiling fits nowhere,
 * so the answer is false everywhere and the caller leaves it where it is.
 */
bool raster_mask_box_clear(const raster_mask_t *m, coord_t height, coord_t lo_x, coord_t lo_y,
                           coord_t hi_x, coord_t hi_y);

/*
 * Blocks the locked components (their courtyard union plus the DRC gap), the
 * keepout polygons, and everything outside the board outline.
 */
void raster_mask_build_obstacles(raster_mask_t *m, const solver_t *s, coord_t gap);

#endif /* PLACE_RASTER_MASK_H */
