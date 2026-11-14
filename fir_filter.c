/*
 * FIR filter with decimation + filter coefficient generation
 *
 * Copyright (c) 2026 CEMAXECUTER LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * FIR filter with decimation + filter coefficient generation
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "fir_filter.h"
#include "simd_kernels.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- FIR filter creation/destruction ---- */

fir_filter_t *fir_filter_create(const float *taps, int ntaps) {
    fir_filter_t *f = malloc(sizeof(*f));
    f->ntaps = ntaps;
    /* Allocate taps aligned to 32 bytes and zero-padded to multiple of 8
     * for SIMD loop efficiency (no tail handling needed) */
    int padded = pad_to_8(ntaps);
    f->taps = aligned_alloc_32(sizeof(float) * padded);
    memcpy(f->taps, taps, sizeof(float) * ntaps);
    /* Zero-pad remainder */
    for (int i = ntaps; i < padded; i++)
        f->taps[i] = 0.0f;
    return f;
}

void fir_filter_destroy(fir_filter_t *f) {
    if (!f) return;
    free(f->taps);
    free(f);
}

/* ---- Complex FIR filter ---- */

void fir_filter_ccf(fir_filter_t *f, float complex *out,
                    const float complex *in, int n) {
    simd_fir_ccf(f->taps, f->ntaps, in, out, n);
}

/* ---- Complex FIR filter with decimation ---- */

void fir_filter_ccf_dec(fir_filter_t *f, float complex *out,
                        const float complex *in, int n_out, int decimation) {
    simd_fir_ccf_dec(f->taps, f->ntaps, in, out, n_out, decimation);
}

/* ---- Real FIR filter ---- */

void fir_filter_fff(fir_filter_t *f, float *out, const float *in, int n) {
    simd_fir_fff(f->taps, f->ntaps, in, out, n);
}

/* ---- sinc function ---- */

static float sincf(float x) {
    if (fabsf(x) < 1e-10f) return 1.0f;
    return sinf((float)M_PI * x) / ((float)M_PI * x);
}

/* ---- Root Raised Cosine taps ---- */

float *rrc_taps(int *ntaps_out, float gain, float sample_rate,
                float symbol_rate, float alpha, int ntaps) {
    /* Ensure odd number of taps */
    ntaps |= 1;
    *ntaps_out = ntaps;

    float *taps = malloc(sizeof(float) * ntaps);
    float sps = sample_rate / symbol_rate;
    int center = ntaps / 2;

    float energy = 0;
    for (int i = 0; i < ntaps; i++) {
        float t = (i - center) / sps;

        if (fabsf(t) < 1e-10f) {
            /* t = 0 */
            taps[i] = (1.0f - alpha + 4.0f * alpha / (float)M_PI);
        } else if (fabsf(fabsf(t) - 1.0f / (4.0f * alpha)) < 1e-6f) {
            /* t = +/- 1/(4*alpha) */
            taps[i] = alpha / sqrtf(2.0f) *
                ((1.0f + 2.0f / (float)M_PI) * sinf((float)M_PI / (4.0f * alpha)) +
                 (1.0f - 2.0f / (float)M_PI) * cosf((float)M_PI / (4.0f * alpha)));
        } else {
            float num = sinf((float)M_PI * t * (1.0f - alpha)) +
                        4.0f * alpha * t * cosf((float)M_PI * t * (1.0f + alpha));
            float den = (float)M_PI * t * (1.0f - (4.0f * alpha * t) * (4.0f * alpha * t));
            taps[i] = num / den;
        }
        energy += taps[i] * taps[i];
    }

    /* Normalize */
    float scale = gain / sqrtf(energy);
    for (int i = 0; i < ntaps; i++)
        taps[i] *= scale;

    return taps;
}

/* ---- Raised Cosine taps ---- */

float *rc_taps(int *ntaps_out, float sample_rate, float symbol_rate,
               float alpha, int ntaps) {
    ntaps |= 1;
    *ntaps_out = ntaps;

    float *taps = malloc(sizeof(float) * ntaps);
    float sps = sample_rate / symbol_rate;
    int center = ntaps / 2;

    for (int i = 0; i < ntaps; i++) {
        float t = (i - center) / sps;

        if (fabsf(t) < 1e-10f) {
            taps[i] = 1.0f;
        } else if (alpha > 0 && fabsf(fabsf(t) - 1.0f / (2.0f * alpha)) < 1e-6f) {
            taps[i] = (float)M_PI / (4.0f) * sincf(1.0f / (2.0f * alpha));
        } else {
            float cos_term = cosf((float)M_PI * alpha * t);
            float den = 1.0f - (2.0f * alpha * t) * (2.0f * alpha * t);
            taps[i] = sincf(t) * cos_term / den;
        }
    }

    return taps;
}

/* ---- Low-pass filter taps (windowed sinc with Blackman-Harris) ---- */

float *lpf_taps(int *ntaps_out, float gain, float sample_rate,
                float cutoff_freq, float transition_width) {
    /* Estimate filter order from transition width */
    int ntaps = (int)(4.0f / (transition_width / sample_rate));
    ntaps |= 1;
    *ntaps_out = ntaps;

    float *taps = malloc(sizeof(float) * ntaps);
    int center = ntaps / 2;
    float omega_c = 2.0f * (float)M_PI * cutoff_freq / sample_rate;

    float energy = 0;
    for (int i = 0; i < ntaps; i++) {
        float n = i - center;
        /* Sinc */
        float h;
        if (fabsf(n) < 1e-10f)
            h = omega_c / (float)M_PI;
        else
            h = sinf(omega_c * n) / ((float)M_PI * n);

        /* Blackman-Harris window */
        float w = 0.35875f
                - 0.48829f * cosf(2.0f * (float)M_PI * i / (ntaps - 1))
                + 0.14128f * cosf(4.0f * (float)M_PI * i / (ntaps - 1))
                - 0.01168f * cosf(6.0f * (float)M_PI * i / (ntaps - 1));

        taps[i] = h * w;
        energy += taps[i];
    }

    /* Normalize to unity DC gain, then apply desired gain */
    if (fabsf(energy) > 0) {
        float scale = gain / energy;
        for (int i = 0; i < ntaps; i++)
            taps[i] *= scale;
    }

    return taps;
}

/* ---- Low-pass filter taps (windowed sinc with Kaiser, explicit edges) ---- */

/* Zeroth-order modified Bessel function of the first kind (power series). */
static double bessel_i0(double x) {
    double sum = 1.0, term = 1.0, q = x * x / 4.0;
    for (int k = 1; k < 64; k++) {
        term *= q / ((double)k * k);
        sum += term;
        if (term < sum * 1e-12) break;
    }
    return sum;
}

float *kaiser_lpf_taps(int *ntaps_out, float sample_rate, float pass_edge,
                       float stop_edge, float atten_db) {
    /* Kaiser's estimates for length and beta (Oppenheim & Schafer 7.6). */
    double dw = 2.0 * M_PI * (stop_edge - pass_edge) / sample_rate;
    int ntaps = (int)ceil((atten_db - 8.0) / (2.285 * dw));
    ntaps |= 1;
    double beta = atten_db > 50 ? 0.1102 * (atten_db - 8.7)
                : atten_db >= 21 ? 0.5842 * pow(atten_db - 21, 0.4)
                                   + 0.07886 * (atten_db - 21)
                : 0.0;
    *ntaps_out = ntaps;

    float *taps = malloc(sizeof(float) * ntaps);
    int center = ntaps / 2;
    double omega_c = M_PI * (pass_edge + stop_edge) / sample_rate;
    double i0_beta = bessel_i0(beta);
    double sum = 0;
    for (int i = 0; i < ntaps; i++) {
        int n = i - center;
        double h = n == 0 ? omega_c / M_PI : sin(omega_c * n) / (M_PI * n);
        double r = (double)n / center;
        double w = bessel_i0(beta * sqrt(1.0 - r * r)) / i0_beta;
        taps[i] = (float)(h * w);
        sum += taps[i];
    }
    /* Unity DC gain */
    for (int i = 0; i < ntaps; i++)
        taps[i] = (float)(taps[i] / sum);
    return taps;
}

/* ---- Box (averaging) filter taps ---- */

float *box_taps(int *ntaps_out, int length) {
    *ntaps_out = length;
    float *taps = malloc(sizeof(float) * length);
    float val = 1.0f / length;
    for (int i = 0; i < length; i++)
        taps[i] = val;
    return taps;
}
