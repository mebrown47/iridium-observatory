/*
 * Real-to-IQ conversion for Airspy R2 / Mini raw samples
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See airspy_conv.h. With x the real input, z[n] = x[n] e^{-i pi n / 2} is
 * the fs/4-shifted signal: z[2i] = (-1)^i x[2i] is real and
 * z[2i+1] = -i (-1)^i x[2i+1] is imaginary. A halfband h of length 4J+3 has
 * its centre c = 2J+1 at an odd index and is zero at every other odd index,
 * so of y[m] = sum_k h[k] z[2m - k] only the even k reach the even (real)
 * samples and only k = c reaches an odd (imaginary) one:
 *
 *     I[m] = sum_j h[2j] e[m - j],     e[i] = (-1)^i x[2i]
 *     Q[m] = h[c] o[m - (c + 1) / 2],  o[i] = (-1)^(i+1) x[2i+1]
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "airspy_conv.h"
#include "fir_filter.h"
#include "simd_kernels.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* 79 taps, Kaiser beta 6.0, cutoff fs/4: flat to 0.45 fs_iq (0.00 dB at
 * 4.5 MHz for 10 MS/s), -70 dB at 0.55 fs_iq and -72 dB beyond 0.6 fs_iq.
 * Against libairspy's 47-tap filter (-0.35 dB at 4.5 MHz, -28 dB at 5.5,
 * -62 beyond 6) that is a flatter top edge, where the simplex channels sit
 * at 1626.0-1626.5 MHz, and much less aliasing onto it from just outside
 * the band. 40 nonzero taps reach I. */
#define HB_NTAPS 79
#define HB_BETA  6.0
#define HB_C     (HB_NTAPS / 2)               /* 39 */
#define HB_NI    (HB_NTAPS / 2 + 1)           /* 40 taps over e[] */
#define HB_QDLY  ((HB_C + 1) / 2)             /* 20 */

/* DC: running mean of the raw samples, updated once per block. The Airspy's
 * ADC offset is static, so a slow tracker is enough, and a per-block
 * constant keeps the inner loop free of a per-sample recursion. */
#define DC_ALPHA 0.05

#define SCALE (1.0f / 2048.0f)

struct airspy_conv {
    float taps[HB_NTAPS];
    fir_filter_t *fir;          /* the HB_NI even taps */
    float hc;                   /* centre tap */
    double dc;
    int dc_init;
    int odd;                    /* parity of the next e/o index */
    float *e;                   /* HB_NI - 1 history + block */
    float *o;                   /* HB_QDLY history + block */
    float *iout;
    size_t cap;                 /* block capacity in IQ samples */
};

static double bessel_i0(double x) {
    double sum = 1.0, term = 1.0, q = x * x / 4.0;
    for (int k = 1; k < 64; k++) {
        term *= q / ((double)k * k);
        sum += term;
        if (term < sum * 1e-12) break;
    }
    return sum;
}

airspy_conv_t *airspy_conv_create(void) {
    airspy_conv_t *c = calloc(1, sizeof(*c));
    double sum = 0, i0b = bessel_i0(HB_BETA);
    double h[HB_NTAPS];
    for (int k = 0; k < HB_NTAPS; k++) {
        int d = k - HB_C;
        double v;
        if (d == 0)
            v = 0.5;
        else if (d % 2 == 0)
            v = 0.0;                            /* exact halfband zeros */
        else
            v = sin(M_PI * d / 2.0) / (M_PI * d);
        double r = (double)d / HB_C;
        h[k] = v * bessel_i0(HB_BETA * sqrt(1.0 - r * r)) / i0b;
        sum += h[k];
    }
    for (int k = 0; k < HB_NTAPS; k++)
        c->taps[k] = (float)(h[k] / sum);
    c->hc = c->taps[HB_C];

    float even[HB_NI];
    for (int j = 0; j < HB_NI; j++)
        even[j] = c->taps[2 * j];               /* symmetric: order is moot */
    c->fir = fir_filter_create(even, HB_NI);
    return c;
}

void airspy_conv_destroy(airspy_conv_t *c) {
    if (!c) return;
    fir_filter_destroy(c->fir);
    free(c->e);
    free(c->o);
    free(c->iout);
    free(c);
}

const float *airspy_conv_taps(const airspy_conv_t *c, int *ntaps_out) {
    *ntaps_out = HB_NTAPS;
    return c->taps;
}

static int grow(airspy_conv_t *c, size_t m) {
    if (m <= c->cap)
        return 0;
    size_t cap = m + m / 2;
    float *e = aligned_alloc_32(sizeof(float) * (cap + HB_NI - 1 + 8));
    float *o = aligned_alloc_32(sizeof(float) * (cap + HB_QDLY));
    float *iout = aligned_alloc_32(sizeof(float) * (cap + 8));
    if (!e || !o || !iout) {
        free(e); free(o); free(iout);
        return -1;
    }
    if (c->e) {
        memcpy(e, c->e, sizeof(float) * (HB_NI - 1));
        memcpy(o, c->o, sizeof(float) * HB_QDLY);
    } else {
        memset(e, 0, sizeof(float) * (HB_NI - 1));
        memset(o, 0, sizeof(float) * HB_QDLY);
    }
    free(c->e); free(c->o); free(c->iout);
    c->e = e; c->o = o; c->iout = iout;
    c->cap = cap;
    return 0;
}

size_t airspy_conv_process(airspy_conv_t *c, const uint16_t *in, size_t n,
                           float complex *out) {
    size_t m = n / 2;
    if (m == 0 || grow(c, m) != 0)
        return 0;

    uint64_t acc = 0;
    for (size_t i = 0; i < 2 * m; i++)
        acc += in[i];
    double mean = (double)acc / (double)(2 * m);
    if (!c->dc_init) {
        c->dc = mean;
        c->dc_init = 1;
    } else {
        c->dc += DC_ALPHA * (mean - c->dc);
    }
    const float dc = (float)c->dc;

    /* Split, remove DC, scale and apply the fs/4 signs. sgn is +1 for an
     * even index i, so e = +x, o = -x there, and the other way round for an
     * odd one. */
    float *e = c->e + (HB_NI - 1);
    float *o = c->o + HB_QDLY;
    float s0 = c->odd ? -SCALE : SCALE;
    size_t i = 0;
    for (; i + 1 < m; i += 2) {
        e[i]     =  s0 * ((float)in[2 * i]     - dc);
        o[i]     = -s0 * ((float)in[2 * i + 1] - dc);
        e[i + 1] = -s0 * ((float)in[2 * i + 2] - dc);
        o[i + 1] =  s0 * ((float)in[2 * i + 3] - dc);
    }
    if (i < m) {
        e[i] =  s0 * ((float)in[2 * i]     - dc);
        o[i] = -s0 * ((float)in[2 * i + 1] - dc);
    }
    if (m & 1)
        c->odd = !c->odd;

    /* I: the even taps over e (history first); Q: o delayed. I is negated:
     * -conj(I + iQ) = -I + iQ is the +fs/4 shift instead of -fs/4, which is
     * the orientation the Airspy's IF needs (high side: with -fs/4 the band
     * comes out mirrored, as checked against libairspy's output). */
    fir_filter_fff(c->fir, c->iout, c->e, (int)m);
    const float hc = c->hc;
    float *y = (float *)out;
    for (size_t k = 0; k < m; k++) {
        y[2 * k]     = -c->iout[k];
        y[2 * k + 1] = hc * c->o[k];
    }

    memmove(c->e, c->e + m, sizeof(float) * (HB_NI - 1));
    memmove(c->o, c->o + m, sizeof(float) * HB_QDLY);
    return m;
}
