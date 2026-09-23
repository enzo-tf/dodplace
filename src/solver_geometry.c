/*
 * solver_geometry.c - the working placement and the rigid bodies in it.
 *
 * The scene is never modified: every position lives in the solver's own tables.
 * A cluster moves as one body through solver_apply_cluster, which re-derives
 * each satellite from the master's pose so the block can never come apart.
 */
#include "solver_internal.h"

#include <math.h>

void solver_rebuild_extents(solver_t *s)
{
    const components_soa_t *c = &s->ctx->comps;
    for (uint32_t i = 0u; i < c->count; ++i) {
        const bool odd = (s->orient[i] & 1u) != 0u;
        const coord_t hw = odd ? c->half_h[i] : c->half_w[i];
        const coord_t hh = odd ? c->half_w[i] : c->half_h[i];
        s->half_w[i] = hw;
        s->half_h[i] = hh;
    }
}

void solver_set_position(solver_t *s, uint32_t comp, coord_t px, coord_t py)
{
    s->x[comp] = px;
    s->y[comp] = py;
}

uint32_t solver_component_cluster(const solver_t *s, uint32_t comp)
{
    return s->cluster_of[comp];
}

bool solver_component_is_movable(const solver_t *s, uint32_t comp)
{
    if ((s->ctx->comps.flags[comp] & COMP_LOCKED) != 0u) {
        return false;
    }
    if (s->frozen != nullptr && s->frozen[comp] != 0u) {
        return false; /* taller than the ceiling: nowhere to put it */
    }
    const uint32_t cluster = s->cluster_of[comp];
    if (cluster != SOLVER_NO_INDEX && s->cluster_master[cluster] != comp) {
        return false; /* it follows its master */
    }
    return true;
}

/* Moves a cluster as a rigid body: the master here, the members relative. */
void solver_apply_cluster(solver_t *s, uint32_t cluster, coord_t mx, coord_t my)
{
    if (cluster == SOLVER_NO_INDEX) {
        return;
    }
    const uint32_t master = s->cluster_master[cluster];
    s->x[master] = mx;
    s->y[master] = my;
    const uint8_t master_orient = s->orient[master];
    /* R(master orientation) applied to the stored offset */
    const coord_t c = (master_orient == 0u) ? 1.0f : (master_orient == 2u ? -1.0f : 0.0f);
    const coord_t sn = (master_orient == 1u) ? 1.0f : (master_orient == 3u ? -1.0f : 0.0f);
    for (uint32_t k = s->cluster_first[cluster];
         k < s->cluster_first[cluster] + s->cluster_count[cluster]; ++k) {
        const coord_t dx = s->member_dx[k];
        const coord_t dy = s->member_dy[k];
        const uint32_t m = s->member_comp[k];
        s->x[m] = mx + c * dx - sn * dy;
        s->y[m] = my + sn * dx + c * dy;
        /* the block turns as one body: a satellite keeps its pose relative to
         * the master, or a rotated IC would leave its capacitors sideways */
        s->orient[m] = (uint8_t)((master_orient + s->member_dorient[k]) & 3u);
        /* inline of solver_rebuild_extents for one part: a quarter turn swaps
         * the extents, and doing it here avoids an O(n) rebuild per member */
        const components_soa_t *src = &s->ctx->comps;
        const bool odd = (s->orient[m] & 1u) != 0u;
        s->half_w[m] = odd ? src->half_h[m] : src->half_w[m];
        s->half_h[m] = odd ? src->half_w[m] : src->half_h[m];
    }
}

void solver_sync_cluster_members(solver_t *s)
{
    for (uint32_t cl = 0u; cl < s->nclusters; ++cl) {
        const uint32_t master = s->cluster_master[cl];
        solver_apply_cluster(s, cl, s->x[master], s->y[master]);
    }
}

/* ========================================================================= */
/* Wirelength                                                                */
/* ========================================================================= */

