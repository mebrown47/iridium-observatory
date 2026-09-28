/*
 * Doppler-based receiver positioning from Iridium signals
 *
 * Based on: "New Method for Positioning Using IRIDIUM Satellite Signals
 * of Opportunity" (Tan et al., IEEE Access, 2019)
 *
 * Copyright (c) 2026 CEMAXECUTER LLC
 * Modifications Copyright (c) 2026 Mike Brown
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "doppler_pos.h"
#include "wgs84.h"
#include "gsmtap.h"  /* IR_BASE_FREQ, IR_CHANNEL_WIDTH */

extern int verbose;

/* ---- Configuration ---- */

#define MAX_SATELLITES       128
#define MEAS_PER_SAT         1500    /* measurements kept per satellite (newest by time):
                                        * a whole pass at ~2 IRA/s. At 200 only its last
                                        * ~100 s survived - not the zero crossing and
                                        * curvature that locate the receiver */
#define MIN_MEASUREMENTS     8       /* minimum to attempt a solution */
#define MIN_SATELLITES       2       /* need geometry from multiple passes */
#define MIN_PASS_MEAS        20      /* usable measurements for a satellite to count as a
                                        * pass: stray decodes (1-10 IRAs with corrupted
                                        * positions, velocities and so ~MHz residuals)
                                        * otherwise met MIN_SATELLITES and wrecked the fit */
#define GRID_STEP_DEG        1.0     /* coarse misfit grid for the starting points */
#define GRID_HALF_LAT        25.0
#define GRID_HALF_LON        30.0
#define GRID_MAX_MEAS        600     /* thin the measurements to this many for the grid */
#define N_GRID_STARTS        4       /* separated grid minima tried as starting points */
#define START_SEPARATION     500e3   /* between grid starts (m) */
#define AMBIGUOUS_DIST       100e3   /* a distinct second solution this far away... */
#define AMBIGUOUS_RMS_RATIO  1.25    /* ...fitting within this ratio: no fix (mirror) */
#define IRA_FREQ_WINDOW      100e3   /* Hz around the ring-alert channel: Doppler (+-37
                                        * kHz) plus a cheap oscillator's offset. Frames
                                        * decoded as IRAs MHz away are corrupted, with
                                        * corrupted positions too */
#define PARTNER_TOL_RAD      0.006   /* ~40 km at orbit radius: how far a velocity
                                        * partner may sit from where the orbit puts it */
#define TRACK_WINDOW_S       20.0    /* on_track: neighbours this close in time... */
#define TRACK_TOL_RAD        0.0015  /* ...must agree to ~11 km */
#define ROBUST_K             5.0     /* per iteration, set aside residuals > K robust
                                        * sigmas from the median */
#define MAX_HDOP             1000.0  /* weaker geometry than this: no fix */
#define MAX_ITERATIONS       200     /* WLS iteration limit */
#define CONVERGENCE_M        100.0   /* position correction threshold (m) */
#define OUTLIER_SIGMA        3.0     /* residual rejection threshold */
#define MAX_MEAS_AGE_NS      (30ULL * 60 * 1000000000ULL)  /* 30 min */
#define MIN_VEL_INTERVAL_NS  (2ULL * 1000000000ULL)         /* 2 sec */
#define MAX_SAT_CLUSTER_DIST 8000e3   /* max 3D ECEF distance between visible sats (m) */
#define SAT_GAP_RESET_S      600.0   /* reset sat buffer after 10 min gap (new pass) */
#define MAX_SOLUTION_JUMP    500e3    /* reject solutions >500 km from previous (m) */

/* ---- Internal types ---- */

typedef struct {
    double sat_ecef[3];     /* satellite ECEF position (m) */
    double freq;            /* measured burst frequency (Hz) */
    uint64_t timestamp;     /* nanoseconds */
    int valid;
} sat_meas_t;

typedef struct {
    int sat_id;
    sat_meas_t meas[MEAS_PER_SAT];   /* sorted by timestamp, oldest first */
    int count;              /* total stored (capped at MEAS_PER_SAT) */
    double channel_freq;    /* estimated true channel frequency (0 = unknown) */
} sat_buffer_t;

/* Flattened measurement for the solver */
typedef struct {
    double sat_ecef[3];
    double sat_vel[3];      /* estimated satellite velocity (m/s) */
    double range_rate;      /* measured range rate (m/s) */
    double weight;
    int sat_idx;            /* index into satellites[] for per-sat residual check */
} solver_meas_t;

/* ---- Module state ---- */

static pthread_mutex_t pos_lock;
static sat_buffer_t satellites[MAX_SATELLITES];
static int n_satellites;
static double height_aiding_m = -1.0;  /* -1 = disabled, >= 0 = altitude in meters */
static int height_aiding_enabled;

/* Persistent solution state (reused between solve calls) */
static double prev_ecef[3] = {0, 0, 0};
static double prev_clock_drift = 0;
static int has_prev_solution = 0;

void doppler_pos_reset_solution(void)
{
    pthread_mutex_lock(&pos_lock);
    has_prev_solution = 0;
    prev_clock_drift = 0;
    pthread_mutex_unlock(&pos_lock);
}
static int jump_reject_count = 0;

/* ---- Helpers ---- */

static double vec3_dot(const double a[3], const double b[3])
{
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}

static double vec3_norm(const double v[3])
{
    return sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
}

static void vec3_sub(const double a[3], const double b[3], double out[3])
{
    out[0] = a[0] - b[0];
    out[1] = a[1] - b[1];
    out[2] = a[2] - b[2];
}

static void vec3_cross(const double a[3], const double b[3], double out[3])
{
    out[0] = a[1]*b[2] - a[2]*b[1];
    out[1] = a[2]*b[0] - a[0]*b[2];
    out[2] = a[0]*b[1] - a[1]*b[0];
}

/* Convert IRA-encoded satellite position to ECEF meters.
 * IRA uses 12-bit signed XYZ where 1 unit ~ 4 km of geocentric distance. */
static void ira_xyz_to_ecef(const int pos_xyz[3], double ecef[3])
{
    ecef[0] = (double)pos_xyz[0] * 4000.0;
    ecef[1] = (double)pos_xyz[1] * 4000.0;
    ecef[2] = (double)pos_xyz[2] * 4000.0;
}

/* Find satellite buffer by ID, or allocate a new one */
static sat_buffer_t *find_or_create_sat(int sat_id)
{
    for (int i = 0; i < n_satellites; i++) {
        if (satellites[i].sat_id == sat_id)
            return &satellites[i];
    }
    if (n_satellites >= MAX_SATELLITES)
        return NULL;
    sat_buffer_t *s = &satellites[n_satellites++];
    memset(s, 0, sizeof(*s));
    s->sat_id = sat_id;
    return s;
}

/* Add a measurement, keeping the buffer sorted by timestamp. Frames arrive
 * slightly out of order (the downmix/demod workers run in parallel), so
 * keeping arrival order - as a ring buffer did - made which measurements
 * survive, and so the solution, vary from run to run on the same input.
 * When full, the oldest by time goes (or the new one, if it is older). */
static void sat_buf_add(sat_buffer_t *s, const double ecef[3],
                         double freq, uint64_t ts)
{
    if (s->count == MEAS_PER_SAT) {
        if (ts < s->meas[0].timestamp)
            return;
        memmove(&s->meas[0], &s->meas[1], sizeof(sat_meas_t) * (MEAS_PER_SAT - 1));
        s->count--;
    }
    int k = s->count;
    while (k > 0 && s->meas[k - 1].timestamp > ts) {
        s->meas[k] = s->meas[k - 1];
        k--;
    }
    sat_meas_t *m = &s->meas[k];
    m->sat_ecef[0] = ecef[0];
    m->sat_ecef[1] = ecef[1];
    m->sat_ecef[2] = ecef[2];
    m->freq = freq;
    m->timestamp = ts;
    m->valid = 1;
    s->count++;
}

/* Get measurement by index (0 = oldest by time) */
static sat_meas_t *sat_buf_get(sat_buffer_t *s, int idx)
{
    if (idx < 0 || idx >= s->count) return NULL;
    return &s->meas[idx];
}

/* The frequency a satellite's measurements were transmitted on. Every
 * measurement comes from a ring alert (IRA), and ring alerts are always sent
 * on the ring-alert channel: channel 246 ("S.07"), whose center is half a
 * channel into it, 1626.270833 MHz (iridium-toolkit util.py numbering).
 * Checked on a 20 min recording: 905 of 905 IRA frames with satellite
 * positions were on it (Doppler from TLEs).
 *
 * This used to vote over nearest channel centers at IR_BASE_FREQ +
 * k * IR_CHANNEL_WIDTH, which are half a channel off the real centers: every
 * satellite came out +-20.8 kHz (+-3.84 km/s of range rate) wrong, with the
 * sign varying by satellite, so the common clock term could not absorb it,
 * and positions landed ~1500 km off. Voting on the corrected raster still
 * fails for passes spent mostly past +-20.8 kHz of Doppler. */
#define IR_RING_ALERT_FREQ (IR_BASE_FREQ + (246 + 0.5) * IR_CHANNEL_WIDTH)

static double estimate_channel_freq(sat_buffer_t *s, uint64_t now)
{
    (void)s;
    (void)now;
    return IR_RING_ALERT_FREQ;
}

/* Estimate satellite velocity using orbital mechanics.
 *
 * Position differencing gives ~7-15 degree direction error due to 4 km IRA
 * quantization over short baselines. Instead, determine the orbital plane from
 * the cross product of two well-separated position vectors (h = r1 x r2), then
 * compute velocity as h x r / |h x r| (perpendicular to both the orbital pole
 * and current position -- geometrically exact for a circular orbit).
 *
 * Two positions separated by 5 minutes span ~2200 km of arc, so the orbital
 * plane is determined with sub-degree accuracy despite 4 km quantization.
 * Speed magnitude comes from the vis-viva equation. */
/* Is `m` where the satellite at `cur` would be after their time
 * difference? The angle between the two positions (the other one rotated
 * into cur's axes, undoing the Earth's turn) must match the orbital rate.
 * A corrupted IRA carrying a real pass's sat_id failed this by 70-2000 km;
 * picked as the partner, it gave the whole pass a wrong plane and wrong
 * velocities. */
static int on_orbit_within(const sat_meas_t *cur, const sat_meas_t *m, double tol)
{
    double dt = ((double)m->timestamp - (double)cur->timestamp) / 1e9;
    double a = OMEGA_EARTH * dt, ca = cos(a), sa = sin(a);
    double o[3] = { ca * m->sat_ecef[0] - sa * m->sat_ecef[1],
                    sa * m->sat_ecef[0] + ca * m->sat_ecef[1],
                    m->sat_ecef[2] };
    double r1 = vec3_norm(cur->sat_ecef), r2 = vec3_norm(o);
    double c = vec3_dot(cur->sat_ecef, o) / (r1 * r2);
    double angle = acos(c > 1 ? 1 : c < -1 ? -1 : c);
    double rate = sqrt(GM_EARTH / (r1 * r1 * r1));
    return fabs(angle - rate * fabs(dt)) < tol;
}

static int partner_on_orbit(const sat_meas_t *cur, const sat_meas_t *m)
{
    return on_orbit_within(cur, m, PARTNER_TOL_RAD);
}

/* Is measurement idx on its satellite's track? Some measurement 2-20 s
 * away must agree with it to ~11 km. Checking against the partner alone is
 * not enough: an angle test barely sees a cross-track error at a long
 * separation (off by d, the angle changes by ~d^2 / (2 x arc)) - minutes
 * apart, points ~300-600 km off-track passed. 20 s apart it is ~57 km. */
static int on_track(sat_buffer_t *s, int idx)
{
    const sat_meas_t *cur = sat_buf_get(s, idx);
    for (int dir = -1; dir <= 1; dir += 2) {
        for (int i = idx + dir; i >= 0 && i < s->count; i += dir) {
            const sat_meas_t *m = sat_buf_get(s, i);
            if (!m || !m->valid) continue;
            double dt = fabs(((double)m->timestamp - (double)cur->timestamp) / 1e9);
            if (dt > TRACK_WINDOW_S) break;
            if (dt < MIN_VEL_INTERVAL_NS / 1e9) continue;
            if (on_orbit_within(cur, m, TRACK_TOL_RAD)) return 1;
        }
    }
    return 0;
}

static int estimate_velocity(sat_buffer_t *s, int idx, double vel[3])
{
    sat_meas_t *cur = sat_buf_get(s, idx);
    if (!cur) return -1;

    double r_norm = vec3_norm(cur->sat_ecef);
    if (r_norm < 1e6) return -1;
    if (!on_track(s, idx)) return -1;       /* a corrupted position */

    /* Find the most temporally separated measurement for best orbital plane
     * accuracy. Prefer larger separation (more arc = less quantization noise).
     * Cap at 10 minutes to avoid orbital perturbation drift. */
    sat_meas_t *best_other = NULL;
    double best_dt = 0;

    /* The buffer is sorted by time, so the qualifying measurement farthest
     * away on each side is the first one met scanning in from that end. */
    for (int i = 0; i < idx; i++) {
        sat_meas_t *m = sat_buf_get(s, i);
        if (!m || !m->valid) continue;
        double dt = (double)(cur->timestamp - m->timestamp) / 1e9;
        if (dt >= 600.0) continue;
        if (dt < MIN_VEL_INTERVAL_NS / 1e9) break;
        double other_r = vec3_norm(m->sat_ecef);
        if (other_r < 7050e3 || other_r > 7250e3) continue;
        if (!partner_on_orbit(cur, m) || !on_track(s, i)) continue;
        best_dt = dt;
        best_other = m;
        break;
    }
    for (int i = s->count - 1; i > idx; i--) {
        sat_meas_t *m = sat_buf_get(s, i);
        if (!m || !m->valid) continue;
        double dt = (double)(m->timestamp - cur->timestamp) / 1e9;
        if (dt >= 600.0) continue;
        if (dt < MIN_VEL_INTERVAL_NS / 1e9) break;
        double other_r = vec3_norm(m->sat_ecef);
        if (other_r < 7050e3 || other_r > 7250e3) continue;
        if (!partner_on_orbit(cur, m) || !on_track(s, i)) continue;
        if (dt > best_dt) {
            best_dt = dt;
            best_other = m;
        }
        break;
    }
    if (!best_other) return -1;

    /* Orbital angular momentum vector: h = r_cur x r_other
     * Defines the orbital plane normal. Direction depends on which
     * position is "first" but we fix the sign below using temporal order. */
    /* Both positions are Earth-fixed, but the orbit is a plane in inertial
     * space: express the other position in the current epoch's axes by
     * undoing the Earth's rotation over the interval (2.5 deg in 10 min,
     * ~300 km at orbit radius). The plane - and the velocity - are then the
     * inertial ones the range-rate model (sat_vel - omega x r_rx) assumes.
     * Fitting the Earth-fixed positions directly put the velocity off by
     * ~185 m/s at 1-3 min separation (4 m/s with this correction). */
    double other[3];
    {
        double dt_s = ((double)best_other->timestamp - (double)cur->timestamp) / 1e9;
        double a = OMEGA_EARTH * dt_s, ca = cos(a), sa = sin(a);
        other[0] = ca * best_other->sat_ecef[0] - sa * best_other->sat_ecef[1];
        other[1] = sa * best_other->sat_ecef[0] + ca * best_other->sat_ecef[1];
        other[2] = best_other->sat_ecef[2];
    }
    double h[3];
    vec3_cross(cur->sat_ecef, other, h);
    double h_norm = vec3_norm(h);
    if (h_norm < 1e6) return -1;  /* nearly collinear positions */

    /* Velocity direction: v = h x r / |h x r|
     * Perpendicular to both orbital pole (h) and position (r),
     * lying in the orbital plane. This is exact for circular orbits. */
    double v_dir[3];
    vec3_cross(h, cur->sat_ecef, v_dir);
    double v_norm = vec3_norm(v_dir);
    if (v_norm < 1.0) return -1;

    /* Determine sign from temporal ordering: the satellite should move
     * in the v_dir direction over time. Use the displacement vector
     * from the earlier to later position to check. */
    double forward[3];
    if (best_other->timestamp > cur->timestamp)
        vec3_sub(other, cur->sat_ecef, forward);
    else
        vec3_sub(cur->sat_ecef, other, forward);

    double sign = (vec3_dot(v_dir, forward) >= 0) ? 1.0 : -1.0;

    /* Speed from vis-viva equation (circular orbit approximation) */
    double speed = sqrt(GM_EARTH / r_norm);

    vel[0] = sign * speed * v_dir[0] / v_norm;
    vel[1] = sign * speed * v_dir[1] / v_norm;
    vel[2] = sign * speed * v_dir[2] / v_norm;
    return 0;
}

/* 4x4 matrix inversion via Gauss-Jordan elimination.
 * Operates in-place on A[4][4], stores inverse in inv[4][4].
 * Returns 0 on success, -1 if singular. */
static int mat4_invert(double A[4][4], double inv[4][4])
{
    /* Initialize inv to identity */
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            inv[i][j] = (i == j) ? 1.0 : 0.0;

    for (int col = 0; col < 4; col++) {
        /* Partial pivoting */
        int pivot = col;
        double max_val = fabs(A[col][col]);
        for (int row = col + 1; row < 4; row++) {
            if (fabs(A[row][col]) > max_val) {
                max_val = fabs(A[row][col]);
                pivot = row;
            }
        }
        if (max_val < 1e-30) return -1;

        if (pivot != col) {
            for (int j = 0; j < 4; j++) {
                double tmp;
                tmp = A[col][j]; A[col][j] = A[pivot][j]; A[pivot][j] = tmp;
                tmp = inv[col][j]; inv[col][j] = inv[pivot][j]; inv[pivot][j] = tmp;
            }
        }

        double diag = A[col][col];
        for (int j = 0; j < 4; j++) {
            A[col][j] /= diag;
            inv[col][j] /= diag;
        }

        for (int row = 0; row < 4; row++) {
            if (row == col) continue;
            double factor = A[row][col];
            for (int j = 0; j < 4; j++) {
                A[row][j] -= factor * A[col][j];
                inv[row][j] -= factor * inv[col][j];
            }
        }
    }
    return 0;
}

/* ---- Public API ---- */

void doppler_pos_init(void)
{
    pthread_mutex_init(&pos_lock, NULL);
    n_satellites = 0;
    height_aiding_m = -1.0;
    height_aiding_enabled = 0;
    memset(satellites, 0, sizeof(satellites));
}

void doppler_pos_set_height(double height_m)
{
    height_aiding_m = height_m;
    height_aiding_enabled = 1;
}

void doppler_pos_add_measurement(const ira_data_t *ira, double frequency,
                                  uint64_t timestamp)
{
    static unsigned long dbg_total = 0, dbg_sat0 = 0, dbg_coord = 0,
                         dbg_radius = 0, dbg_ok = 0, dbg_vel_rej = 0;

    dbg_total++;

    if (ira->sat_id == 0) { dbg_sat0++; goto dbg_print; }
    if (fabs(frequency - IR_RING_ALERT_FREQ) > IRA_FREQ_WINDOW) {
        dbg_coord++;
        goto dbg_print;
    }
    if (ira->lat < -90 || ira->lat > 90) { dbg_coord++; goto dbg_print; }
    if (ira->lon < -180 || ira->lon > 180) { dbg_coord++; goto dbg_print; }

    /* Convert IRA satellite position to ECEF */
    double sat_ecef[3];
    ira_xyz_to_ecef(ira->pos_xyz, sat_ecef);

    /* Sanity: Iridium orbit radius ~7158 km (780 km altitude).
     * Accept 7050-7250 km (altitude 672-872 km) to reject false positives. */
    double r = vec3_norm(sat_ecef);
    if (r < 7050e3 || r > 7250e3) {
        dbg_radius++;
        goto dbg_print;
    }

    pthread_mutex_lock(&pos_lock);
    sat_buffer_t *s = find_or_create_sat(ira->sat_id);
    if (s) {
        if (s->count > 0) {
            /* compare with the measurement just before it in time (or the
             * first one, if it is the oldest) - not the last to arrive */
            int last = s->count - 1;
            while (last > 0 && s->meas[last].timestamp > timestamp)
                last--;
            double dt = ((double)timestamp - (double)s->meas[last].timestamp) / 1e9;

            /* Long gap: likely a different physical satellite reusing
             * this 7-bit sat_id. Reset the buffer to avoid mixing
             * measurements from different orbital planes. */
            if (dt > SAT_GAP_RESET_S) {
                if (verbose)
                    fprintf(stderr, "DOPPLER: sat=%d gap=%.0fs, "
                            "resetting buffer (likely new pass)\n",
                            ira->sat_id, dt);
                s->count = 0;
                s->channel_freq = 0;
            }
            else {
                /* Short gap: verify position consistency (> 10 km/s is
                 * impossible, allowing 14 km for two positions each rounded
                 * to the 4 km grid) - against any of the last 3 measurements
                 * before it within 2 min, not just the last: when that one
                 * was a corrupted IRA, checking it alone rejected every
                 * real measurement after it for 2 min. */
                int checked = 0, consistent = 0;
                for (int j = last; j >= 0 && j > last - 3; j--) {
                    double dtj = fabs(((double)timestamp - (double)s->meas[j].timestamp) / 1e9);
                    if (dtj == 0 || dtj >= 120) continue;
                    double dx = sat_ecef[0] - s->meas[j].sat_ecef[0];
                    double dy = sat_ecef[1] - s->meas[j].sat_ecef[1];
                    double dz = sat_ecef[2] - s->meas[j].sat_ecef[2];
                    checked++;
                    if (sqrt(dx*dx + dy*dy + dz*dz) - 14000.0 <= 10000.0 * dtj) {
                        consistent = 1;
                        break;
                    }
                }
                if (checked && !consistent) {
                    dbg_vel_rej++;
                    pthread_mutex_unlock(&pos_lock);
                    goto dbg_print;
                }
            }
        }
        sat_buf_add(s, sat_ecef, frequency, timestamp);
    }
    pthread_mutex_unlock(&pos_lock);

    if (verbose) {
        double slat, slon, salt;
        ecef_to_geodetic(sat_ecef, &slat, &slon, &salt);
        fprintf(stderr, "DOPPLER: accepted sat=%d pos=%.1f,%.1f "
                "alt=%.0fkm freq=%.0f\n",
                ira->sat_id, slat, slon, salt/1000.0, frequency);
    }
    dbg_ok++;

dbg_print:
    if (verbose && dbg_total % 50 == 0)
        fprintf(stderr, "DOPPLER: ira_total=%lu ok=%lu "
                "reject_sat0=%lu reject_coord=%lu reject_radius=%lu "
                "reject_vel=%lu\n",
                dbg_total, dbg_ok, dbg_sat0, dbg_coord, dbg_radius,
                dbg_vel_rej);
}

/* ---- Solver ---- */

/* Predicted range rate for one measurement from receiver position rx (the
 * receiver turning with the Earth) and clock term clk, with its partials
 * with respect to [rx, clk] in H_row. */
static double predict_range_rate(const solver_meas_t *m, const double rx[3],
                                 double clk, double H_row[4])
{
    double rx_vel[3] = { -OMEGA_EARTH * rx[1], OMEGA_EARTH * rx[0], 0.0 };
    double los[3];
    vec3_sub(m->sat_ecef, rx, los);
    double rho = vec3_norm(los);
    if (rho < 1.0) return NAN;
    double rel_vel[3] = { m->sat_vel[0] - rx_vel[0],
                          m->sat_vel[1] - rx_vel[1],
                          m->sat_vel[2] - rx_vel[2] };
    double rho_dot_geom = vec3_dot(los, rel_vel) / rho;
    if (H_row) {
        /* d(rho_dot)/d(rx_i) = -(v_rel_i)/rho + los_i * rho_dot_geom / rho^2
         *                      + los . d(-v_rx)/d(rx_i) / rho
         * where d(v_rx_x)/d(rx_y) = -omega, d(v_rx_y)/d(rx_x) = omega */
        double rho2 = rho * rho;
        H_row[0] = -rel_vel[0] / rho + los[0] * rho_dot_geom / rho2
                    + OMEGA_EARTH * los[1] / rho;
        H_row[1] = -rel_vel[1] / rho + los[1] * rho_dot_geom / rho2
                    - OMEGA_EARTH * los[0] / rho;
        H_row[2] = -rel_vel[2] / rho + los[2] * rho_dot_geom / rho2;
        H_row[3] = 1.0;  /* clock drift */
    }
    return rho_dot_geom + clk;
}

/* Iterated weighted least squares (plain Gauss-Newton) over the measurements
 * with weight > 0, from rx/clk, which it updates. State: [x, y, z, clock
 * drift], with height aiding when enabled. Returns 1 if it converged.
 *
 * No Levenberg-Marquardt damping (a tiny ridge only keeps the inverse
 * finite): with it each step covered a small fraction of the way, the steps
 * shrank below CONVERGENCE_M within a few km, and the solve "converged" next
 * to its initial guess. The 500 km step limit guards the first steps. */
static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static int gauss_newton(solver_meas_t *meas, int n, double rx[3], double *clk)
{
    static double res[MAX_SATELLITES * MEAS_PER_SAT];
    static double tmp[MAX_SATELLITES * MEAS_PER_SAT];
    for (int iter = 0; iter < MAX_ITERATIONS; iter++) {
        double HtWH[4][4] = {{0}};
        double HtWy[4] = {0};

        /* Robust mask for this step: a few wild measurements (a corrupted
         * position that slipped through gives ~1 km/s) must not outweigh
         * hundreds of good ones (~8 m/s) in plain least squares. */
        int k = 0;
        for (int i = 0; i < n; i++) {
            res[i] = NAN;
            if (meas[i].weight == 0) continue;
            double pred = predict_range_rate(&meas[i], rx, *clk, NULL);
            if (isnan(pred)) continue;
            res[i] = meas[i].range_rate - pred;
            tmp[k++] = res[i];
        }
        if (k < MIN_MEASUREMENTS) return 0;
        qsort(tmp, k, sizeof(double), cmp_double);
        double med = tmp[k / 2];
        for (int i = 0; i < k; i++) tmp[i] = fabs(tmp[i] - med);
        qsort(tmp, k, sizeof(double), cmp_double);
        double cut = ROBUST_K * 1.4826 * tmp[k / 2];
        if (cut < 5.0) cut = 5.0;              /* m/s */

        for (int i = 0; i < n; i++) {
            solver_meas_t *m = &meas[i];
            if (m->weight == 0 || isnan(res[i]) || fabs(res[i] - med) > cut) continue;
            double H_row[4] = {0};
            double pred = predict_range_rate(m, rx, *clk, H_row);
            double dy = m->range_rate - pred;
            for (int r = 0; r < 4; r++) {
                for (int c = 0; c < 4; c++)
                    HtWH[r][c] += H_row[r] * m->weight * H_row[c];
                HtWy[r] += H_row[r] * m->weight * dy;
            }
        }

        /* Height aiding: constrain geodetic altitude to height_aiding_m.
         * Uses geodetic altitude (not ECEF radius) because WGS-84 surface
         * radius varies by ~21 km with latitude. The radial unit vector
         * approximates d(altitude)/d(ecef). */
        if (height_aiding_enabled) {
            double r0 = vec3_norm(rx);
            if (r0 > 0) {
                double hlat, hlon, halt;
                ecef_to_geodetic(rx, &hlat, &hlon, &halt);
                double dy_h = height_aiding_m - halt;
                double H_h[4] = { rx[0] / r0, rx[1] / r0, rx[2] / r0, 0.0 };
                double w_h = 100.0;   /* height constraint weighted heavily */
                for (int r = 0; r < 4; r++) {
                    for (int c = 0; c < 4; c++)
                        HtWH[r][c] += H_h[r] * w_h * H_h[c];
                    HtWy[r] += H_h[r] * w_h * dy_h;
                }
            }
        }

        for (int i = 0; i < 4; i++)
            HtWH[i][i] += 1e-6;

        double inv[4][4];
        if (mat4_invert(HtWH, inv) != 0)
            return 0;

        double delta[4] = {0};
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
                delta[i] += inv[i][j] * HtWy[j];

        double step = sqrt(delta[0]*delta[0] + delta[1]*delta[1] +
                           delta[2]*delta[2]);
        double max_step = 500000.0;  /* 500 km max step */
        if (step > max_step) {
            double scale = max_step / step;
            for (int i = 0; i < 4; i++)
                delta[i] *= scale;
            step = max_step;
        }

        rx[0] += delta[0];
        rx[1] += delta[1];
        rx[2] += delta[2];
        *clk += delta[3];

        if (step < CONVERGENCE_M)
            return 1;
    }
    return 0;
}

/* RMS range-rate residual (m/s) over the measurements with weight > 0. */
static double rms_residual(const solver_meas_t *meas, int n, const double rx[3],
                           double clk, int *n_used)
{
    double sum = 0;
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (meas[i].weight == 0) continue;
        double pred = predict_range_rate(&meas[i], rx, clk, NULL);
        if (isnan(pred)) continue;
        double res = meas[i].range_rate - pred;
        sum += res * res;
        k++;
    }
    if (n_used) *n_used = k;
    return k ? sqrt(sum / k) : INFINITY;
}

/* Distinct satellites among the measurements with weight > 0. */
static int count_sats(const solver_meas_t *meas, int n)
{
    int seen[MAX_SATELLITES] = {0}, k = 0;
    for (int i = 0; i < n; i++)
        if (meas[i].weight > 0 && meas[i].sat_idx >= 0 &&
            meas[i].sat_idx < MAX_SATELLITES && !seen[meas[i].sat_idx]) {
            seen[meas[i].sat_idx] = 1;
            k++;
        }
    return k;
}

/* The full solve from one starting point: Gauss-Newton on every
 * measurement, then again without the 3-sigma outliers, then again without
 * any satellite whose mean residual is > 3x the median satellite's (a wrong
 * channel or corrupted positions). Resets the weights first. Returns 1 with
 * rx, clk and the weights of the result, 0 if a stage failed. */
static int refine(solver_meas_t *meas, int n, double rx[3], double *clk)
{
    for (int i = 0; i < n; i++)
        meas[i].weight = 1.0;
    if (!gauss_newton(meas, n, rx, clk))
        return 0;

    /* Outlier rejection */
    int n_valid;
    double sigma = rms_residual(meas, n, rx, *clk, &n_valid);
    if (n_valid > 4) {
        sigma *= sqrt((double)n_valid / (n_valid - 4));
        int rejected = 0;
        for (int i = 0; i < n; i++) {
            double pred = predict_range_rate(&meas[i], rx, *clk, NULL);
            if (isnan(pred) || fabs(meas[i].range_rate - pred) > OUTLIER_SIGMA * sigma) {
                meas[i].weight = 0;
                rejected++;
            }
        }
        if (rejected > 0 && n_valid - rejected >= MIN_MEASUREMENTS &&
            !gauss_newton(meas, n, rx, clk))
            return 0;
    }

    /* Per-satellite residual screening */
    double sat_res_sum[MAX_SATELLITES] = {0};
    int sat_res_count[MAX_SATELLITES] = {0};
    for (int i = 0; i < n; i++) {
        solver_meas_t *m = &meas[i];
        if (m->weight == 0 || m->sat_idx < 0 || m->sat_idx >= MAX_SATELLITES) continue;
        double pred = predict_range_rate(m, rx, *clk, NULL);
        if (isnan(pred)) continue;
        sat_res_sum[m->sat_idx] += fabs(m->range_rate - pred);
        sat_res_count[m->sat_idx]++;
    }
    double sorted_res[MAX_SATELLITES];
    int n_active = 0;
    for (int s = 0; s < MAX_SATELLITES; s++)
        if (sat_res_count[s]) {
            double v = sat_res_sum[s] / sat_res_count[s];
            int j = n_active++;
            while (j > 0 && sorted_res[j - 1] > v) { sorted_res[j] = sorted_res[j - 1]; j--; }
            sorted_res[j] = v;
        }
    if (n_active >= 3) {
        double median_res = sorted_res[n_active / 2];
        int sat_dropped = 0;
        for (int s = 0; s < MAX_SATELLITES; s++) {
            if (!sat_res_count[s] || median_res <= 0) continue;
            double mean_res = sat_res_sum[s] / sat_res_count[s];
            if (mean_res <= 3.0 * median_res) continue;
            if (verbose)
                fprintf(stderr, "DOPPLER: dropping sat_idx=%d (sat_id=%d) "
                        "residual=%.1f vs median=%.1f\n", s,
                        satellites[s].sat_id, mean_res, median_res);
            for (int i = 0; i < n; i++)
                if (meas[i].sat_idx == s)
                    meas[i].weight = 0;
            sat_dropped++;
        }
        if (sat_dropped > 0) {
            int remaining;
            rms_residual(meas, n, rx, *clk, &remaining);
            if (remaining < MIN_MEASUREMENTS || count_sats(meas, n) < MIN_SATELLITES)
                return 0;
            if (!gauss_newton(meas, n, rx, clk))
                return 0;
        }
    }
    return 1;
}

/* Starting points: the minima of the misfit on a coarse grid around the
 * satellites' mean sub-satellite point (the clock term at each point being
 * the mean residual). Doppler from a single pass fits the receiver's mirror
 * image across the ground track as well as the receiver, and a solve from
 * one start (the last fix, or the sub-satellite point) settles in whichever
 * basin it starts in - which put fixes ~750-2200 km away, and kept them
 * there. Writes up to max_starts ECEF points, the best first, at least
 * START_SEPARATION apart. Returns the count. */
static int grid_starts(const solver_meas_t *meas, int n, double starts[][3],
                       int max_starts)
{
    enum { NLAT = (int)(2 * GRID_HALF_LAT / GRID_STEP_DEG) + 1,
           NLON = (int)(2 * GRID_HALF_LON / GRID_STEP_DEG) + 1 };
    static double cost[NLAT * NLON];
    static double pts[NLAT * NLON][3];
    double h = height_aiding_enabled ? height_aiding_m : 0.0;

    double c[3] = {0, 0, 0};
    for (int i = 0; i < n; i++) {
        double r = vec3_norm(meas[i].sat_ecef);
        for (int k = 0; k < 3; k++)
            c[k] += meas[i].sat_ecef[k] / r;
    }
    double lat0 = atan2(c[2], sqrt(c[0]*c[0] + c[1]*c[1])) * 180.0 / M_PI;
    double lon0 = atan2(c[1], c[0]) * 180.0 / M_PI;
    int stride = n / GRID_MAX_MEAS + 1;

    int np = 0;
    for (int a = 0; a < NLAT; a++) {
        double lat = lat0 - GRID_HALF_LAT + a * GRID_STEP_DEG;
        if (lat < -89.0 || lat > 89.0) continue;
        for (int b = 0; b < NLON; b++) {
            double lon = lon0 - GRID_HALF_LON + b * GRID_STEP_DEG;
            geodetic_to_ecef(lat, lon, h, pts[np]);
            double sum = 0, sum2 = 0;
            int k = 0;
            for (int i = 0; i < n; i += stride) {
                double pred = predict_range_rate(&meas[i], pts[np], 0.0, NULL);
                if (isnan(pred)) continue;
                double res = meas[i].range_rate - pred;
                sum += res;
                sum2 += res * res;
                k++;
            }
            cost[np++] = k ? sum2 / k - (sum / k) * (sum / k) : INFINITY;
        }
    }

    int ns = 0;
    while (ns < max_starts) {
        int best = -1;
        for (int p = 0; p < np; p++) {
            if (best >= 0 && cost[p] >= cost[best]) continue;
            int far = 1;
            for (int s = 0; s < ns && far; s++) {
                double d[3];
                vec3_sub(pts[p], starts[s], d);
                if (vec3_norm(d) < START_SEPARATION) far = 0;
            }
            if (far && isfinite(cost[p])) best = p;
        }
        if (best < 0) break;
        memcpy(starts[ns++], pts[best], sizeof(pts[best]));
    }
    return ns;
}

int doppler_pos_solve(doppler_solution_t *out)
{
    memset(out, 0, sizeof(*out));

    pthread_mutex_lock(&pos_lock);

    /* Collect valid measurements with velocity estimates */
    static solver_meas_t all_meas[MAX_SATELLITES * MEAS_PER_SAT];
    int n_meas = 0;
    int sats_used = 0;
    uint64_t now = 0;

    /* Find latest timestamp as reference */
    for (int s = 0; s < n_satellites; s++) {
        for (int i = 0; i < satellites[s].count; i++) {
            sat_meas_t *m = sat_buf_get(&satellites[s], i);
            if (m && m->valid && m->timestamp > now)
                now = m->timestamp;
        }
    }

    /* Spatial visibility filter: cluster satellites by proximity.
     * All satellites visible from a single receiver are within ~6000 km
     * ground distance, which corresponds to ~8000 km in 3D ECEF at orbit
     * altitude. Corrupted IRA positions (valid altitude, wrong lat/lon)
     * place satellites on the opposite side of the planet (>12000 km away).
     *
     * Key insight: only consider satellites with demonstrated orbital motion
     * (valid velocity estimates). Real satellites move at ~7.5 km/s so their
     * positions change between IRA frames. Corrupted decodes tend to report
     * the same wrong position repeatedly (zero apparent motion), which fails
     * the velocity estimation. This filters them before clustering. */
    int sat_keep[MAX_SATELLITES] = {0};
    {
        double sat_pos[MAX_SATELLITES][3];
        int sat_has_motion[MAX_SATELLITES] = {0};
        int n_with_motion = 0;
        int vel_usable[MAX_SATELLITES] = {0};  /* count of velocity-valid meas */

        for (int s = 0; s < n_satellites; s++) {
            if (satellites[s].count < 2) continue;

            /* Check if this satellite has any measurement with a valid
             * velocity estimate (proves it's actually moving in orbit) */
            int latest_vel_idx = -1;
            for (int i = satellites[s].count - 1; i >= 0; i--) {
                sat_meas_t *m = sat_buf_get(&satellites[s], i);
                if (!m || !m->valid) continue;
                if (now > 0 && now - m->timestamp > MAX_MEAS_AGE_NS) continue;
                double vel[3];
                if (estimate_velocity(&satellites[s], i, vel) == 0) {
                    vel_usable[s]++;
                    if (latest_vel_idx < 0) latest_vel_idx = i;
                }
            }

            if (latest_vel_idx >= 0) {
                sat_meas_t *m = sat_buf_get(&satellites[s], latest_vel_idx);
                sat_pos[s][0] = m->sat_ecef[0];
                sat_pos[s][1] = m->sat_ecef[1];
                sat_pos[s][2] = m->sat_ecef[2];
                sat_has_motion[s] = 1;
                n_with_motion++;
            }
        }

        if (n_with_motion >= 3) {
            /* Count mutual neighbors among motion-validated satellites */
            int neighbors[MAX_SATELLITES] = {0};
            for (int i = 0; i < n_satellites; i++) {
                if (!sat_has_motion[i]) continue;
                for (int j = i + 1; j < n_satellites; j++) {
                    if (!sat_has_motion[j]) continue;
                    double d[3];
                    vec3_sub(sat_pos[i], sat_pos[j], d);
                    if (vec3_norm(d) < MAX_SAT_CLUSTER_DIST) {
                        neighbors[i]++;
                        neighbors[j]++;
                    }
                }
            }

            /* Find cluster core: most neighbors, break ties by number
             * of velocity-usable measurements (not raw buffer count) */
            int core = -1;
            int max_nb = -1;
            int max_vel = -1;
            for (int s = 0; s < n_satellites; s++) {
                if (!sat_has_motion[s]) continue;
                if (neighbors[s] > max_nb ||
                    (neighbors[s] == max_nb &&
                     vel_usable[s] > max_vel)) {
                    max_nb = neighbors[s];
                    max_vel = vel_usable[s];
                    core = s;
                }
            }

            /* Keep all motion-validated satellites within threshold of core.
             * Also keep satellites without motion proof if they have recent
             * measurements (newcomers that haven't accumulated enough for
             * velocity yet) and are near the cluster. */
            if (core >= 0) {
                sat_keep[core] = 1;
                for (int s = 0; s < n_satellites; s++) {
                    if (s == core) continue;
                    if (!sat_has_motion[s]) {
                        /* Newcomer: get its latest position and check distance */
                        for (int i = satellites[s].count - 1; i >= 0; i--) {
                            sat_meas_t *m = sat_buf_get(&satellites[s], i);
                            if (!m || !m->valid) continue;
                            if (now > 0 && now - m->timestamp > MAX_MEAS_AGE_NS)
                                continue;
                            double d[3];
                            vec3_sub(m->sat_ecef, sat_pos[core], d);
                            if (vec3_norm(d) < MAX_SAT_CLUSTER_DIST)
                                sat_keep[s] = 1;
                            break;
                        }
                        continue;
                    }
                    double d[3];
                    vec3_sub(sat_pos[s], sat_pos[core], d);
                    double dist = vec3_norm(d);
                    if (dist < MAX_SAT_CLUSTER_DIST) {
                        sat_keep[s] = 1;
                    } else if (verbose) {
                        double slat, slon, salt;
                        ecef_to_geodetic(sat_pos[s], &slat, &slon, &salt);
                        fprintf(stderr, "DOPPLER: visibility reject "
                                "sat=%d pos=%.1f,%.1f (%.0fkm from core "
                                "sat=%d)\n", satellites[s].sat_id, slat, slon,
                                dist / 1000.0, satellites[core].sat_id);
                    }
                }
            }
        } else {
            /* Not enough motion-validated satellites to cluster;
             * keep all that have recent measurements */
            for (int s = 0; s < n_satellites; s++) {
                if (satellites[s].count == 0) continue;
                for (int i = satellites[s].count - 1; i >= 0; i--) {
                    sat_meas_t *m = sat_buf_get(&satellites[s], i);
                    if (m && m->valid &&
                        (now == 0 || now - m->timestamp <= MAX_MEAS_AGE_NS)) {
                        sat_keep[s] = 1;
                        break;
                    }
                }
            }
        }
    }

    for (int s = 0; s < n_satellites; s++) {
        if (!sat_keep[s]) continue;
        int sat_contributed = 0;

        /* Determine the true channel frequency for this satellite.
         * Uses the measurement with smallest offset from nearest channel
         * center (smallest Doppler = most reliable channel assignment). */
        double sat_chan_freq = estimate_channel_freq(&satellites[s], now);
        if (sat_chan_freq == 0) continue;
        int first_meas = n_meas;

        for (int i = 0; i < satellites[s].count; i++) {
            sat_meas_t *m = sat_buf_get(&satellites[s], i);
            if (!m || !m->valid) continue;

            /* Skip old measurements */
            if (now - m->timestamp > MAX_MEAS_AGE_NS) continue;

            /* Estimate satellite velocity */
            double vel[3];
            if (estimate_velocity(&satellites[s], i, vel) != 0)
                continue;

            /* Compute Doppler and convert to range rate.
             * Use per-satellite channel frequency (not per-measurement) to
             * avoid wrong channel assignment when Doppler > half channel width.
             * Use actual channel wavelength (not nominal 1626 MHz) for
             * accurate range-rate conversion across the Iridium band. */
            double f_doppler = m->freq - sat_chan_freq;
            double chan_lambda = C_LIGHT / sat_chan_freq;
            double range_rate = -chan_lambda * f_doppler;

            solver_meas_t *sm = &all_meas[n_meas];
            memcpy(sm->sat_ecef, m->sat_ecef, sizeof(sm->sat_ecef));
            memcpy(sm->sat_vel, vel, sizeof(sm->sat_vel));
            sm->range_rate = range_rate;
            sm->weight = 1.0;
            sm->sat_idx = s;
            n_meas++;
            sat_contributed = 1;

            if (n_meas >= MAX_SATELLITES * MEAS_PER_SAT)
                goto done_collect;
        }

        if (n_meas - first_meas < MIN_PASS_MEAS) {
            n_meas = first_meas;          /* a stray decode, not a pass */
            sat_contributed = 0;
        }

        if (sat_contributed)
            sats_used++;
    }
done_collect:

    /* Debug: show buffer state vs usable measurements */
    if (verbose) {
        int total_buf = 0;
        for (int s = 0; s < n_satellites; s++)
            total_buf += satellites[s].count;
        static int solve_dbg_cnt = 0;
        if (solve_dbg_cnt++ % 6 == 0)
            fprintf(stderr, "DOPPLER: buffers=%d stored=%d usable=%d "
                    "from %d sats\n",
                    n_satellites, total_buf, n_meas, sats_used);
    }

    pthread_mutex_unlock(&pos_lock);

    /* Check minimum data requirements */
    if (n_meas < MIN_MEASUREMENTS || sats_used < MIN_SATELLITES) {
        out->n_measurements = n_meas;
        out->n_satellites = sats_used;
        return 0;
    }

    /* Solve from several starting points - the separated minima of a
     * coarse misfit grid, and the previous fix - and keep the best fit. */
    double starts[N_GRID_STARTS + 1][3];
    double start_clk[N_GRID_STARTS + 1] = {0};
    int n_starts = grid_starts(all_meas, n_meas, starts, N_GRID_STARTS);
    if (has_prev_solution) {
        memcpy(starts[n_starts], prev_ecef, sizeof(prev_ecef));
        start_clk[n_starts++] = prev_clock_drift;
    }

    double sol[N_GRID_STARTS + 1][3], sol_clk[N_GRID_STARTS + 1];
    double sol_rms[N_GRID_STARTS + 1];
    int n_sol = 0, best = -1;
    for (int k = 0; k < n_starts; k++) {
        double rx[3] = { starts[k][0], starts[k][1], starts[k][2] };
        double clk = start_clk[k];
        if (!refine(all_meas, n_meas, rx, &clk) ||
            count_sats(all_meas, n_meas) < MIN_SATELLITES)
            continue;
        memcpy(sol[n_sol], rx, sizeof(rx));
        sol_clk[n_sol] = clk;
        sol_rms[n_sol] = rms_residual(all_meas, n_meas, rx, clk, NULL);
        if (verbose) {
            double lat, lon, alt, slat, slon, salt;
            ecef_to_geodetic(starts[k], &slat, &slon, &salt);
            ecef_to_geodetic(rx, &lat, &lon, &alt);
            fprintf(stderr, "DOPPLER: start %.1f,%.1f -> %.4f,%.4f "
                    "rms %.2f m/s\n", slat, slon, lat, lon, sol_rms[n_sol]);
        }
        if (best < 0 || sol_rms[n_sol] < sol_rms[best])
            best = n_sol;
        n_sol++;
    }
    if (best < 0) {
        if (verbose)
            fprintf(stderr, "DOPPLER: solver FAIL - no start converged "
                    "(%d meas, %d sats)\n", n_meas, sats_used);
        out->n_measurements = n_meas;
        out->n_satellites = sats_used;
        return 0;
    }

    /* A distinct solution that fits nearly as well: the measurements
     * cannot tell the receiver from its mirror image (one pass, or passes
     * along nearly the same track). Report nothing rather than a side. */
    for (int k = 0; k < n_sol; k++) {
        double d[3];
        vec3_sub(sol[k], sol[best], d);
        if (vec3_norm(d) > AMBIGUOUS_DIST &&
            sol_rms[k] < AMBIGUOUS_RMS_RATIO * sol_rms[best]) {
            if (verbose) {
                double lat, lon, alt, alat, alon, aalt;
                ecef_to_geodetic(sol[best], &lat, &lon, &alt);
                ecef_to_geodetic(sol[k], &alat, &alon, &aalt);
                fprintf(stderr, "DOPPLER: ambiguous - %.4f,%.4f (rms %.2f) "
                        "and %.4f,%.4f (rms %.2f)\n", lat, lon, sol_rms[best],
                        alat, alon, sol_rms[k]);
            }
            out->n_measurements = n_meas;
            out->n_satellites = sats_used;
            return 0;
        }
    }

    /* The chosen solution, with its weights (the last refine was another's) */
    double rx_ecef[3] = { sol[best][0], sol[best][1], sol[best][2] };
    double clock_drift = sol_clk[best];
    if (!refine(all_meas, n_meas, rx_ecef, &clock_drift))
        return 0;

    /* Compute HDOP from the final solution's covariance */
    int n_total = n_meas;  /* all collected; weight 0 = rejected */
    double hdop = 99.9;
    {
        double HtH[4][4] = {{0}};
        int count = 0;

        double hdop_rx_vel[3];
        hdop_rx_vel[0] = -OMEGA_EARTH * rx_ecef[1];
        hdop_rx_vel[1] =  OMEGA_EARTH * rx_ecef[0];
        hdop_rx_vel[2] = 0.0;

        /* Rebuild H^T H from valid (non-rejected) measurements */
        for (int i = 0; i < n_total; i++) {
            solver_meas_t *m = &all_meas[i];
            if (m->weight == 0) continue;

            double los[3];
            vec3_sub(m->sat_ecef, rx_ecef, los);
            double rho = vec3_norm(los);
            if (rho < 1.0) continue;

            double hrel[3] = { m->sat_vel[0] - hdop_rx_vel[0],
                               m->sat_vel[1] - hdop_rx_vel[1],
                               m->sat_vel[2] - hdop_rx_vel[2] };
            double rho_dot_geom = vec3_dot(los, hrel) / rho;
            double H_row[4];
            double rho2 = rho * rho;
            H_row[0] = -hrel[0]/rho + los[0]*rho_dot_geom/rho2
                        + OMEGA_EARTH * los[1] / rho;
            H_row[1] = -hrel[1]/rho + los[1]*rho_dot_geom/rho2
                        - OMEGA_EARTH * los[0] / rho;
            H_row[2] = -hrel[2]/rho + los[2]*rho_dot_geom/rho2;
            H_row[3] = 1.0;

            for (int r = 0; r < 4; r++)
                for (int c = 0; c < 4; c++)
                    HtH[r][c] += H_row[r] * H_row[c];
            count++;
        }

        if (count >= 4) {
            double HtH_copy[4][4];
            memcpy(HtH_copy, HtH, sizeof(HtH));
            double Q[4][4];
            if (mat4_invert(HtH_copy, Q) == 0) {
                /* Rotate Q to ENU */
                double lat, lon, alt;
                ecef_to_geodetic(rx_ecef, &lat, &lon, &alt);
                double R[3][3];
                ecef_to_enu_matrix(lat, lon, R);

                /* Q_enu = R * Q_xyz * R^T (3x3 position block) */
                double Q_enu[3][3] = {{0}};
                for (int i = 0; i < 3; i++)
                    for (int j = 0; j < 3; j++)
                        for (int k = 0; k < 3; k++)
                            for (int l = 0; l < 3; l++)
                                Q_enu[i][j] += R[i][k] * Q[k][l] * R[j][l];

                /* HDOP = sqrt(Q_ee + Q_nn) */
                if (Q_enu[0][0] + Q_enu[1][1] > 0)
                    hdop = sqrt(Q_enu[0][0] + Q_enu[1][1]);
            }
        }
    }

    if (hdop > MAX_HDOP) {
        if (verbose)
            fprintf(stderr, "DOPPLER: HDOP %.0f too weak, no fix\n", hdop);
        out->n_measurements = n_meas;
        out->n_satellites = sats_used;
        return 0;
    }
    rms_residual(all_meas, n_total, rx_ecef, clock_drift, &n_meas);
    sats_used = count_sats(all_meas, n_total);

    /* Note: HDOP is reported in the solution for the caller to assess.
     * With few satellite passes (early operation), HDOP can be 100+
     * but the position is still useful for approximate location. */

    /* Solution jump guard: reject solutions that jump too far from
     * the previous established position. A stationary receiver should
     * not move >500km between solves. Allow override after 5 consecutive
     * rejections (the old position was probably wrong). */
    if (has_prev_solution) {
        double dx = rx_ecef[0] - prev_ecef[0];
        double dy = rx_ecef[1] - prev_ecef[1];
        double dz = rx_ecef[2] - prev_ecef[2];
        double jump = sqrt(dx*dx + dy*dy + dz*dz);

        if (jump > MAX_SOLUTION_JUMP) {
            jump_reject_count++;
            if (jump_reject_count < 5) {
                if (verbose) {
                    double jlat, jlon, jalt;
                    ecef_to_geodetic(rx_ecef, &jlat, &jlon, &jalt);
                    fprintf(stderr, "DOPPLER: rejecting %.0fkm jump to "
                            "%.4f,%.4f (reject #%d)\n",
                            jump / 1000.0, jlat, jlon, jump_reject_count);
                }
                /* Return the previous solution instead */
                ecef_to_geodetic(prev_ecef, &out->lat, &out->lon, &out->alt);
                out->hdop = hdop;
                out->n_measurements = n_meas;
                out->n_satellites = sats_used;
                out->converged = 1;
                return 1;
            } else {
                if (verbose)
                    fprintf(stderr, "DOPPLER: accepting jump after %d "
                            "consecutive rejections (resetting)\n",
                            jump_reject_count);
                jump_reject_count = 0;
            }
        } else {
            jump_reject_count = 0;
        }
    }

    /* Save solution for next iteration */
    prev_ecef[0] = rx_ecef[0];
    prev_ecef[1] = rx_ecef[1];
    prev_ecef[2] = rx_ecef[2];
    prev_clock_drift = clock_drift;
    has_prev_solution = 1;

    /* Convert ECEF to geodetic for output */
    ecef_to_geodetic(rx_ecef, &out->lat, &out->lon, &out->alt);
    out->hdop = hdop;
    out->n_measurements = n_meas;
    out->n_satellites = sats_used;
    out->converged = 1;
    return 1;
}

int doppler_pos_get_active_sats(int *sat_ids, double *latest_freq,
                                  uint64_t *latest_timestamp,
                                  uint64_t now_ns, int max_out)
{
    const uint64_t MAX_AGE_NS = 60000000000ULL;
    pthread_mutex_lock(&pos_lock);

    /* Age measurements against the newest measurement's own clock, NOT the
     * caller's wall clock. Frame timestamps ride a sample-derived clock
     * (anchored to CLOCK_REALTIME once at startup, then advanced by processed
     * sample count). Dropped sample buffers and any SDR sample-rate offset make
     * that clock drift monotonically behind wall time; comparing against a
     * fresh CLOCK_REALTIME therefore ages *every* satellite out after enough
     * runtime (~a day at ~0.07% rate error), zeroing the Doppler display until
     * a restart re-anchors the clock. Using the max stored timestamp as the
     * reference keeps "active" meaning "seen within 60 s of the latest burst",
     * which is drift-immune and matches how doppler_pos_solve() defines now.
     * now_ns is retained for API compatibility but intentionally unused. */
    (void)now_ns;
    uint64_t ref_ns = 0;
    for (int i = 0; i < n_satellites; i++) {
        sat_buffer_t *s = &satellites[i];
        if (s->count == 0) continue;
        sat_meas_t *latest = sat_buf_get(s, s->count - 1);
        if (latest && latest->timestamp > ref_ns)
            ref_ns = latest->timestamp;
    }

    int written = 0;
    for (int i = 0; i < n_satellites && written < max_out; i++) {
        sat_buffer_t *s = &satellites[i];
        if (s->count == 0) continue;
        sat_meas_t *latest = sat_buf_get(s, s->count - 1);
        if (!latest) continue;
        if (ref_ns - latest->timestamp > MAX_AGE_NS) continue;
        sat_ids[written] = s->sat_id;
        latest_freq[written] = latest->freq;
        latest_timestamp[written] = latest->timestamp;
        written++;
    }
    pthread_mutex_unlock(&pos_lock);
    return written;
}

int doppler_pos_get_history(int sat_id, double *freqs,
                              uint64_t *timestamps, int max_out)
{
    pthread_mutex_lock(&pos_lock);
    sat_buffer_t *s = NULL;
    for (int i = 0; i < n_satellites; i++) {
        if (satellites[i].sat_id == sat_id) { s = &satellites[i]; break; }
    }
    if (!s) { pthread_mutex_unlock(&pos_lock); return 0; }
    int n = (s->count < max_out) ? s->count : max_out;
    for (int i = 0; i < n; i++) {
        sat_meas_t *m = sat_buf_get(s, i);
        freqs[i] = m->freq;
        timestamps[i] = m->timestamp;
    }
    pthread_mutex_unlock(&pos_lock);
    return n;
}
