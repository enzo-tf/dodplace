/*
 * solver_params.c - the option defaults and the random source.
 *
 * The generator is xorshift32 so a seed reproduces a layout exactly; nothing
 * else in the solver reads a clock or the environment.
 */
#include "solver_internal.h"

#include <string.h>

void solver_options_defaults(solver_options_t *opt)
{
    if (opt == nullptr) {
        return;
    }
    memset(opt, 0, sizeof *opt);
    opt->global_iterations = 400u;
    /* The ePlace density model is the default: on r10 it produces the same
     * placement as the pairwise repulsion, byte for byte, for 0.10 s instead of
     * 0.29 s - and unlike the pairwise sum it does not get slower as the square
     * of the part count. `--global-model repulsion` keeps the old model, which
     * is what the calibrations in docs/calibration were measured with. */
    opt->global_model = GLOBAL_MODEL_ANALYTIC;
    opt->w_density = 1.0f;
    opt->momentum = 0.9f;
    opt->density_bins = 64u;
    /* The execution budget is the machine's, not a tenth of a second: sixteen
     * walks of thirty-two thousand moves each (about a minute on r10) beat a
     * single walk of four thousand by 1 700 mm, and the wall clock is not a
     * contract any more. `--moves` and `--restarts` dial it back for a
     * developer loop or a plugin run. */
    opt->refine_moves = 64000u;
    opt->refine_restarts = 32u;
    /* Four walks at once is safe on any laptop; the calibration machine runs
     * eight and finishes the whole search in forty-five seconds. */
    opt->jobs = 4u;
    /* Relative to the score, because the cost mixes millimetres and counts. */
    opt->anneal_t_start_ratio = 0.01f;
    opt->anneal_t_end_ratio = 1e-3f;
    opt->legalize_passes = 80u;
    opt->legalize_rounds = 12u;

    opt->w_hpwl = 1.0f;
    opt->w_crossings = 5.0f;
    opt->w_overlaps = 4.0f;
    opt->w_overlap_depth = 2.0f;
    opt->w_displacement = 0.0f; /* the anchor as a soft recall */
    opt->w_thermal = 3.0f;
    /* Copper-to-copper margin, calibrated against KiCad's DRC rather than
     * derived from netclass arithmetic. Zero means "the metal must not touch",
     * which is what its shorting_items rule reports; anything positive is
     * conservative, because the boxes over-estimate rounded pads - r10's
     * original layout has pads whose boxes touch and whose metal does not. */
    /*
     * A rail is not a trace: it dives to an inner plane through a local via,
     * and its wirelength is not a placement objective. The rule is right, and
     * the filter is implemented - but measured on r10 it costs 170 mm of
     * SIGNAL wirelength (33 898 -> 34 068, same nets on both sides) and takes
     * KiCad's shorts from 1 to 9: the rails were acting as a spread in the
     * objective, and removing them lets the signals clump. Off by default
     * until the weights are re-tuned around their absence.
     */
    opt->max_net_degree = 15u;
    opt->exclude_plane_nets = false;
    opt->w_decoupling = 1.0f;
    opt->w_diffpair = 4.0f;
    /* Part swapping: exchanging two identical parts cannot move a rectangle,
     * so it is the one move that changes the netlist assignment at no
     * geometric risk. Off for the 24-pin packages until the LED signal is
     * measured on its own. */
    opt->swap_prob = 0.15f;
    opt->swap_max_pins = 24u;
    /* Short alignments of interchangeable parts are permuted exactly, after the
     * annealer: k! is small enough to enumerate and too large for a random walk
     * to find inside a 4000-move budget. */
    opt->swap_window = 5u;
    /* Buckets smaller than this are left to the local windows: the Hungarian
     * solve costs n^3 and a two- or three-part bucket is what the windows are
     * best at anyway. */
    opt->swap_assign_min = 6u;
    /* The star model prices a pin by its distance to the centroid of the rest
     * of its net, which misreads the edges of a bounding box; the engine is
     * judged on bounding boxes. The weighted average is the differentiable
     * estimate the literature uses for exactly this reason. */
    opt->assign_model = ASSIGN_MODEL_WA;
    opt->wa_gamma = 1.0f;
    opt->pad_clearance = 0.5f;
    /* The mask rule sits a little outside the copper margin; 0.20 covers the
     * r10 pairs (0.646 and 0.650 mm) with margin to spare. */
    opt->polish_reach = 0.20f;
    opt->max_slip_x = 0.0f; /* derived from the board's own pitch, see solver_state */
    opt->max_slip_y = 0.0f;
    /* Two and a half lanes for a part that arrives already overlapping: enough
     * to clear what it landed on, little enough that it stays in its
     * neighbourhood. The old emergency scale of five let parts jump clean out
     * of their rail. */
    opt->anchor_soft_scale = 2.5f;
    opt->thermal_min_power = 0.3f;

    opt->attraction = 1.0f;
    opt->repulsion = 0.0f; /* derived from the board area */
    opt->net_weight_scale = 1.0f;

    opt->rotation_mask = 0x0Fu; /* all four orientations */
    opt->allow_side_flip = false;
    opt->respect_rot_fixed = true;

    /* Rigid fusion is off until the discrete assignment stage replaces it:
     * welding decoupling pairs into super-nodes took the legaliser's freedom
     * away and left 28 collisions on r10 where free passives left 4. */
    opt->enable_clustering = false;
    /* Implemented and tested, but off: on r9 and r10 the rule-driven
     * assignment pulled each capacitor towards its decoupling pin and away
     * from its ground and supply nets, and the wirelength came out 1-6 %
     * worse than the continuous stages alone. Enable it when a board has
     * decoupling rules that are the dominant constraint. */
    /* On by default: the assignment only pays once the overlap count is exact
     * (it was measured against an inflated one for a while), and with that
     * fixed it is worth a few tens of millimetres on both reference boards at
     * no cost in collisions. */
    opt->enable_matching = true;
    opt->enable_global = true;
    opt->enable_refine = true;
    opt->enable_legalize = true;

    opt->board_margin = 0.5f;
    opt->courtyard_clearance = 0.2f;
    opt->crossing_cell = 4.0f;
    opt->seed = 1u;
}

/* ========================================================================= */
/* Randomness: xorshift32, so a seed reproduces a layout exactly              */
/* ========================================================================= */

static uint32_t rng_next(solver_t *s)
{
    uint32_t v = s->rng;
    v ^= v << 13;
    v ^= v >> 17;
    v ^= v << 5;
    s->rng = v;
    return v;
}

coord_t solver_rand_unit(solver_t *s)
{
    return (coord_t)(rng_next(s) >> 8) * (1.0f / 16777216.0f);
}

uint32_t solver_rand_below(solver_t *s, uint32_t bound)
{
    return bound == 0u ? 0u : rng_next(s) % bound;
}

/* ========================================================================= */
/* Arena helpers                                                             */
/* ========================================================================= */

/* ========================================================================= */
/* Geometry                                                                  */
/* ========================================================================= */

/*
 * Courtyard extents in board space. The stored half extents are in the
 * footprint frame, so a part at 90 or 270 degrees occupies the swapped
 * rectangle: this is the "oriented box" of the SAT stage, reduced to what a
 * quarter-turn grid can actually produce.
 */
