/*
 * Overlap-save burst channelizer (ka9q-radio style fast convolution)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See os_chan.h for what this is for. Notation:
 *   D  decimation, in_rate / out_rate (40 at 10 MS/s -> 250 kS/s)
 *   M  output bins per block, OS_CHAN_M (1024: 244 Hz bins)
 *   N  block FFT size, M * D (40960 at 10 MS/s)
 *   L  new input samples per block, 3N/4; V = N - L overlap
 *
 * Block b transforms input [bL - V, bL + L). With the filter's taps h[0..P)
 * zero-padded to N, the circular convolution of that block with h is the
 * linear one for the last L positions (V >= P - 1), and decimating it by D
 * is an M-point inverse FFT of the M bins around the burst's frequency:
 *     z[j] = sum_t h[t] x[B + jD + tau - t] e^{-2 pi i c (jD + tau - t) / N}
 * for block start B = bL - V, bin shift c and a time advance tau < D (a
 * phase ramp on the bins). The old path's out[k] is that same sum at
 * m = start + kD + P - 1, phase-referenced to start instead of B, so tau is
 * chosen to put the D-sample grid on m and each block's output is turned by
 * e^{2 pi i c (start - B) / N}. h is symmetric, so correlation (the old FIR's
 * form) and convolution are the same filter.
 */

#include <complex.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <fftw3.h>

#include "os_chan.h"
#include "fir_filter.h"
#include "fftw_lock.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define OS_CHAN_M      1024
/* Cached block FFTs: 64 blocks of 3.07 ms (at 10 MS/s) span ~200 ms, well
 * past how far apart in time the bursts of one emission batch start. A miss
 * costs only a recompute. */
#define OS_CHAN_SLOTS  64

struct os_chan {
    int D, M, N, L, V, ntaps;
    uint64_t delay_ns;

    /* G[tau * M + idx]: filter response at bin idx (k' = idx - M if
     * idx >= M/2), times the tau-sample advance ramp, times 1/N. */
    float complex *G;

    fftwf_plan fwd;             /* N-point, fin -> a slot or fout */
    float complex *fin, *fout;
    fftwf_plan inv;             /* M-point, zin -> zout */
    float complex *zin, *zout;

    float complex *slots;       /* OS_CHAN_SLOTS * N */
    int64_t tag[OS_CHAN_SLOTS]; /* block index held, -1 if none */

    os_chan_read_fn read;
    void *read_ctx;

    uint64_t n_fft, n_hit;
};

os_chan_t *os_chan_create(int in_rate, int out_rate,
                          os_chan_read_fn read, void *read_ctx) {
    if (in_rate <= 0 || out_rate <= 0 || in_rate % out_rate != 0)
        return NULL;
    int D = in_rate / out_rate;
    if (D < 2)
        return NULL;

    os_chan_t *c = calloc(1, sizeof(*c));
    c->D = D;
    c->M = OS_CHAN_M;
    c->N = c->M * D;
    c->V = c->N / 4;
    c->L = c->N - c->V;
    c->read = read;
    c->read_ctx = read_ctx;
    for (int i = 0; i < OS_CHAN_SLOTS; i++)
        c->tag[i] = -1;

    /* The same filter the time-domain path uses for a one-stage decimation. */
    int ntaps;
    float *taps = lpf_taps(&ntaps, 1.0f, (float)in_rate,
                           out_rate * 0.4f, out_rate * 0.2f);
    c->ntaps = ntaps;
    c->delay_ns = (uint64_t)((double)(ntaps / 2) / in_rate * 1e9);
    if (ntaps - 1 > c->V) {         /* can't happen for these designs */
        free(taps);
        free(c);
        return NULL;
    }

    c->fin = fftwf_alloc_complex(c->N);
    c->fout = fftwf_alloc_complex(c->N);
    c->slots = fftwf_alloc_complex((size_t)OS_CHAN_SLOTS * c->N);
    c->zin = fftwf_alloc_complex(c->M);
    c->zout = fftwf_alloc_complex(c->M);
    c->G = malloc(sizeof(float complex) * (size_t)D * c->M);

    fftw_lock();
    c->fwd = fftwf_plan_dft_1d(c->N, (fftwf_complex *)c->fin,
                               (fftwf_complex *)c->fout,
                               FFTW_FORWARD, FFTW_MEASURE);
    c->inv = fftwf_plan_dft_1d(c->M, (fftwf_complex *)c->zin,
                               (fftwf_complex *)c->zout,
                               FFTW_BACKWARD, FFTW_MEASURE);
    fftw_unlock();

    /* Filter response at the M bins around 0 (planning may have scribbled
     * on fin, so fill it after). */
    memset(c->fin, 0, sizeof(float complex) * c->N);
    for (int t = 0; t < ntaps; t++)
        c->fin[t] = taps[t];
    free(taps);
    fftwf_execute(c->fwd);
    for (int tau = 0; tau < D; tau++) {
        for (int idx = 0; idx < c->M; idx++) {
            int kp = idx < c->M / 2 ? idx : idx - c->M;
            double complex ramp = cexp(I * 2.0 * M_PI * kp * tau / c->N);
            c->G[(size_t)tau * c->M + idx] =
                (float complex)(c->fout[(kp + c->N) % c->N] * ramp / c->N);
        }
    }
    return c;
}

void os_chan_destroy(os_chan_t *c) {
    if (!c) return;
    fftw_lock();
    fftwf_destroy_plan(c->fwd);
    fftwf_destroy_plan(c->inv);
    fftw_unlock();
    fftwf_free(c->fin);
    fftwf_free(c->fout);
    fftwf_free(c->slots);
    fftwf_free(c->zin);
    fftwf_free(c->zout);
    free(c->G);
    free(c);
}

int os_chan_bins_per(const os_chan_t *c, int fft_size) {
    return fft_size > 0 && c->N % fft_size == 0 ? c->N / fft_size : 0;
}

int os_chan_fft_size(const os_chan_t *c) {
    return c->N;
}

void os_chan_stats(const os_chan_t *c, uint64_t *ffts, uint64_t *hits) {
    *ffts = c->n_fft;
    *hits = c->n_hit;
}

/* FFT of block b, from the cache if it's there. A block whose input wasn't
 * all available (start of stream, aged out, not received yet) is computed
 * into fout and not cached, so a later burst can't get the zero-filled one. */
static const float complex *get_block(os_chan_t *c, int64_t b) {
    int slot = (int)(b % OS_CHAN_SLOTS);
    float complex *dst = &c->slots[(size_t)slot * c->N];
    if (c->tag[slot] == b) {
        c->n_hit++;
        return dst;
    }

    int64_t B = b * c->L - c->V;
    int complete;
    if (B >= 0) {
        complete = c->read(c->read_ctx, (uint64_t)B, (size_t)c->N, c->fin);
    } else {
        size_t skip = (size_t)(-B);
        memset(c->fin, 0, sizeof(float complex) * skip);
        c->read(c->read_ctx, 0, (size_t)c->N - skip, c->fin + skip);
        complete = 0;
    }

    if (!complete)
        dst = c->fout;
    fftwf_execute_dft(c->fwd, (fftwf_complex *)c->fin, (fftwf_complex *)dst);
    c->n_fft++;
    if (complete)
        c->tag[slot] = b;
    return dst;
}

int os_chan_extract(os_chan_t *c, uint64_t start, uint64_t stop,
                    int bin_shift, float complex *out, int max_out,
                    uint64_t *delay_ns) {
    if (delay_ns)
        *delay_ns = c->delay_ns;
    if (stop <= start || stop - start < (uint64_t)c->ntaps)
        return 0;
    int64_t n_out = (int64_t)(stop - start - c->ntaps + 1) / c->D;
    if (n_out > max_out)
        n_out = max_out;

    const int D = c->D, M = c->M, N = c->N;
    uint64_t m0 = start + c->ntaps - 1;     /* input index under out[0] */
    int tau = (int)(m0 % D);
    const float complex *G = &c->G[(size_t)tau * M];
    int64_t g0 = (int64_t)((m0 - tau) / D);  /* out[0] on the D-sample grid */
    int shift = ((bin_shift % N) + N) % N;

    int64_t k = 0;
    while (k < n_out) {
        int64_t pos = (g0 + k) * D;
        int64_t b = pos / c->L;
        int jj = (int)((pos - b * c->L) / D) + c->V / D;
        int64_t cnt = M - jj;
        if (cnt > n_out - k)
            cnt = n_out - k;

        const float complex *X = get_block(c, b);

        /* Bins shift - M/2 .. shift + M/2 - 1, laid out for the inverse FFT
         * (non-negative k' first), as two runs around the wrap at N. */
        for (int idx = 0; idx < M; idx++) {
            int kp = idx < M / 2 ? idx : idx - M;
            int src = shift + kp;
            if (src < 0) src += N;
            else if (src >= N) src -= N;
            c->zin[idx] = X[src] * G[idx];
        }
        fftwf_execute(c->inv);

        /* Phase reference: the burst's start, not the block's. */
        int64_t B = b * c->L - c->V;
        int64_t rel = (int64_t)start - B;
        int64_t r = ((rel % N) * (int64_t)shift) % N;   /* exact, mod N */
        double ang = 2.0 * M_PI * (double)r / N;
        float complex ph = (float complex)cexp(I * ang);

        for (int64_t i = 0; i < cnt; i++)
            out[k + i] = c->zout[jj + i] * ph;
        k += cnt;
    }
    return (int)n_out;
}
