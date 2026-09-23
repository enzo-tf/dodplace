/*
 * shape.c - see shape.h.
 */
#include "shape.h"
#include "copper_test.h"

#include <math.h>

/* The copper box of a part, posed on the board: a quarter turn is exact. */
static void copper_box(const solver_t *s, uint32_t k, coord_t x, coord_t y, coord_t *lo_x,
                       coord_t *hi_x, coord_t *lo_y, coord_t *hi_y)
{
    coord_t cx = s->copper_cx[k];
    coord_t cy = s->copper_cy[k];
    coord_t hw = s->copper_hw[k];
    coord_t hh = s->copper_hh[k];
    const uint8_t o = solver_pose_orient(s, k);
    if (o == 1u) {
        const coord_t t = cx;
        cx = -cy;
        cy = t;
        const coord_t u = hw;
        hw = hh;
        hh = u;
    } else if (o == 2u) {
        cx = -cx;
        cy = -cy;
    } else if (o == 3u) {
        const coord_t t = cx;
        cx = cy;
        cy = -t;
        const coord_t u = hw;
        hw = hh;
        hh = u;
    }
    *lo_x = x + cx - hw;
    *hi_x = x + cx + hw;
    *lo_y = y + cy - hh;
    *hi_y = y + cy + hh;
}

static bool collide_at(const solver_t *s, uint32_t i, coord_t xi, coord_t yi, uint32_t j,
                       coord_t xj, coord_t yj, coord_t gap)
{
    /* One side at a time - the rule KiCad applies - with a drilled hole as the
     * only exception: a hole crosses every layer, plated or not, so U36 landing
     * on H3's mounting hole was a mask bridge, a hole-clearance error and an
     * edge-clearance error at once on r10. The filter is skipped when the scene
     * does not say which pads are holes, so an old scene cannot silently allow
     * a cross-side overlap. */
    if (s->sides_known && !same_side(s, i, j) && s->spans_sides[i] == 0u &&
        s->spans_sides[j] == 0u) {
        return false;
    }

    const coord_t dx = fabsf(xi - xj);
    const coord_t dy = fabsf(yi - yj);
    const coord_t touch_x = s->half_w[i] + s->half_w[j];
    const coord_t touch_y = s->half_h[i] + s->half_h[j];

    /* The courtyards first: the rule the DRC checks, and the cheap test. */
    if (dx < touch_x + gap && dy < touch_y + gap) {
        return true;
    }
    /* They are apart, but the pads can still meet in the gap between them -
     * and the reach must be measured on the METAL, not on the courtyard: a
     * footprint's pads overhang its courtyard, so two parts whose courtyards
     * are far apart can still touch. That was Q7 x R42 on r10: courtyards
     * apart, copper at 0.000 mm, and the copper test never ran. */
    const coord_t reach = s->opt->pad_clearance;
    coord_t a_lo = 0.0f;
    coord_t a_hi = 0.0f;
    coord_t a_lo_y = 0.0f;
    coord_t a_hi_y = 0.0f;
    coord_t b_lo = 0.0f;
    coord_t b_hi = 0.0f;
    coord_t b_lo_y = 0.0f;
    coord_t b_hi_y = 0.0f;
    copper_box(s, i, xi, yi, &a_lo, &a_hi, &a_lo_y, &a_hi_y);
    copper_box(s, j, xj, yj, &b_lo, &b_hi, &b_lo_y, &b_hi_y);
    if (a_lo - b_hi > reach || b_lo - a_hi > reach || a_lo_y - b_hi_y > reach ||
        b_lo_y - a_hi_y > reach) {
        return false; /* the metal is too far apart to matter */
    }
    return copper_overlaps(s, i, xi, yi, j, xj, yj, reach);
}

bool shape_overlaps(const solver_t *s, uint32_t i, uint32_t j, coord_t gap)
{
    return collide_at(s, i, s->x[i], s->y[i], j, s->x[j], s->y[j], gap);
}

bool shape_overlaps_posed(const solver_t *s, uint32_t i, coord_t xi, coord_t yi, uint32_t j,
                          coord_t gap)
{
    return collide_at(s, i, xi, yi, j, s->x[j], s->y[j], gap);
}
