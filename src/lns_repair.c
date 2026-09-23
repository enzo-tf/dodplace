/*
 * lns_repair.c - large-neighbourhood search: where a stuck part can go.
 *
 * The legaliser pushes and this module decides, when pushing is not enough.
 * Every candidate slot is proved legal before it is taken - inside the board,
 * clear of every other courtyard by the DRC clearance - so a placement made
 * here is never the source of a later conflict.
 */
#include "solver_internal.h"
#include "lns_repair.h"
#include "shape.h"

#include <math.h>

/* Would the part's union box fit inside the board at (x, y)? */
static bool lns_slot_inside_board(const solver_t *s, uint32_t comp, coord_t x, coord_t y)
{
    coord_t lo_x = 0.0f;
    coord_t lo_y = 0.0f;
    coord_t hi_x = 0.0f;
    coord_t hi_y = 0.0f;
    solver_union_extent(s, comp, &lo_x, &lo_y, &hi_x, &hi_y);
    if (x + lo_x < s->board_min_x || x + hi_x > s->board_max_x ||
        y + lo_y < s->board_min_y || y + hi_y > s->board_max_y) {
        return false;
    }
    /* And inside the part's own anchor box: the board allows a slot the lane
     * does not, and the lane is what keeps the layout the designer drew. */
    return solver_within_anchor(s, comp, x, y);
}

/*
 * Is a slot free for `comp` - its whole cluster included - against every other
 * part, with the courtyard clearance respected?
 */
bool lns_slot_free(const solver_t *s, uint32_t comp, coord_t x, coord_t y, coord_t gap)
{
    coord_t lo_x = 0.0f;
    coord_t lo_y = 0.0f;
    coord_t hi_x = 0.0f;
    coord_t hi_y = 0.0f;
    solver_union_extent(s, comp, &lo_x, &lo_y, &hi_x, &hi_y);

    for (uint32_t j = 0u; j < s->ncomp; ++j) {
        if (j == comp || s->ruin_mask[j] != 0u) {
            continue;
        }
        if (shape_overlaps_posed(s, comp, x, y, j, gap)) {
            return false;
        }
    }
    return true;
}

/*
 * Discrete slot search: a fine grid walked ring by ring outwards from where the
 * part wants to be, nearest slot first.
 *
 * Continuous pushes cannot converge for a part wedged between locked
 * neighbours - every direction is blocked and the pair loop just oscillates,
 * which is what left a handful of collisions behind on a dense board. The
 * array around such a part does have free slots; this finds the closest one.
 */
static bool lns_spiral_slot_from(solver_t *s, uint32_t comp, coord_t cx, coord_t cy, coord_t *out_x,
                             coord_t *out_y)
{
    const coord_t gap = s->opt->courtyard_clearance;
    const coord_t biggest = (s->half_w[comp] > s->half_h[comp]) ? s->half_w[comp]
                                                                : s->half_h[comp];
    /* Half a part per step: fine enough to land in the gaps of a dense array,
     * coarse enough that sixteen rings stay a bounded search. */
    const coord_t step = biggest * 0.5f + gap + 0.25f;
    coord_t lo_x = 0.0f;
    coord_t lo_y = 0.0f;
    coord_t hi_x = 0.0f;
    coord_t hi_y = 0.0f;
    solver_union_extent(s, comp, &lo_x, &lo_y, &hi_x, &hi_y);

    /* Reaches across the whole board, because on a packed one the nearest free
     * slot may be a hundred millimetres away and a capped search would report
     * "nowhere to go" while the board still has room. The walk stops at the
     * first hit and candidates off the board cost one comparison. */
    const coord_t span_x = s->board_max_x - s->board_min_x;
    const coord_t span_y = s->board_max_y - s->board_min_y;
    const coord_t diagonal = sqrtf(span_x * span_x + span_y * span_y);
    const int32_t max_rings = (int32_t)(diagonal / step) + 2;
    for (int32_t ring = 1; ring <= max_rings; ++ring) {
        for (int32_t gy = -ring; gy <= ring; ++gy) {
            for (int32_t gx = -ring; gx <= ring; ++gx) {
                if (gx > -ring && gx < ring && gy > -ring && gy < ring) {
                    continue; /* the ring, not its interior: already searched */
                }
                const coord_t nx = cx + (coord_t)gx * step;
                const coord_t ny = cy + (coord_t)gy * step;
                if (nx + lo_x < s->board_min_x || nx + hi_x > s->board_max_x ||
                    ny + lo_y < s->board_min_y || ny + hi_y > s->board_max_y) {
                    continue;
                }
                if (!solver_within_anchor(s, comp, nx, ny)) {
                    continue; /* outside the lane: no slot, however free it is */
                }
                if (!lns_slot_free(s, comp, nx, ny, gap)) {
                    continue;
                }
                *out_x = nx;
                *out_y = ny;
                return true;
            }
        }
    }
    return false;
}

bool lns_spiral_slot(solver_t *s, uint32_t comp, coord_t *out_x, coord_t *out_y)
{
    return lns_spiral_slot_from(s, comp, s->x[comp], s->y[comp], out_x, out_y);
}

/*
 * The designer's slots.
 *
 * On a board packed to 74 % with locked parts, the free space is not a
 * continuous field: it is the set of spots the movable parts used to occupy.
 * A lattice spiral walks over them without ever landing on one - the lattice is
 * anchored on the part's current position and the holes are elsewhere - so the
 * search here is over the seed positions themselves, nearest first. It is the
 * ECO heuristic: to place a part, look where parts like it are allowed to be.
 */
bool lns_seed_slot(solver_t *s, uint32_t comp, coord_t *out_x, coord_t *out_y)
{
    const components_soa_t *c = &s->ctx->comps;
    const coord_t gap = s->opt->courtyard_clearance;
    const coord_t from_x = s->x[comp];
    const coord_t from_y = s->y[comp];
    coord_t best_d = 0.0f;
    coord_t best_x = 0.0f;
    coord_t best_y = 0.0f;
    bool found = false;

    for (uint32_t j = 0u; j < s->ncomp; ++j) {
        if (j == comp || !solver_component_is_movable(s, j)) {
            continue;
        }
        const coord_t x = c->x[j];
        const coord_t y = c->y[j];
        const coord_t d = fabsf(x - from_x) + fabsf(y - from_y);
        if (found && d >= best_d) {
            continue;
        }
        /* A seed is sized for the part that owned it. Putting a larger part
         * there pushes it over the board edge, and the clamp that follows
         * shoves it back into its neighbours - which is exactly the collision
         * this pass exists to remove. */
        if (!lns_slot_inside_board(s, comp, x, y) || !lns_slot_free(s, comp, x, y, gap)) {
            continue;
        }
        found = true;
        best_d = d;
        best_x = x;
        best_y = y;
    }
    if (!found) {
        return false;
    }
    *out_x = best_x;
    *out_y = best_y;
    return true;
}

/*
 * Where the designer had it.
 *
 * The optimiser only ever sees a cost, and on a board that is three quarters
 * locked it can walk a package into a pocket it cannot get out of - every
 * position the slot search reaches is taken, and the part ends up overlapping a
 * frozen neighbour. The seed position has one property no searched slot has: it
 * was legal when the board arrived. Restoring it is the honest fallback, and it
 * is what an ECO engineer would do by hand.
 */
bool lns_restore_to_seed(solver_t *s, uint32_t comp)
{
    const components_soa_t *c = &s->ctx->comps;
    coord_t nx = 0.0f;
    coord_t ny = 0.0f;
    if (lns_spiral_slot_from(s, comp, c->x[comp], c->y[comp], &nx, &ny)) {
        solver_nudge(s, comp, nx - s->x[comp], ny - s->y[comp]);
        solver_clamp_to_board(s, comp);
        return true;
    }
    return false;
}

/* Does this part - cluster included - still collide with something? */
bool lns_comp_collides(const solver_t *s, uint32_t comp, coord_t gap)
{
    coord_t lo_x = 0.0f;
    coord_t lo_y = 0.0f;
    coord_t hi_x = 0.0f;
    coord_t hi_y = 0.0f;
    solver_union_extent(s, comp, &lo_x, &lo_y, &hi_x, &hi_y);

    for (uint32_t j = 0u; j < s->ncomp; ++j) {
        if (j == comp || s->ruin_mask[j] != 0u) {
            continue;
        }
        if (shape_overlaps(s, comp, j, gap)) {
            return true;
        }
    }
    return false;
}

/*
 * Ruin and recreate (the CG:SHOP move): a part that the slot search cannot place
 * is not necessarily in an impossible spot - the pocket around it is simply
 * packed with parts that were themselves pushed there, and none of them can move
 * first. Lift the movable ones out, put the stuck part down, then reinsert the
 * rest around it, biggest first. A fresh arrangement comes out of a local
 * deadlock that no sequence of single pushes can break.
 *
 * Everything is restored if the pocket cannot be rebuilt, so a failed attempt
 * costs nothing but the time to try it.
 */
#define RUIN_MAX_POCKET 64u

bool lns_ruin_and_recreate(solver_t *s, uint32_t stuck, coord_t gap)
{
    uint32_t pocket[RUIN_MAX_POCKET];
    coord_t save_x[RUIN_MAX_POCKET];
    coord_t save_y[RUIN_MAX_POCKET];
    uint32_t count = 0u;

    const coord_t reach = 2.0f * (s->half_w[stuck] + s->half_h[stuck]) + 2.0f * gap + 1.0f;
    const coord_t sx = s->x[stuck];
    const coord_t sy = s->y[stuck];
    for (uint32_t j = 0u; j < s->ncomp && count < RUIN_MAX_POCKET; ++j) {
        if (j == stuck || !solver_component_is_movable(s, j)) {
            continue;
        }
        if (fabsf(s->x[j] - sx) > reach || fabsf(s->y[j] - sy) > reach) {
            continue;
        }
        pocket[count] = j;
        save_x[count] = s->x[j];
        save_y[count] = s->y[j];
        s->ruin_mask[j] = 1u;
        count += 1u;
    }

    coord_t nx = 0.0f;
    coord_t ny = 0.0f;
    const bool placed = lns_spiral_slot(s, stuck, &nx, &ny);
    if (placed) {
        solver_nudge(s, stuck, nx - s->x[stuck], ny - s->y[stuck]);
        solver_clamp_to_board(s, stuck);
    }

    /* put the pocket back, largest first, each one seeing only what is already
     * down: a passive waits for the package, never the other way round */
    bool rebuilt = placed;
    for (uint32_t step = 0u; step < count && rebuilt; ++step) {
        uint32_t pick = SOLVER_NO_INDEX;
        coord_t pick_area = -1.0f;
        for (uint32_t k = 0u; k < count; ++k) {
            const uint32_t j = pocket[k];
            if (s->ruin_mask[j] == 0u) {
                continue; /* already reinserted */
            }
            const coord_t area = s->half_w[j] * s->half_h[j];
            if (area > pick_area) {
                pick_area = area;
                pick = j;
            }
        }
        if (pick == SOLVER_NO_INDEX) {
            break;
        }
        s->ruin_mask[pick] = 0u;
        coord_t rx = 0.0f;
        coord_t ry = 0.0f;
        if (!lns_spiral_slot(s, pick, &rx, &ry)) {
            rebuilt = false;
            break;
        }
        solver_nudge(s, pick, rx - s->x[pick], ry - s->y[pick]);
        solver_clamp_to_board(s, pick);
    }

    if (!rebuilt) {
        for (uint32_t k = 0u; k < count; ++k) {
            const uint32_t j = pocket[k];
            s->ruin_mask[j] = 0u;
            solver_nudge(s, j, save_x[k] - s->x[j], save_y[k] - s->y[j]);
        }
        return false;
    }
    for (uint32_t k = 0u; k < count; ++k) {
        s->ruin_mask[pocket[k]] = 0u;
    }
    return true;
}

/*
 * Movable parts, largest courtyard first. Fixing the drivers, inductors and
 * packages while the board is still loose, then letting the 0402s flow into
 * what is left, converges where pushing everything at once oscillates: a
 * passive that cannot fit is a much smaller problem than an IC that cannot.
 */
void lns_order_by_area(const solver_t *s, uint32_t *order, uint32_t *count)
{
    uint32_t n = 0u;
    for (uint32_t i = 0u; i < s->ncomp; ++i) {
        if (solver_component_is_movable(s, i)) {
            order[n++] = i;
        }
    }
    for (uint32_t a = 1u; a < n; ++a) {
        const uint32_t comp = order[a];
        const coord_t area = s->half_w[comp] * s->half_h[comp];
        uint32_t b = a;
        while (b > 0u) {
            const uint32_t prev = order[b - 1u];
            if (s->half_w[prev] * s->half_h[prev] >= area) {
                break;
            }
            order[b] = prev;
            b -= 1u;
        }
        order[b] = comp;
    }
    *count = n;
}

