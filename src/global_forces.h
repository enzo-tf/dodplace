/*
 * global_forces.h - the pieces every global model shares.
 *
 * The two global models disagree about one thing: how parts push each other
 * apart. They agree about everything else - the netlist pulls its pins to a
 * centroid, a part no net reaches drifts to the middle, a rigid body takes the
 * sum of its members' forces, and nothing leaves the board - so those live here
 * and are written once.
 */
#ifndef PLACE_GLOBAL_FORCES_H
#define PLACE_GLOBAL_FORCES_H

#include "solver_internal.h"

/* `fx`, `fy` and `touched` are zeroed here; one entry per component. */
void global_reset_forces(solver_t *s, coord_t *fx, coord_t *fy, uint8_t *touched);

/* Every pin is pulled towards the centroid of its net: the star model, and a
 * differentiable wirelength - the gradient of a sum of squared pin-to-centroid
 * distances. */
void global_attraction(solver_t *s, coord_t *fx, coord_t *fy, uint8_t *touched);

/* A weak pull to the board centre for parts the netlist does not reach. */
void global_centring(solver_t *s, coord_t *fx, coord_t *fy, const uint8_t *touched);

/* A rigid body moves as one: its members' forces add to the master's. */
void global_cluster_forces(solver_t *s, coord_t *fx, coord_t *fy);

/* The largest force over the movable set, for the step's normalisation. */
coord_t global_max_force(solver_t *s, const coord_t *fx, const coord_t *fy);

/* Confine every movable part - or cluster master - to the board. */
void global_confine(solver_t *s);

#endif /* PLACE_GLOBAL_FORCES_H */
