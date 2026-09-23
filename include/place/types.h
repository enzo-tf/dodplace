/*
 * types.h - dense Structure-of-Arrays data model for the dodplace engine.
 *
 * Everything here is meant to be read inside the placement solver's hot loops,
 * therefore:
 *   - no struct-of-pointers-per-object: one flat array per attribute;
 *   - ids, not pointers, for cross references (survives arena relocation and
 *     keeps the data relocatable/serialisable);
 *   - per-component branch data collapsed into a single flags byte;
 *   - optional enrichment lives in *sparse* tables (see constraints.h) so a
 *     plain passive never pays a byte for thermal/decoupling metadata.
 *
 * Coordinate frame: millimetres, origin = board origin, X right, Y down
 * (KiCad's PCB editor convention). Pin offsets are stored *unrotated*, in the
 * footprint frame, relative to the component centre; the solver applies the
 * component's discrete orientation when it needs absolute pin positions.
 */
#ifndef PLACE_TYPES_H
#define PLACE_TYPES_H

#include <stdbool.h>
#include <stdint.h>

/* f32 millimetres: a PCB spans < 1 m, so f32 keeps ~0.06 um resolution at
 * 500 mm while doubling SIMD lane count and cache density versus f64. */
typedef float coord_t;

/* Sparse index into any SoA table. PLACE_ID_NONE marks "no such element". */
typedef uint32_t place_id_t;
#define PLACE_ID_NONE UINT32_MAX

/* Per-component pin counts fit in 16 bits (no package has 65k pins). */
typedef uint16_t place_count_t;

static_assert(sizeof(coord_t) == 4, "coord_t is the hot-loop scalar: keep it 32-bit");
static_assert(sizeof(place_id_t) == 4, "place_id_t must stay 32-bit");

/* ------------------------------------------------------------------------- */
/* Components                                                                 */
/* ------------------------------------------------------------------------- */

/*
 * comp_flags packs every per-component branch the solver needs into one byte
 * (this is the only per-component word the solver reads to decide behaviour):
 *
 *   [0:1] orientation index 0..3  ->  0 / 90 / 180 / 270 degrees CCW
 *   [2]   side: 0 = top, 1 = bottom
 *   [3]   locked: position frozen by the user
 *   [4]   rotation fixed: orientation frozen, position free
 *   [5]   courtyard is an explicit polygon (polygon_id is valid)
 *   [6]   height known (3D / z-interference checks enabled for this part)
 *   [7]   mass known (centre of gravity / inertia available)
 */
typedef uint8_t comp_flags_t;

enum : comp_flags_t {
    COMP_ORIENT_MASK    = 0x03u,
    COMP_SIDE_BOTTOM    = 0x04u,
    COMP_LOCKED         = 0x08u,
    COMP_ROT_FIXED      = 0x10u,
    COMP_POLY_COURTYARD = 0x20u,
    COMP_HAS_HEIGHT     = 0x40u,
    COMP_HAS_MASS       = 0x80u
};

static_assert(sizeof(comp_flags_t) == 1, "packed component flags must stay 1 byte");

/* Discrete orientation, index == orientation / 90 degrees. */
static inline unsigned comp_orientation(comp_flags_t flags)
{
    return (unsigned)(flags & COMP_ORIENT_MASK);
}

static inline coord_t comp_orientation_deg(comp_flags_t flags)
{
    return (coord_t)(90 * comp_orientation(flags));
}

static inline bool comp_on_bottom(comp_flags_t flags)
{
    return (flags & COMP_SIDE_BOTTOM) != 0u;
}

static inline bool comp_is_locked(comp_flags_t flags)
{
    return (flags & COMP_LOCKED) != 0u;
}

/*
 * Component kind, derived by the producer from the reference designator prefix
 * (U, C, L, Y...). The engine has no names, and the semantic clustering stage
 * needs to know what it is looking at: an inductor and its output capacitor are
 * a different pattern from two decoupling capacitors.
 */
typedef uint8_t comp_kind_t;

enum : comp_kind_t {
    COMP_KIND_UNKNOWN = 0,
    COMP_KIND_IC,          /* U */
    COMP_KIND_CAPACITOR,   /* C, CE */
    COMP_KIND_RESISTOR,    /* R */
    COMP_KIND_INDUCTOR,    /* L, FB */
    COMP_KIND_DIODE_LED,   /* D */
    COMP_KIND_TRANSISTOR,  /* Q */
    COMP_KIND_CRYSTAL,     /* Y, X, XTAL */
    COMP_KIND_CONNECTOR,   /* J, P, CN */
    COMP_KIND_MECHANICAL,  /* H, MK, FID, TP */
    COMP_KIND_OTHER        /* recognised prefix, no specific role */
};

static_assert(sizeof(comp_kind_t) == 1, "comp_kind_t must stay 1 byte");

/*
 * Component SoA. `x`/`y` are the courtyard centre; `half_w`/`half_h` the
 * courtyard AABB half extents (always positive once ingestion is finalised:
 * explicit, polygon-derived, or pad-bbox + margin in degraded mode).
 */
typedef struct {
    coord_t       *x;          /* centre X, mm */
    coord_t       *y;          /* centre Y, mm */
    coord_t       *half_w;     /* courtyard AABB half width, mm */
    coord_t       *half_h;     /* courtyard AABB half height, mm */
    coord_t       *height;     /* z extent, mm; 0 = unknown (2D placement) */
    coord_t       *mass;       /* grams; 0 = unknown (no CoG / inertia) */
    comp_flags_t  *flags;      /* packed COMP_* bits */
    comp_kind_t   *kind;       /* role derived from the reference prefix */
    place_id_t    *first_pin;  /* CSR: first pin index of this component */
    place_count_t *pin_count;  /* pins owned by this component */
    place_id_t    *polygon_id; /* exact courtyard in polygon_pool, or NONE */
    uint32_t      *part_id;    /* hashed supplier part number; 0 = unknown */
    uint32_t       count;
    uint32_t       capacity;
} components_soa_t;

/* ------------------------------------------------------------------------- */
/* Pins                                                                       */
/* ------------------------------------------------------------------------- */

/*
 * pin_flags packs the electrical role, upgraded from optional PDF/pinout data:
 *   [0] power rail, [1] ground, [2]/[3] differential P/N, [4] clock,
 *   [5] plated through hole, [6] non-plated hole,
 *   [7] value came from name inference rather than authoritative data.
 */
typedef uint8_t pin_flags_t;

enum : pin_flags_t {
    PIN_POWER    = 0x01u,
    PIN_GROUND   = 0x02u,
    PIN_DIFF_P   = 0x04u,
    PIN_DIFF_N   = 0x08u,
    PIN_CLOCK    = 0x10u,
    PIN_PTH      = 0x20u,   /* the pad is a plated through hole: copper on both faces */
    /* A non-plated hole carries no copper, but it is still a hole: the drill
     * and the solder-mask opening around it cut through every layer, so a part
     * on the far side of the board cannot be placed over it either. */
    PIN_NPTH     = 0x40u,
    PIN_INFERRED = 0x80u
};

static_assert(sizeof(pin_flags_t) == 1, "packed pin flags must stay 1 byte");

typedef struct {
    coord_t      *offset_x;    /* mm, unrotated, from component centre */
    coord_t      *offset_y;
    place_id_t   *comp_id;     /* owner; denormalised on purpose: HPWL needs
                                * pin -> component X/Y without a second lookup */
    place_id_t   *net_id;      /* PLACE_ID_NONE = unconnected */
    pin_flags_t  *flags;       /* electrical role, possibly inferred */
    coord_t      *max_current; /* A; 0 = unknown (standard net weight) */
    coord_t      *half_x;      /* mm, pad copper half extent in the footprint frame */
    coord_t      *half_y;
    uint32_t      count;
    uint32_t      capacity;
} pins_soa_t;

/* ------------------------------------------------------------------------- */
/* Netlist hypergraph (CSR)                                                   */
/* ------------------------------------------------------------------------- */

/*
 * Net -> pin adjacency in Compressed Sparse Row form. A PCB netlist is a
 * *simple* bipartite graph (a pin belongs to at most one net), so the reverse
 * adjacency is just `pins.net_id` and no second CSR is needed.
 *
 * Row `n` owns net_to_pins[net_offsets[n] .. net_offsets[n+1]).
 * Entries of a row are pin ids; rows are ordered by net id.
 */
typedef struct {
    uint32_t     *net_offsets;  /* num_nets + 1 row starts */
    place_id_t   *net_to_pins;  /* num_entries pin ids, grouped by net */
    coord_t      *weights;      /* num_nets; uniform 1.0f in degraded mode */
    uint8_t      *net_class;    /* num_nets; 0 = default class */
    uint32_t      num_nets;
    uint32_t      num_entries;
} netlist_csr_t;

#endif /* PLACE_TYPES_H */
