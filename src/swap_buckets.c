/*
 * swap_buckets.c - who may be exchanged with whom.
 *
 * Two parts are interchangeable when the placement cannot tell them apart:
 * same shape, same face, same rotation freedom - and the same PART. That last
 * one is what makes the exchange electrically sound: two 0603 capacitors of
 * 100 nF and 1 uF share a footprint, and swapping them would silently rewrite
 * the schematic. The identity is hashed by the producer (comps.part_id); zero
 * means it is unknown, and an unknown part is never swapped.
 *
 * The buckets are a CSR, built once at init.
 */
#include "swap.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* Two parts are interchangeable: same shape, same face, same rotation freedom,
 * same part. */
typedef struct {
    uint32_t    part_id;
    comp_kind_t kind;
    uint16_t    pins;
    coord_t     half_w, half_h;
    uint8_t     side;
    uint8_t     rot_fixed;
} swap_key_t;

static bool key_of(const solver_t *s, uint32_t i, swap_key_t *key)
{
    const components_soa_t *c = &s->ctx->comps;
    if (c->part_id == nullptr || c->part_id[i] == 0u) {
        return false; /* no identity: never exchanged */
    }
    if (!solver_component_is_movable(s, i)) {
        return false;
    }
    if (s->cluster_of[i] != SOLVER_NO_INDEX) {
        return false; /* a satellite follows its master; a master drags it */
    }
    if (s->opt->swap_max_pins > 0u && c->pin_count[i] > s->opt->swap_max_pins) {
        return false;
    }
    key->part_id = c->part_id[i];
    key->kind = c->kind[i];
    key->pins = (uint16_t)c->pin_count[i];
    /* Quantised: two parts of the same footprint must land in the same bucket
     * even when their derived extents differ in the last bit. */
    key->half_w = roundf(s->half_w[i] * 100.0f) * 0.01f;
    key->half_h = roundf(s->half_h[i] * 100.0f) * 0.01f;
    key->side = s->side[i];
    key->rot_fixed = (s->opt->respect_rot_fixed && (c->flags[i] & COMP_ROT_FIXED) != 0u) ? 1u
                                                                                        : 0u;
    return true;
}

static int key_cmp(const swap_key_t *a, const swap_key_t *b)
{
    if (a->part_id != b->part_id) {
        return (a->part_id < b->part_id) ? -1 : 1;
    }
    if (a->kind != b->kind) {
        return (int)a->kind - (int)b->kind;
    }
    if (a->pins != b->pins) {
        return (int)a->pins - (int)b->pins;
    }
    if (a->side != b->side) {
        return (int)a->side - (int)b->side;
    }
    if (a->rot_fixed != b->rot_fixed) {
        return (int)a->rot_fixed - (int)b->rot_fixed;
    }
    if (a->half_w < b->half_w) {
        return -1;
    }
    if (a->half_w > b->half_w) {
        return 1;
    }
    if (a->half_h < b->half_h) {
        return -1;
    }
    if (a->half_h > b->half_h) {
        return 1;
    }
    return 0;
}

bool swap_buckets_build(solver_t *s)
{
    const uint32_t n = s->ncomp;
    uint32_t *order = SOLVER_ALLOC(s, uint32_t, (n > 0u) ? n : 1u);
    swap_key_t *keys = SOLVER_ALLOC(s, swap_key_t, (n > 0u) ? n : 1u);
    s->swap_first = SOLVER_ALLOC(s, uint32_t, (n > 0u) ? n : 1u);
    s->swap_len = SOLVER_ALLOC(s, uint32_t, (n > 0u) ? n : 1u);
    s->swap_member = SOLVER_ALLOC(s, uint32_t, (n > 0u) ? n : 1u);
    s->swap_bucket_of = SOLVER_ALLOC(s, uint32_t, (n > 0u) ? n : 1u);
    if (!s->ok) {
        return false;
    }
    for (uint32_t i = 0u; i < n; ++i) {
        s->swap_bucket_of[i] = SOLVER_NO_INDEX;
    }
    uint32_t count = 0u;
    for (uint32_t i = 0u; i < n; ++i) {
        swap_key_t key;
        if (key_of(s, i, &key)) {
            order[count] = i;
            keys[count] = key;
            count += 1u;
        }
    }
    for (uint32_t i = 1u; i < count; ++i) { /* insertion sort on a total order */
        const uint32_t o = order[i];
        const swap_key_t k = keys[i];
        uint32_t j = i;
        while (j > 0u && key_cmp(&keys[j - 1u], &k) > 0) {
            order[j] = order[j - 1u];
            keys[j] = keys[j - 1u];
            j -= 1u;
        }
        order[j] = o;
        keys[j] = k;
    }
    s->swap_bucket_count = 0u;
    uint32_t write = 0u;
    uint32_t i = 0u;
    while (i < count) {
        uint32_t j = i + 1u;
        while (j < count && key_cmp(&keys[i], &keys[j]) == 0) {
            j += 1u;
        }
        if (j - i >= 2u) {
            s->swap_first[s->swap_bucket_count] = write;
            s->swap_len[s->swap_bucket_count] = j - i;
            for (uint32_t k = i; k < j; ++k) {
                s->swap_member[write++] = order[k];
                s->swap_bucket_of[order[k]] = s->swap_bucket_count;
            }
            s->swap_bucket_count += 1u;
        }
        i = j;
    }
    if (getenv("DODPLACE_SWAP_DEBUG") != nullptr) {
        (void)fprintf(stderr, "swap buckets: %u (from %u eligible parts)\n",
                      s->swap_bucket_count, count);
        for (uint32_t b = 0u; b < s->swap_bucket_count && b < 8u; ++b) {
            (void)fprintf(stderr, "  bucket %u: %u members (part %08x, %u pins)\n", b,
                          s->swap_len[b], s->ctx->comps.part_id[s->swap_member[s->swap_first[b]]],
                          s->ctx->comps.pin_count[s->swap_member[s->swap_first[b]]]);
        }
    }
    return s->ok;
}
