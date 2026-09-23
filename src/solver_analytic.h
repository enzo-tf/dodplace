/*
 * solver_analytic.h - the analytical global placer (ePlace's density model).
 *
 * Attraction is the star model the engine already had: every pin is pulled to
 * the centroid of its net, which is a differentiable stand-in for HPWL and
 * makes each iteration a gradient step. The difference is the *repulsion*: a
 * Poisson field solved on a uniform bin grid, sampled at each part's centre and
 * weighted by the part's own area, instead of a sum over every pair.
 *
 * Each iteration is one Nesterov-style step: the forces are evaluated at a
 * look-ahead point and the step keeps momentum, with the step length normalised
 * by the largest force so the walk cannot jump the board. Nothing here reads a
 * clock or a random number: the same scene and the same seed give the same
 * placement, to the bit.
 */
#ifndef PLACE_SOLVER_ANALYTIC_H
#define PLACE_SOLVER_ANALYTIC_H

#include "solver_internal.h"

/* Runs `global_iterations` steps in place. Returns s->ok. */
bool solver_global_analytic(solver_t *s);

#endif /* PLACE_SOLVER_ANALYTIC_H */
