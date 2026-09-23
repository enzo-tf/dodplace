/*
 * solver_union.c - the box a part occupies, cluster included.
 *
 * A cluster moves as one rigid body, so any test of "does it fit here" has to
 * use the union of its members' boxes; the master's own box says nothing about
 * its satellites.
 */
#include "solver_internal.h"

void solver_union_extent(const solver_t *s, uint32_t comp, coord_t *lo_x, coord_t *lo_y,
                         coord_t *hi_x, coord_t *hi_y)
{
    *lo_x = -s->half_w[comp];
    *hi_x = s->half_w[comp];
    *lo_y = -s->half_h[comp];
    *hi_y = s->half_h[comp];
    const uint32_t cl = s->cluster_of[comp];
    if (cl == SOLVER_NO_INDEX) {
        return;
    }
    for (uint32_t k = s->cluster_first[cl];
         k < s->cluster_first[cl] + s->cluster_count[cl]; ++k) {
        const uint32_t m = s->member_comp[k];
        const coord_t dx = s->x[m] - s->x[comp];
        const coord_t dy = s->y[m] - s->y[comp];
        const coord_t lo = dx - s->half_w[m];
        const coord_t hi = dx + s->half_w[m];
        *lo_x = (lo < *lo_x) ? lo : *lo_x;
        *hi_x = (hi > *hi_x) ? hi : *hi_x;
        const coord_t lo_y_m = dy - s->half_h[m];
        const coord_t hi_y_m = dy + s->half_h[m];
        *lo_y = (lo_y_m < *lo_y) ? lo_y_m : *lo_y;
        *hi_y = (hi_y_m > *hi_y) ? hi_y_m : *hi_y;
    }
}

uint32_t solver_collect_overlap_pairs(solver_t *s, uint32_t *out_i, uint32_t *out_j,
                                      uint32_t max)
{
    return solver_collect_conflicts(s, s->opt->courtyard_clearance, out_i, out_j, max);
}

/*
 * Pairs closer than `gap` between their courtyard edges. `gap` is the courtyard
 * clearance for the cost and for legalisation (a legal layout respects DRC), and
 * zero for the "true overlap" count reported to the user: on a dense LED array
 * two courtyards 0.1 mm apart are a clearance issue, not a collision, and
 * conflating them would report a designer's good layout as broken.
 */
