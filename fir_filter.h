/*
 * FIR filter with decimation + filter coefficient generation
 *
 * Copyright (c) 2026 CEMAXECUTER LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * FIR filter with decimation
 */

#ifndef __FIR_FILTER_H__
#define __FIR_FILTER_H__

#include <complex.h>
#include <stddef.h>

typedef struct {
    float *taps;
    int ntaps;
} fir_filter_t;

/* Create FIR filter (copies taps) */
fir_filter_t *fir_filter_create(const float *taps, int ntaps);

/* Destroy FIR filter */
void fir_filter_destroy(fir_filter_t *f);

/* Filter n complex samples: out[i] = sum(taps[k] * in[i+k]) */
void fir_filter_ccf(fir_filter_t *f, float complex *out,
                    const float complex *in, int n);

/* Filter with decimation: out[i] = sum(taps[k] * in[i*dec+k]) */
void fir_filter_ccf_dec(fir_filter_t *f, float complex *out,
                        const float complex *in, int n_out, int decimation);

/* Filter n real samples */
void fir_filter_fff(fir_filter_t *f, float *out,
                    const float *in, int n);

/* Generate root-raised-cosine filter taps */
float *rrc_taps(int *ntaps_out, float gain, float sample_rate,
                float symbol_rate, float alpha, int ntaps);

/* Generate raised-cosine filter taps */
float *rc_taps(int *ntaps_out, float sample_rate, float symbol_rate,
               float alpha, int ntaps);

/* Generate low-pass filter taps (windowed sinc) */
float *lpf_taps(int *ntaps_out, float gain, float sample_rate,
                float cutoff_freq, float transition_width);

/* Generate low-pass filter taps (windowed sinc, Kaiser window) for a
 * passband up to pass_edge and at least atten_db rejection from stop_edge.
 * Unlike lpf_taps the length follows from the edges and attenuation, so a
 * wide transition gets a short filter. Unity DC gain. */
float *kaiser_lpf_taps(int *ntaps_out, float sample_rate, float pass_edge,
                       float stop_edge, float atten_db);

/* Generate a simple box/averaging filter */
float *box_taps(int *ntaps_out, int length);

#endif
