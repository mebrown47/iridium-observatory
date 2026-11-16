/*
 * Real-to-IQ conversion for Airspy R2 / Mini raw samples
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The R2 and Mini sample a real IF at twice the IQ rate (20 MS/s real for
 * 10 MS/s IQ) and leave the conversion to the host. libairspy's own
 * converter ran mostly scalar and was half of this program's CPU on an
 * Airspy feed once the burst downmix was fast. This one is the textbook
 * method, vectorised:
 *
 *   1. remove DC (a slow running mean, updated per block),
 *   2. shift the band down by fs/4: multiply by 1, -j, -1, j, ...
 *   3. halfband low-pass and keep every second sample.
 *
 * After the fs/4 shift the even input samples are purely real and the odd
 * ones purely imaginary, and a halfband filter is zero at every even
 * distance from its centre. So I is a short real FIR over the even samples
 * and Q is the odd samples delayed and scaled by the centre tap: no complex
 * arithmetic anywhere, and the FIR runs on the SIMD real-FIR kernel.
 */

#ifndef __AIRSPY_CONV_H__
#define __AIRSPY_CONV_H__

#include <complex.h>
#include <stddef.h>
#include <stdint.h>

typedef struct airspy_conv airspy_conv_t;

airspy_conv_t *airspy_conv_create(void);
void airspy_conv_destroy(airspy_conv_t *c);

/* n raw samples (12-bit offset binary in uint16, as UINT16_REAL delivers
 * them; n even) to n/2 IQ samples in out. Full scale is 1.0, as with
 * libairspy's FLOAT32_IQ. Returns the number of IQ samples written. */
size_t airspy_conv_process(airspy_conv_t *c, const uint16_t *in, size_t n,
                           float complex *out);

/* Halfband taps used (for tests): *ntaps_out is the full length. */
const float *airspy_conv_taps(const airspy_conv_t *c, int *ntaps_out);

#endif
