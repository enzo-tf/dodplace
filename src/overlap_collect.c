/*
 * overlap_collect.c - the conflicting pairs, materialised for the legaliser.
 *
 * A pair is reported when at least one side can move: two locked parts cannot
 * be separated by the solver, and reporting them would have the legaliser
 * pushing against a wall it is not allowed to touch.
 */
#include "solver_internal.h"
#include "shape.h"
#include "spatial_grid.h"

#include <math.h>

uint32_t solver_collect_conflicts(solver_t *s, coord_t gap, uint32_t *out_i, uint32_t *out_j,
                                  uint32_t max)
{
    solver_grid_t *g = &s->comp_grid;
    const uint32_t ncomp = s->ctx->comps.count;
    uint32_t found = 0u;

    grid_begin(g);
    uint32_t counted = 0u;
    for (uint32_t i = 0u; i < ncomp; ++i) {
        const coord_t hw = s->half_w[i] + gap * 0.5f;
        const coord_t hh = s->half_h[i] + gap * 0.5f;
        const cell_range_t r = grid_range(g, s->x[i] - hw, s->y[i] - hh, s->x[i] + hw,
                                          s->y[i] + hh);
        for (uint32_t cy = r.cy0; cy <= r.cy1; ++cy) {
            for (uint32_t cx = r.cx0; cx <= r.cx1; ++cx) {
                const uint32_t cell = cy * g->cols + cx;
                if (g->cursor[cell] >= g->max_per_cell || counted >= g->item_budget) {
                    continue;
                }
                g->cursor[cell] += 1u;
                g->start[cell + 1u] += 1u;
                counted += 1u;
            }
        }
    }
    grid_prefix(g);
    for (uint32_t i = 0u; i < ncomp; ++i) {
        const coord_t hw = s->half_w[i] + gap * 0.5f;
        const coord_t hh = s->half_h[i] + gap * 0.5f;
        const cell_range_t r = grid_range(g, s->x[i] - hw, s->y[i] - hh, s->x[i] + hw,
                                          s->y[i] + hh);
        grid_scatter(g, i, &r, grid_cell_of(g, s->x[i], s->y[i]));
    }

    const size_t cells = (size_t)g->cols * (size_t)g->rows;
    for (size_t cell = 0u; cell < cells && found < max; ++cell) {
        for (uint32_t a = g->start[cell]; a < g->start[cell + 1u]; ++a) {
            const uint32_t i = g->items[a];
            for (uint32_t b = a + 1u; b < g->start[cell + 1u]; ++b) {
                const uint32_t j = g->items[b];
                if (!cell_owns_pair(g, i, j, (uint32_t)cell)) {
                    continue; /* the pair is counted in its first shared cell */
                }
                if (shape_overlaps(s, i, j, gap)) {
                    if (!solver_component_is_movable(s, i) &&
                        !solver_component_is_movable(s, j)) {
                        continue; /* nothing the legaliser could do about it */
                    }
                    if (found < max) {
                        out_i[found] = i;
                        out_j[found] = j;
                    }
                    found += 1u;
                }
            }
        }
    }
    return found < max ? found : max;
}

/*
 * One pass over the pairs, without materialising them.
 *
 * Pairs of two locked parts are counted apart and never mixed into the solver's
 * own score: it cannot move either side, so a collision between them is the
 * designer's layout, not a placement failure. Keeping them in the same number
 * made a correct run look broken.
 */
