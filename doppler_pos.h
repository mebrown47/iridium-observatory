/*
 * Doppler-based receiver positioning from Iridium signals
 *
 * Based on: "New Method for Positioning Using IRIDIUM Satellite Signals
 * of Opportunity" (Tan et al., IEEE Access, 2019)
 *
 * Uses Doppler shift measurements from decoded IRA frames combined with
 * satellite positions to estimate the receiver's geographic location via
 * iterated weighted least squares.
 *
 * Copyright (c) 2026 CEMAXECUTER LLC
 * Modifications Copyright (c) 2026 Mike Brown
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef __DOPPLER_POS_H__
#define __DOPPLER_POS_H__

#include <stdint.h>
#include "frame_decode.h"

/* Position solution output */
typedef struct {
    double lat, lon;        /* estimated position (degrees) */
    double alt;             /* estimated altitude (meters) */
    double hdop;            /* horizontal dilution of precision */
    int n_measurements;     /* total measurements used */
    int n_satellites;       /* distinct satellites used */
    int converged;          /* 1 if solver converged */
} doppler_solution_t;

/* Initialize the positioning engine. Call once at startup. */
void doppler_pos_init(void);

/* Feed a decoded IRA frame into the measurement buffer. Thread-safe. */
void doppler_pos_add_measurement(const ira_data_t *ira, double frequency,
                                  uint64_t timestamp);

/* Attempt to compute a position solution.
 * Returns 1 if a valid solution was produced, 0 otherwise. */
int doppler_pos_solve(doppler_solution_t *out);

/* Forget the previous solution, so the next solve starts from scratch (no
 * warm start, no jump rejection against it): for a final solve that depends
 * only on the measurements. */
void doppler_pos_reset_solution(void);

/* Set assumed receiver height for height aiding (meters above WGS-84).
 * A value of 0 disables height aiding. */
void doppler_pos_set_height(double height_m);
/* Returns recent active satellites (measurement within last 60s) for an
 * overview display. Writes up to max_out entries. Returns count written. */
int doppler_pos_get_active_sats(int *sat_ids, double *latest_freq,
                                  uint64_t *latest_timestamp,
                                  uint64_t now_ns, int max_out);
/* Returns recent frequency/timestamp history for one satellite, oldest
 * first. Writes up to max_out entries. Returns count written, or 0 if
 * sat_id is unknown. */
int doppler_pos_get_history(int sat_id, double *freqs,
                              uint64_t *timestamps, int max_out);

#endif
