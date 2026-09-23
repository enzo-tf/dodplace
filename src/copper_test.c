/*
 * copper_test.c - see copper_test.h.
 */
#include "copper_test.h"

#include <math.h>

/* One pad's box in the board frame: the offset and the half extents both turn
 * with the component, and a quarter turn swaps the extents exactly. */
static void pad_box(const solver_t *s, const pins_soa_t *pins, uint32_t pin, coord_t cx,
                    coord_t cy, uint8_t orient, coord_t margin, coord_t *lo_x, coord_t *hi_x,
                    coord_t *lo_y, coord_t *hi_y)
{
    coord_t dx = pins->offset_x[pin];
    coord_t dy = pins->offset_y[pin];
    coord_t hw = (pins->half_x != nullptr) ? pins->half_x[pin] : 0.0f;
    coord_t hh = (pins->half_y != nullptr) ? pins->half_y[pin] : 0.0f;
    if (orient == 1u) {
        const coord_t t = dx;
        dx = -dy;
        dy = t;
        const coord_t u = hw;
        hw = hh;
        hh = u;
    } else if (orient == 2u) {
        dx = -dx;
        dy = -dy;
    } else if (orient == 3u) {
        const coord_t t = dx;
        dx = dy;
        dy = -t;
        const coord_t u = hw;
        hw = hh;
        hh = u;
    }
    /*
     * The pad is tested as its bounding box, not as its roundrect shape.
     * Inscribing the shape (shrinking each side by the corner radius, about a
     * quarter of the smaller side) was tried and measured: it buys 405 mm of
     * wirelength (36 872 -> 36 467) and costs four shorts (KiCad's
     * shorting_items 1 -> 5, bridges 3 -> 6), at every clearance from 0.25 to
     * 0.50, because the shrink and the clearance compensate each other. The
     * copper is the one metric where conservative wins.
     */
    const coord_t half = margin * 0.5f;
    *lo_x = cx + dx - hw - half;
    *hi_x = cx + dx + hw + half;
    *lo_y = cy + dy - hh - half;
    *hi_y = cy + dy + hh + half;
    (void)s;
}

bool copper_overlaps(const solver_t *s, uint32_t i, coord_t xi, coord_t yi, uint32_t j,
                     coord_t xj, coord_t yj, coord_t margin)
{
    const pins_soa_t *pins = &s->ctx->pins;
    const components_soa_t *c = &s->ctx->comps;
    if (pins->half_x == nullptr) {
        return false; /* the scene does not carry pad copper */
    }
    const uint32_t i_begin = c->first_pin[i];
    const uint32_t i_end = i_begin + (uint32_t)c->pin_count[i];
    const uint32_t j_begin = c->first_pin[j];
    const uint32_t j_end = j_begin + (uint32_t)c->pin_count[j];

    for (uint32_t p = i_begin; p < i_end; ++p) {
        coord_t a_lo_x = 0.0f;
        coord_t a_hi_x = 0.0f;
        coord_t a_lo_y = 0.0f;
        coord_t a_hi_y = 0.0f;
        pad_box(s, pins, p, xi, yi, solver_pose_orient(s, i), margin, &a_lo_x, &a_hi_x,
                &a_lo_y, &a_hi_y);
        for (uint32_t q = j_begin; q < j_end; ++q) {
            coord_t b_lo_x = 0.0f;
            coord_t b_hi_x = 0.0f;
            coord_t b_lo_y = 0.0f;
            coord_t b_hi_y = 0.0f;
            pad_box(s, pins, q, xj, yj, solver_pose_orient(s, j), margin, &b_lo_x, &b_hi_x,
                    &b_lo_y, &b_hi_y);
            if (a_lo_x < b_hi_x && b_lo_x < a_hi_x && a_lo_y < b_hi_y && b_lo_y < a_hi_y) {
                return true;
            }
        }
    }
    return false;
}
