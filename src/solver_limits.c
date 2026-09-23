/*
 * solver_limits.c - the moves the solver may make and the walls it may not
 * cross: nudging a part (or the cluster it masters), refusing a rotation that
 * could not be legalised, and confining a part to the board.
 */
#include "solver_internal.h"

#include <math.h>

void solver_nudge(solver_t *s, uint32_t comp, coord_t dx, coord_t dy)
{
    const uint32_t cl = s->cluster_of[comp];
    if (cl != SOLVER_NO_INDEX) {
        const uint32_t master = s->cluster_master[cl];
        solver_apply_cluster(s, cl, s->x[master] + dx, s->y[master] + dy);
    } else {
        s->x[comp] += dx;
        s->y[comp] += dy;
    }
}

/*
 * Would the part - or the cluster it masters - still fit inside the board if it
 * were rotated to `orient`? A block that is longer than the board is tall can be
 * turned into a pose no position can legalise, so the rotation must be refused
 * before it is taken, not repaired after.
 */
bool solver_rotation_fits(const solver_t *s, uint32_t comp, uint8_t orient)
{
    const uint32_t cl = s->cluster_of[comp];
    const uint32_t master = (cl == SOLVER_NO_INDEX) ? comp : s->cluster_master[cl];
    if (master != comp) {
        return true; /* a satellite follows: the master's check covers it */
    }
    const coord_t board_w = s->board_max_x - s->board_min_x;
    const coord_t board_h = s->board_max_y - s->board_min_y;
    if (board_w <= 0.0f || board_h <= 0.0f) {
        return true; /* no outline to violate */
    }

    const components_soa_t *src = &s->ctx->comps;
    const bool odd = (orient & 1u) != 0u;
    coord_t hw = odd ? src->half_h[comp] : src->half_w[comp];
    coord_t hh = odd ? src->half_w[comp] : src->half_h[comp];
    coord_t lo_x = s->x[comp] - hw;
    coord_t hi_x = s->x[comp] + hw;
    coord_t lo_y = s->y[comp] - hh;
    coord_t hi_y = s->y[comp] + hh;
    if (cl != SOLVER_NO_INDEX) {
        for (uint32_t k = s->cluster_first[cl];
             k < s->cluster_first[cl] + s->cluster_count[cl]; ++k) {
            const uint32_t m = s->member_comp[k];
            const uint8_t mo = (uint8_t)((orient + s->member_dorient[k]) & 3u);
            const bool m_odd = (mo & 1u) != 0u;
            const coord_t mw = m_odd ? src->half_h[m] : src->half_w[m];
            const coord_t mh = m_odd ? src->half_w[m] : src->half_h[m];
            /* the member's position follows the candidate master pose */
            const uint8_t turn = (uint8_t)((orient - s->orient[comp]) & 3u);
            coord_t dx = s->member_dx[k];
            coord_t dy = s->member_dy[k];
            coord_t rx = dx;
            coord_t ry = dy;
            if (turn == 1u) {
                rx = -dy;
                ry = dx;
            } else if (turn == 2u) {
                rx = -dx;
                ry = -dy;
            } else if (turn == 3u) {
                rx = dy;
                ry = -dx;
            }
            const coord_t mx = s->x[comp] + rx;
            const coord_t my = s->y[comp] + ry;
            lo_x = (mx - mw < lo_x) ? mx - mw : lo_x;
            hi_x = (mx + mw > hi_x) ? mx + mw : hi_x;
            lo_y = (my - mh < lo_y) ? my - mh : lo_y;
            hi_y = (my + mh > hi_y) ? my + mh : hi_y;
        }
    }
    return (hi_x - lo_x) <= board_w && (hi_y - lo_y) <= board_h;
}

/*
 * Clamps a component - or the whole cluster it belongs to - inside the board.
 * The union of the cluster's boxes is what must fit: clamping the master alone
 * would let a satellite stick out over the edge.
 */
/* Is (x, y) inside the part's anchor box? The master answers for its cluster. */
bool solver_within_anchor(const solver_t *s, uint32_t comp, coord_t x, coord_t y)
{
    if (s->anchor_x == nullptr || s->anchor_y == nullptr) {
        return true; /* no anchor recorded: the board is the only bound */
    }
    const uint32_t cl = s->cluster_of[comp];
    const uint32_t target = (cl == SOLVER_NO_INDEX) ? comp : s->cluster_master[cl];
    const coord_t scale = (s->anchor_soft != nullptr && s->anchor_soft[target] != 0u)
                              ? s->opt->anchor_soft_scale
                              : 1.0f;
    const coord_t slip_x = s->slip_x * scale;
    const coord_t slip_y = s->slip_y * scale;
    const coord_t dx = x - s->anchor_x[target];
    const coord_t dy = y - s->anchor_y[target];
    return (dx >= -slip_x) && (dx <= slip_x) && (dy >= -slip_y) && (dy <= slip_y);
}

/* Pulls the part back into its anchor box; a cluster moves with its master. */
void solver_clamp_to_anchor(solver_t *s, uint32_t comp)
{
    if (s->anchor_x == nullptr || s->anchor_y == nullptr) {
        return;
    }
    const uint32_t cl = s->cluster_of[comp];
    const uint32_t target = (cl == SOLVER_NO_INDEX) ? comp : s->cluster_master[cl];
    const coord_t scale = (s->anchor_soft != nullptr && s->anchor_soft[target] != 0u)
                              ? s->opt->anchor_soft_scale
                              : 1.0f;
    const coord_t slip_x = s->slip_x * scale;
    const coord_t slip_y = s->slip_y * scale;
    const coord_t dx = s->x[target] - s->anchor_x[target];
    const coord_t dy = s->y[target] - s->anchor_y[target];
    coord_t cx = s->x[target];
    coord_t cy = s->y[target];
    if (dx < -slip_x) {
        cx = s->anchor_x[target] - slip_x;
    } else if (dx > slip_x) {
        cx = s->anchor_x[target] + slip_x;
    }
    if (dy < -slip_y) {
        cy = s->anchor_y[target] - slip_y;
    } else if (dy > slip_y) {
        cy = s->anchor_y[target] + slip_y;
    }
    if (cx == s->x[target] && cy == s->y[target]) {
        return;
    }
    if (cl != SOLVER_NO_INDEX) {
        solver_apply_cluster(s, cl, cx, cy);
    } else {
        s->x[target] = cx;
        s->y[target] = cy;
    }
}

void solver_clamp_to_board(solver_t *s, uint32_t comp)
{
    /* The anchor is the tighter of the two bounds, so it is applied last: a
     * part that the board would allow but its lane does not is pulled back. */
    solver_clamp_to_anchor(s, comp);
    const uint32_t cl = s->cluster_of[comp];
    const uint32_t target = (cl == SOLVER_NO_INDEX) ? comp : s->cluster_master[cl];

    coord_t lo_x = 0.0f;
    coord_t hi_x = 0.0f;
    coord_t lo_y = 0.0f;
    coord_t hi_y = 0.0f;
    solver_copper_box(s, target, &lo_x, &hi_x, &lo_y, &hi_y);
    if (cl != SOLVER_NO_INDEX) {
        for (uint32_t k = s->cluster_first[cl];
             k < s->cluster_first[cl] + s->cluster_count[cl]; ++k) {
            const uint32_t m = s->member_comp[k];
            coord_t m_lo_x = 0.0f;
            coord_t m_hi_x = 0.0f;
            coord_t m_lo_y = 0.0f;
            coord_t m_hi_y = 0.0f;
            solver_copper_box(s, m, &m_lo_x, &m_hi_x, &m_lo_y, &m_hi_y);
            lo_x = (m_lo_x < lo_x) ? m_lo_x : lo_x;
            hi_x = (m_hi_x > hi_x) ? m_hi_x : hi_x;
            lo_y = (m_lo_y < lo_y) ? m_lo_y : lo_y;
            hi_y = (m_hi_y > hi_y) ? m_hi_y : hi_y;
        }
    }

    const coord_t span_x = hi_x - lo_x;
    const coord_t span_y = hi_y - lo_y;
    const coord_t room_x = s->board_max_x - s->board_min_x;
    const coord_t room_y = s->board_max_y - s->board_min_y;
    coord_t shift_x = 0.0f;
    coord_t shift_y = 0.0f;
    if (span_x > room_x) {
        shift_x = (s->board_min_x + s->board_max_x) * 0.5f - (lo_x + hi_x) * 0.5f;
    } else if (lo_x < s->board_min_x) {
        shift_x = s->board_min_x - lo_x;
    } else if (hi_x > s->board_max_x) {
        shift_x = s->board_max_x - hi_x;
    }
    if (span_y > room_y) {
        shift_y = (s->board_min_y + s->board_max_y) * 0.5f - (lo_y + hi_y) * 0.5f;
    } else if (lo_y < s->board_min_y) {
        shift_y = s->board_min_y - lo_y;
    } else if (hi_y > s->board_max_y) {
        shift_y = s->board_max_y - hi_y;
    }
    if (shift_x == 0.0f && shift_y == 0.0f) {
        return;
    }
    if (cl != SOLVER_NO_INDEX) {
        solver_apply_cluster(s, cl, s->x[target] + shift_x, s->y[target] + shift_y);
    } else {
        s->x[target] += shift_x;
        s->y[target] += shift_y;
    }
}

/* ========================================================================= */
/* Cost                                                                      */
/* ========================================================================= */
