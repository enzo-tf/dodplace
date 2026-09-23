/*
 * swap.h - exchanging two identical parts.
 *
 * The annealer translates and rotates; neither can change WHICH net a pad
 * carries, and on a regular array - 480 LEDs on r10 - that ordering is what
 * fixes the routing. Two parts of the same footprint and the same face are
 * mechanically interchangeable: exchanging their poses leaves every rectangle
 * on the board exactly where it was, so the DRC cannot get worse, while the
 * netlist sees a different assignment.
 *
 * The swap is proposed here and judged by the annealer, which owns the only
 * acceptance path (score, Metropolis, restore).
 */
#ifndef PLACE_SWAP_H
#define PLACE_SWAP_H

#include "solver_internal.h"

/* One candidate exchange, with the poses to put back if it is rejected. */
typedef struct {
    uint32_t a, b;
    coord_t  ax, ay, bx, by;
    uint8_t  a_orient, b_orient;
} swap_move_t;

/* Builds the buckets of interchangeable parts. Once, at init. */
bool swap_buckets_build(solver_t *s);

/*
 * Picks two parts from one bucket and exchanges their poses. False when the
 * exchange is not safe (local copper conflict) or when nothing can be swapped;
 * in that case the placement is untouched and `move` must not be restored.
 */
bool swap_propose(solver_t *s, swap_move_t *move);

/* Puts both parts back. */
void swap_undo(solver_t *s, const swap_move_t *move);

/* True when the two parts are instances of the same part: same identity, same
 * footprint, same face, same rotation freedom. Nothing about them can be told
 * apart on the board, so exchanging their poses moves no rectangle. */
static inline bool swap_same_bucket(const solver_t *s, uint32_t a, uint32_t b)
{
    return s->swap_bucket_of != nullptr && s->swap_bucket_of[a] != SOLVER_NO_INDEX &&
           s->swap_bucket_of[a] == s->swap_bucket_of[b];
}

#endif /* PLACE_SWAP_H */
