/*
 * polish.h - the last few microns.
 *
 * The legaliser works at the courtyard clearance and the copper margin; KiCad
 * applies a mask rule on top, and a pair can satisfy ours and still have its
 * mask openings touch - four solder_mask_bridge on r10, all of them one pair
 * whose copper boxes were 0.646 mm apart and needed four microns more.
 *
 * This pass looks for pairs closer than the polish margin, tries a fixed list
 * of small offsets on the movable member, and takes the first that clears the
 * margin without creating anything anywhere else. It never makes a layout
 * worse: a pair that cannot be cleared is left alone.
 */
#ifndef PLACE_POLISH_H
#define PLACE_POLISH_H

#include "solver_internal.h"

/* Moves what needs moving by a few microns so the mask rule is met. */
void solver_polish(solver_t *s);

#endif /* PLACE_POLISH_H */
