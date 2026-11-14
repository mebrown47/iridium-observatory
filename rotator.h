/*
 * Complex frequency rotator
 *
 * Copyright (c) 2026 CEMAXECUTER LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Complex frequency rotator
 */

#ifndef __ROTATOR_H__
#define __ROTATOR_H__

#include <complex.h>
#include <math.h>

#include "simd_kernels.h"

typedef struct {
    float complex phase;
    float complex phase_incr;
} rotator_t;

static inline void rotator_init(rotator_t *r) {
    r->phase = 1.0f;
    r->phase_incr = 1.0f;
}

static inline void rotator_set_phase(rotator_t *r, float complex phase) {
    r->phase = phase;
}

static inline void rotator_set_phase_incr(rotator_t *r, float complex incr) {
    r->phase_incr = incr;
}

/* Rotate n samples: out[i] = in[i] * phase; phase *= phase_incr.
 * in may equal out. The work is simd_rotate (simd_kernels.h): lane-parallel
 * phasors, since a single running one is latency-bound (it was 3.4 ns a
 * sample, over a quarter of the downmix, for the full-rate coarse CFO). */
static inline void rotator_rotate_n(rotator_t *r, float complex *out,
                                     const float complex *in, int n) {
    if (simd_rotate)
        simd_rotate(in, out, n, &r->phase, r->phase_incr);
    else
        generic_rotate(in, out, n, &r->phase, r->phase_incr);
    /* Re-normalize phase to prevent drift */
    float mag = cabsf(r->phase);
    if (mag > 0)
        r->phase /= mag;
}

#endif
