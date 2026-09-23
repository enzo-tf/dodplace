/*
 * swap_window.h - the deterministic permutation of a short alignment.
 *
 * A bucket is a set of parts the board cannot tell apart: same part, same
 * footprint, same face. Any permutation of their *poses* therefore leaves every
 * rectangle exactly where it was - the set of shapes on the board is unchanged,
 * so no courtyard, pad or mask aperture can move and the DRC cannot get worse.
 * What does change is which net hangs where, and that is wirelength.
 *
 * The annealer nudges this at random; this pass does it exhaustively over short
 * windows, which is the part a stochastic walk cannot do: for k parts there are
 * k! assignments, a number small enough to enumerate exactly and large enough
 * that a random walk never finds the best one in a 4000-move budget.
 */
#ifndef PLACE_SWAP_WINDOW_H
#define PLACE_SWAP_WINDOW_H

#include "solver_internal.h"

/* k! orderings per window: 6 is 720, still a few microseconds of netlist. */
#define SWAP_WINDOW_MAX 6u

/* Reorders windows of `k` interchangeable parts (2..6) until none improves the
 * wirelength, then keeps the result only if the engine's whole score improved
 * too. Returns whether the placement changed. */
bool swap_window_pass(solver_t *s, uint32_t k);

#endif /* PLACE_SWAP_WINDOW_H */
