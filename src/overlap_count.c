/*
 * overlap_count.c - how many courtyard pairs collide, and how deeply.
 *
 * A pair belongs to the first cell of the intersection of the two ranges and
 * to that one only (cell_owns_pair): comparing the cells that hold the two
 * centres is not a property of the pair, and counted a pair once per shared
 * cell - or never, which is how the solver came to report five times the
 * collisions the board actually had.
 */
#include "solver_internal.h"
#include "shape.h"
#include "spatial_grid.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

uint32_t count_conflicts(solver_t *s, coord_t gap, uint32_t *locked_pairs,
                                coord_t *depth_out)
{
    solver_grid_t *g = &s->comp_grid;
    const uint32_t ncomp = s->ctx->comps.count;
    uint32_t overlaps = 0u;
    uint32_t locked = 0u;

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
    for (size_t cell = 0u; cell < cells; ++cell) {
        for (uint32_t a = g->start[cell]; a < g->start[cell + 1u]; ++a) {
            const uint32_t i = g->items[a];
            for (uint32_t b = a + 1u; b < g->start[cell + 1u]; ++b) {
                const uint32_t j = g->items[b];
                /* The pair belongs to the first cell of the intersection of the
                 * two ranges, and to that one only. The rule used to compare
                 * the cells holding the two centres, which is not a property of
                 * the pair: it counted a pair once per shared cell (a 3.5 mm
                 * part on a 1 mm grid shared twelve) or never, when the centres
                 * fell the wrong way round. Every overlap number the solver
                 * reported - and the penalty the annealer was optimising - came
                 * from that. */
                if (!cell_owns_pair(g, i, j, (uint32_t)cell)) {
                    continue;
                }
                if (shape_overlaps(s, i, j, gap)) {
                    if (!solver_component_is_movable(s, i) &&
                        !solver_component_is_movable(s, j)) {
                        locked += 1u; /* neither side can move: not the solver's to fix */
                    } else {
                        overlaps += 1u;
                        if (depth_out != nullptr) {
                            /* how deep the bite is, not just that there is one:
                             * a pair 0.01 mm in is a nudge, a part buried in a
                             * locked neighbour is a layout the router cannot
                             * save, and a count treats them the same. */
                            const coord_t pen_x = (s->half_w[i] + s->half_w[j] + gap) -
                                                  fabsf(s->x[i] - s->x[j]);
                            const coord_t pen_y = (s->half_h[i] + s->half_h[j] + gap) -
                                                  fabsf(s->y[i] - s->y[j]);
                            *depth_out += (pen_x < pen_y) ? pen_x : pen_y;
                        }
                    }
                }
            }
        }
    }
    if (locked_pairs != nullptr) {
        *locked_pairs = locked;
    }
    if (depth_out != nullptr) {
        *depth_out = 0.0f; /* the caller adds it up */
    }
    /* The spatial index is an optimisation, and an optimisation that misses a
     * pair would quietly legalise nothing. DODPLACE_BRUTE_CONFLICTS=1 re-counts
     * the whole board with a full O(n^2) scan and speaks up only when the two
     * disagree, which is the check that found the grid sound. */
    if (getenv("DODPLACE_BRUTE_CONFLICTS") != nullptr) {
        uint32_t brute = 0u;
        for (uint32_t i = 0u; i < ncomp; ++i) {
            for (uint32_t j = i + 1u; j < ncomp; ++j) {
                if (shape_overlaps(s, i, j, gap)) {
                    brute += 1u;
                }
            }
        }
        if (brute != overlaps + locked) {
            (void)fprintf(stderr,
                          "conflicts: the grid counted %u movable and %u locked, a full scan "
                          "found %u\n",
                          (unsigned)overlaps, (unsigned)locked, (unsigned)brute);
        }
    }
    return overlaps;
}

uint32_t solver_count_conflicts(solver_t *s, coord_t gap)
{
    return count_conflicts(s, gap, nullptr, nullptr);
}

uint32_t solver_count_locked_conflicts(solver_t *s, coord_t gap)
{
    uint32_t locked = 0u;
    (void)count_conflicts(s, gap, &locked, nullptr);
    return locked;
}

coord_t solver_overlap_depth(solver_t *s, coord_t gap)
{
    coord_t depth = 0.0f;
    (void)count_conflicts(s, gap, nullptr, &depth);
    return depth;
}

/*
 * Pin torque (ISPD-style orientation heuristic).
 *
 * A part with a net on one side and nothing on the other has one orientation
 * that points its pins at their partners and three that make the router go
 * around. The torque its connections exert is
 *
 *     tau = sum over pins of (r_pin x F_pin)
 *
 * with r_pin the pin offset from the part centre and F_pin the vector to the
 * centroid of the *other* pins on that net. The orientation that zeroes it is
 * the one where the pin field faces its wiring, and unlike a full cost
 * evaluation it costs one pass over the part's own pins.
 *
 * Multi-pin parts only: a two-terminal passive has nothing to untangle.
 */
