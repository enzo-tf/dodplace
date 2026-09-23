/*
 * lns_repair.h - the slot searches the legaliser falls back on.
 *
 * Invariants every function here keeps:
 *   - the part is placed inside the board outline (margin included);
 *   - no other component's courtyard is closer than the DRC clearance;
 *   - a cluster moves as a rigid body, so its union box is what is tested;
 *   - a failed search changes nothing.
 * A caller may therefore treat a true return as "this part is legal now".
 */
#ifndef PLACE_LNS_REPAIR_H
#define PLACE_LNS_REPAIR_H

#include "solver_internal.h"

/* Exact clearance test: the part, cluster included, against every other. */
bool lns_slot_free(const solver_t *s, uint32_t comp, coord_t x, coord_t y, coord_t gap);

/* The nearest free slot to where the part is, on a lattice of its own size. */
bool lns_spiral_slot(solver_t *s, uint32_t comp, coord_t *out_x, coord_t *out_y);

/* The nearest free slot among the seed positions of the other movable parts. */
bool lns_seed_slot(solver_t *s, uint32_t comp, coord_t *out_x, coord_t *out_y);

/* Lift the movable parts around a stuck one, place it, put them back. */
bool lns_ruin_and_recreate(solver_t *s, uint32_t stuck, coord_t gap);

/* The part's own seed position, if the board still has room for it there. */
bool lns_restore_to_seed(solver_t *s, uint32_t comp);

/* Does the part - cluster included - still collide with anything? */
bool lns_comp_collides(const solver_t *s, uint32_t comp, coord_t gap);

/* Movable parts, largest courtyard first, into `order`; returns the count. */
void lns_order_by_area(const solver_t *s, uint32_t *order, uint32_t *count);

#endif /* PLACE_LNS_REPAIR_H */
