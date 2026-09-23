/*
 * polish.c - see polish.h.
 */
#include "polish.h"
#include "copper_test.h"
#include "shape.h"

#include <math.h>

/* Small offsets, nearest first: the deficit is usually microns, not tenths. */
static const coord_t k_offsets[][2] = {
    {0.05f, 0.0f},   {-0.05f, 0.0f},  {0.0f, 0.05f},   {0.0f, -0.05f},
    {0.05f, 0.05f},  {-0.05f, 0.05f}, {0.05f, -0.05f}, {-0.05f, -0.05f},
    {0.10f, 0.0f},   {-0.10f, 0.0f},  {0.0f, 0.10f},   {0.0f, -0.10f},
    {0.15f, 0.0f},   {-0.15f, 0.0f},  {0.0f, 0.15f},   {0.0f, -0.15f},
    {0.20f, 0.0f},   {-0.20f, 0.0f},  {0.0f, 0.20f},   {0.0f, -0.20f},
};
#define POLISH_OFFSETS (sizeof k_offsets / sizeof k_offsets[0])

/*
 * Would the part be legal at (x, y)?
 *
 * The hard rules are the ordinary ones - no courtyard or copper conflict with
 * anyone. The polish margin is a heuristic for where KiCad's mask rule bites,
 * so it is required only of the pair being fixed: demanding it of every
 * neighbour too made the pass refuse every candidate in a dense area, and Q8
 * (four bridges on its own) never moved at all.
 */
static bool position_is_clean(const solver_t *s, uint32_t comp, coord_t x, coord_t y,
                              coord_t gap, coord_t polish_gap, uint32_t target)
{
    if (!solver_within_anchor(s, comp, x, y)) {
        return false;
    }
    /* Inside the board, with the part's own extents. */
    if (x - s->half_w[comp] < s->board_min_x || x + s->half_w[comp] > s->board_max_x ||
        y - s->half_h[comp] < s->board_min_y || y + s->half_h[comp] > s->board_max_y) {
        return false;
    }
    for (uint32_t j = 0u; j < s->ncomp; ++j) {
        if (j == comp || s->ruin_mask[j] != 0u) {
            continue;
        }
        if (shape_overlaps_posed(s, comp, x, y, j, gap)) {
            return false; /* a real conflict: not worth a mask bridge */
        }
        if (j == target && copper_overlaps(s, comp, x, y, j, s->x[j], s->y[j], polish_gap)) {
            return false; /* the pair this pass is for is still too close */
        }
    }
    return true;
}

/* Which of the pair should move: the movable one, the smaller if both. */
static uint32_t polish_mover(const solver_t *s, uint32_t i, uint32_t j)
{
    const bool mi = solver_component_is_movable(s, i);
    const bool mj = solver_component_is_movable(s, j);
    if (mi && mj) {
        const coord_t a_i = s->half_w[i] * s->half_h[i];
        const coord_t a_j = s->half_w[j] * s->half_h[j];
        return (a_i <= a_j) ? i : j;
    }
    return mi ? i : (mj ? j : SOLVER_NO_INDEX);
}

void solver_polish(solver_t *s)
{
    if (s->opt->polish_reach <= 0.0f || s->ncomp == 0u || s->ctx->pins.half_x == nullptr) {
        return;
    }
    const coord_t gap = s->opt->courtyard_clearance;
    const coord_t polish_gap = s->opt->pad_clearance + s->opt->polish_reach;

    for (uint32_t i = 0u; i < s->ncomp; ++i) {
        for (uint32_t j = 0u; j < s->ncomp; ++j) {
            if (j == i || s->ruin_mask[j] != 0u) {
                continue;
            }
            if (!copper_overlaps(s, i, s->x[i], s->y[i], j, s->x[j], s->y[j], polish_gap)) {
                continue; /* the mask rule is satisfied here */
            }
            const uint32_t mover = polish_mover(s, i, j);
            if (mover == SOLVER_NO_INDEX) {
                continue; /* two locked parts: the designer's own layout */
            }
            const coord_t from_x = s->x[mover];
            const coord_t from_y = s->y[mover];
            for (uint32_t k = 0u; k < POLISH_OFFSETS; ++k) {
                const coord_t nx = from_x + k_offsets[k][0];
                const coord_t ny = from_y + k_offsets[k][1];
                if (!position_is_clean(s, mover, nx, ny, gap, polish_gap, (mover == i) ? j : i)) {
                    continue;
                }
                solver_nudge(s, mover, nx - from_x, ny - from_y);
                s->stats.polished += 1u;
                break;
            }
        }
    }
}
