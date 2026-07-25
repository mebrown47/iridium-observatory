/*
 * QPSK/DQPSK demodulator - port of gr-iridium's iridium_qpsk_demod
 *
 * Original work Copyright 2020 Free Software Foundation, Inc.
 * Modifications Copyright 2026 CEMAXECUTER LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * QPSK/DQPSK demodulator - port of gr-iridium's iridium_qpsk_demod
 *
 * Pipeline: decimate -> PLL -> hard decision -> UW check ->
 *           DQPSK decode -> symbol-to-bits
 */

#ifndef __QPSK_DEMOD_H__
#define __QPSK_DEMOD_H__

#include <complex.h>
#include <stdint.h>
#include "burst_downmix.h"

/* Demodulated frame output */
typedef struct {
    uint64_t id;
    uint64_t timestamp;         /* nanoseconds */
    double center_frequency;    /* Hz */
    ir_direction_t direction;
    float magnitude;            /* dB */
    float noise;                /* dBFS/Hz */
    int confidence;             /* 0-100% */
    float level;                /* average signal amplitude */
    int n_symbols;              /* total symbols including UW */
    int n_payload_symbols;      /* symbols after UW */
    uint8_t *bits;              /* 2 bits per symbol (0 or 1 each) */
    float *llr;                 /* per-bit reliability (|distance from boundary|) */
    int n_bits;
} demod_frame_t;

/* qpsk_demod() return codes */
#define QPSK_DEMOD_OK       1   /* success: *out set, caller frees bits+frame */
#define QPSK_DEMOD_UW_FAIL  0   /* real burst, unique word did not match       */
#define QPSK_DEMOD_REJECT   2   /* weak sync (no UW present): false-positive    */

/* Demodulate a downmixed frame. Returns one of the QPSK_DEMOD_* codes above.
 * On QPSK_DEMOD_OK the caller owns the returned frame and must free bits and
 * frame. QPSK_DEMOD_REJECT flags a burst whose matched-filter sync score is
 * below uw_reject_threshold -- almost certainly a burst-detector false positive,
 * to be dropped WITHOUT counting as a UW-fail. */
int qpsk_demod(downmix_frame_t *in, demod_frame_t **out);

/* Thread function: pulls from frame_queue, pushes to output_queue */
void *qpsk_demod_thread(void *arg);

#endif
