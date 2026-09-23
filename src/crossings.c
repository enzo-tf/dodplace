/*
 * crossings.c - how tangled the ratsnest is.
 *
 * Two segments of different nets that intersect are a crossing; a net crossing
 * itself is not a routability problem, so pairs of the same net are skipped.
 * Pairs are examined once, in the first cell of the intersection of their
 * walked ranges (cell_owns_pair).
 */
#include "solver_internal.h"
#include "ratsnest.h"
#include "spatial_grid.h"

#include <math.h>

static coord_t cross2(coord_t ax, coord_t ay, coord_t bx, coord_t by, coord_t cx, coord_t cy)
{
    return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
}

static bool segments_intersect(coord_t x1, coord_t y1, coord_t x2, coord_t y2, coord_t x3,
                               coord_t y3, coord_t x4, coord_t y4)
{
    const coord_t d1 = cross2(x3, y3, x4, y4, x1, y1);
    const coord_t d2 = cross2(x3, y3, x4, y4, x2, y2);
    const coord_t d3 = cross2(x1, y1, x2, y2, x3, y3);
    const coord_t d4 = cross2(x1, y1, x2, y2, x4, y4);
    return ((d1 > 0.0f) != (d2 > 0.0f)) && ((d3 > 0.0f) != (d4 > 0.0f));
}

/*
 * The ratsnest is a star per net: every pin is connected to the net's leftmost
 * pin. A minimum spanning tree would be shorter, and a sorted chain slightly
 * shorter still, but both cost a sort per evaluation and the crossing count
 * only needs a topology that follows the net's shape. A star is O(pins) with no
 * sorting at all, which matters because this runs on every annealing step.
 */
/*
 * The ratsnest is a rectilinear minimum spanning tree per net, grown with Prim
 * from the pin nearest the net's centre.
 *
 * It used to be a star around the leftmost pin, which made the crossing count a
 * function of which pin happened to be leftmost: moving a part by a millimetre
 * could flip the hub, redraw every edge of the net and change the count without
 * the layout having changed at all. The annealer was optimising noise, which is
 * why raising the crossing weight only degraded the wirelength. An MST is
 * stable under small moves, its edges are the ones a router actually lays down,
 * and it has exactly the same k-1 edges as the star, so nothing downstream
 * changes size.
 */
uint32_t count_crossings(solver_t *s)
{
    solver_grid_t *g = &s->seg_grid;
    if (s->nsegments == 0u) {
        return 0u;
    }
    grid_begin(g);
    uint32_t counted = 0u;
    for (uint32_t i = 0u; i < s->nsegments; ++i) {
        uint32_t cells[GRID_MAX_CELLS_PER_ITEM];
        const uint32_t n = grid_cells_for_segment(g, s->seg_x1[i], s->seg_y1[i], s->seg_x2[i],
                                                  s->seg_y2[i], cells);
        for (uint32_t k = 0u; k < n; ++k) {
            const uint32_t cell = cells[k];
            if (g->cursor[cell] >= g->max_per_cell || counted >= g->item_budget) {
                continue;
            }
            g->cursor[cell] += 1u;
            g->start[cell + 1u] += 1u;
            counted += 1u;
        }
    }
    grid_prefix(g);
    for (uint32_t i = 0u; i < s->nsegments; ++i) {
        const coord_t mx = (s->seg_x1[i] + s->seg_x2[i]) * 0.5f;
        const coord_t my = (s->seg_y1[i] + s->seg_y2[i]) * 0.5f;
        g->home[i] = grid_cell_of(g, mx, my);
        uint32_t cells[GRID_MAX_CELLS_PER_ITEM];
        const uint32_t n = grid_cells_for_segment(g, s->seg_x1[i], s->seg_y1[i], s->seg_x2[i],
                                                  s->seg_y2[i], cells);
        /* the ownership rule needs a range per entry; for a walked segment the
         * cells themselves are the range */
        g->cx0[i] = cells[0];
        g->cy0[i] = 0u;
        g->cx1[i] = cells[0];
        g->cy1[i] = 0u;
        for (uint32_t k = 0u; k < n; ++k) {
            g->items[g->cursor[cells[k]]++] = i;
        }
    }

    uint32_t crossings = 0u;
    const size_t cells = (size_t)g->cols * (size_t)g->rows;
    for (size_t cell = 0u; cell < cells; ++cell) {
        const uint32_t cell_count = g->start[cell + 1u] - g->start[cell];
        if (cell_count > GRID_MAX_ITEMS_PER_CELL) {
            continue; /* too dense to say anything about individual pairs */
        }
        for (uint32_t a = g->start[cell]; a < g->start[cell + 1u]; ++a) {
            const uint32_t i = g->items[a];
            for (uint32_t b = a + 1u; b < g->start[cell + 1u]; ++b) {
                const uint32_t j = g->items[b];
                if (s->seg_net[i] == s->seg_net[j]) {
                    continue;
                }
                if (!cell_owns_pair(g, i, j, (uint32_t)cell)) {
                    continue; /* the pair is counted in its first shared cell */
                }
                if (segments_intersect(s->seg_x1[i], s->seg_y1[i], s->seg_x2[i], s->seg_y2[i],
                                       s->seg_x1[j], s->seg_y1[j], s->seg_x2[j], s->seg_y2[j])) {
                    crossings += 1u;
                }
            }
        }
    }
    return crossings;
}

/* ========================================================================= */
/* Courtyard overlaps                                                        */
/* ========================================================================= */

/*
 * The union of the part's box, or of the whole cluster it masters, as offsets
 * from its centre. A cluster moves as one body, so a slot is only free if the
 * union fits - the master's own box says nothing about its satellites.
 */
