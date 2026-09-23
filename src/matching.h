/*
 * matching.h - discrete assignment of decoupling passives to free slots.
 *
 * The passives of a decoupling rule are not placed by force: each one is
 * assigned to a slot, one passive per slot, by a linear sum assignment solved
 * with an auction. The slots come from the occupancy raster, so a slot is free
 * of every locked part and every keepout by construction, and an assignment
 * therefore cannot create a collision with the frozen layout.
 *
 * Invariants:
 *   - a passive is only ever assigned to a slot whose footprint the raster
 *     proves clear for its own size (raster_mask_box_clear);
 *   - slots are exclusive: two passives never share one;
 *   - the assignment never moves a part more than `max_dist_sq` from the pin
 *     the rule names, which is what keeps the result compact;
 *   - a passive left unassigned keeps its position; the caller decides what to
 *     do about it (legalisation will).
 */
#ifndef PLACE_MATCHING_H
#define PLACE_MATCHING_H

#include "solver_internal.h"

/* One assignment: this passive goes to (x, y). */
typedef struct {
    uint32_t comp;
    coord_t  x;
    coord_t  y;
} match_slot_t;

/*
 * Assigns the movable passives named by the decoupling table to free slots near
 * their IC pin, moves them there, and returns how many were placed.
 */
uint32_t matching_assign_decoupling(solver_t *s);

#endif /* PLACE_MATCHING_H */
