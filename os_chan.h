/*
 * Overlap-save burst channelizer (ka9q-radio style fast convolution)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The downmix used to take every burst at the full input rate and, per
 * burst, frequency-shift it and run a decimating FIR over it. Bursts overlap
 * in time (about 8 at once on a busy sky), so the same input samples were
 * shifted and filtered about 8 times over.
 *
 * This does the filtering once per stretch of input, in the frequency
 * domain, and shares it. The input is cut into blocks of L new samples with
 * V = N - L samples of overlap and each block gets one N-point FFT. A burst
 * then takes, from each block it spans, the M = N / D bins around its
 * frequency, multiplied by the anti-alias filter's response, and one M-point
 * inverse FFT gives its samples at the output rate. Block FFTs are computed
 * on demand and cached, so a block no burst touches costs nothing and one
 * that eight bursts touch is transformed once.
 *
 * The output is the same as the time-domain path it replaces (coarse
 * frequency shift by the detector bin, then the 801-tap decimating filter):
 * same filter, same sample grid and same phase reference, to float rounding.
 */

#ifndef __OS_CHAN_H__
#define __OS_CHAN_H__

#include <complex.h>
#include <stdint.h>

typedef struct os_chan os_chan_t;

/* Copies input samples [start, start + n) into dst. Samples it doesn't have
 * (before the oldest one kept, or not yet received) are zero-filled. Returns
 * 1 when all n were real samples, 0 otherwise. */
typedef int (*os_chan_read_fn)(void *ctx, uint64_t start, size_t n,
                               float complex *dst);

/* in_rate must be a multiple of out_rate; returns NULL otherwise (the caller
 * keeps the time-domain path). */
os_chan_t *os_chan_create(int in_rate, int out_rate,
                          os_chan_read_fn read, void *read_ctx);
void os_chan_destroy(os_chan_t *c);

/* Bins of the N-point block FFT per bin of an fft_size-point detector FFT,
 * or 0 when N isn't a multiple of fft_size. */
int os_chan_bins_per(const os_chan_t *c, int fft_size);
int os_chan_fft_size(const os_chan_t *c);

/* Output-rate samples of input [start, stop) shifted down by bin_shift / N
 * cycles per sample, as the time-domain path would give: out[k] is the
 * filter over input start + k*D ... start + k*D + ntaps - 1, with phase zero
 * at start. Returns the number written (at most max_out), and sets
 * *delay_ns to the filter's group delay. */
int os_chan_extract(os_chan_t *c, uint64_t start, uint64_t stop,
                    int bin_shift, float complex *out, int max_out,
                    uint64_t *delay_ns);

/* Counters for diagnostics: blocks transformed, blocks served from cache. */
void os_chan_stats(const os_chan_t *c, uint64_t *ffts, uint64_t *hits);

#endif
