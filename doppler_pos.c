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
#include <string.h>

#include "doppler_pos.h"
#include "wgs84.h"
#include "gsmtap.h"  /* IR_BASE_FREQ, IR_CHANNEL_WIDTH */

extern int verbose;

/* ---- Configuration ---- */

#define MAX_SATELLITES       128
#define MEAS_PER_SAT         200     /* circular buffer depth per satellite */
#define MIN_MEASUREMENTS     8       /* minimum to attempt a solution */
#define MIN_SATELLITES       2       /* need geometry from multiple passes */
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
    sat_meas_t meas[MEAS_PER_SAT];
    int head;               /* next write index */
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

/* Add measurement to a satellite's circular buffer */
static void sat_buf_add(sat_buffer_t *s, const double ecef[3],
                         double freq, uint64_t ts)
{
    sat_meas_t *m = &s->meas[s->head];
    m->sat_ecef[0] = ecef[0];
    m->sat_ecef[1] = ecef[1];
    m->sat_ecef[2] = ecef[2];
    m->freq = freq;
    m->timestamp = ts;
    m->valid = 1;
    s->head = (s->head + 1) % MEAS_PER_SAT;
    if (s->count < MEAS_PER_SAT)
        s->count++;
}

/* Get measurement by index (0 = oldest) */
static sat_meas_t *sat_buf_get(sat_buffer_t *s, int idx)
{
    if (idx < 0 || idx >= s->count) return NULL;
    int start = (s->count < MEAS_PER_SAT) ? 0 :
                (s->head - s->count + MEAS_PER_SAT) % MEAS_PER_SAT;
    return &s->meas[(start + idx) % MEAS_PER_SAT];
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
static int estimate_velocity(sat_buffer_t *s, int idx, double vel[3])
{
    sat_meas_t *cur = sat_buf_get(s, idx);
    if (!cur) return -1;

    double r_norm = vec3_norm(cur->sat_ecef);
    if (r_norm < 1e6) return -1;

    /* Find the most temporally separated measurement for best orbital plane
     * accuracy. Prefer larger separation (more arc = less quantization noise).
     * Cap at 10 minutes to avoid orbital perturbation drift. */
    sat_meas_t *best_other = NULL;
    double best_dt = 0;

    for (int i = 0; i < s->count; i++) {
        if (i == idx) continue;
        sat_meas_t *m = sat_buf_get(s, i);
        if (!m || !m->valid) continue;
        double dt = fabs((double)(m->timestamp - cur->timestamp) / 1e9);
        if (dt >= MIN_VEL_INTERVAL_NS / 1e9 && dt < 600.0 && dt > best_dt) {
            /* Verify the other position is also at valid orbit altitude */
            double other_r = vec3_norm(m->sat_ecef);
            if (other_r < 7050e3 || other_r > 7250e3) continue;
            best_dt = dt;
            best_other = m;
        }
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
            int last = (s->head - 1 + MEAS_PER_SAT) % MEAS_PER_SAT;
            double dt = (double)(timestamp - s->meas[last].timestamp) / 1e9;

            /* Long gap: likely a different physical satellite reusing
             * this 7-bit sat_id. Reset the buffer to avoid mixing
             * measurements from different orbital planes. */
            if (dt > SAT_GAP_RESET_S) {
                if (verbose)
                    fprintf(stderr, "DOPPLER: sat=%d gap=%.0fs, "
                            "resetting buffer (likely new pass)\n",
                            ira->sat_id, dt);
                s->count = 0;
                s->head = 0;
                s->channel_freq = 0;
            } else {
                /* Short gap: verify position consistency.
                 * At ~7.5 km/s, speed > 10 km/s is impossible. */
                double dx = sat_ecef[0] - s->meas[last].sat_ecef[0];
                double dy = sat_ecef[1] - s->meas[last].sat_ecef[1];
                double dz = sat_ecef[2] - s->meas[last].sat_ecef[2];
                double dist = sqrt(dx*dx + dy*dy + dz*dz);
                if (dt > 0 && dt < 120 && dist / dt > 10000.0) {
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

    /* Initial position estimate: use previous solution if available,
     * otherwise use weighted mean of satellite subsatellite points.
     * Weight by per-satellite measurement count to prefer satellites
     * with consistently decoded positions over one-off corrupted decodes. */
    double rx_ecef[3] = {0, 0, 0};
    double clock_drift = 0;

    if (has_prev_solution) {
        rx_ecef[0] = prev_ecef[0];
        rx_ecef[1] = prev_ecef[1];
        rx_ecef[2] = prev_ecef[2];
        clock_drift = prev_clock_drift;
    } else {
        /* Weight each satellite's sub-satellite point by its measurement
         * count. Satellites with many consistent positions are more likely
         * real; one-off corrupted decodes carry less weight. */
        double total_weight = 0;
        for (int s = 0; s < n_satellites; s++) {
            if (!sat_keep[s] || satellites[s].count == 0) continue;
            /* Use latest valid measurement position */
            sat_meas_t *latest = NULL;
            for (int i = satellites[s].count - 1; i >= 0; i--) {
                sat_meas_t *m = sat_buf_get(&satellites[s], i);
                if (m && m->valid) { latest = m; break; }
            }
            if (!latest) continue;

            double r = vec3_norm(latest->sat_ecef);
            if (r <= 0) continue;
            double scale = WGS84_A / r;
            double w = (double)satellites[s].count;
            rx_ecef[0] += latest->sat_ecef[0] * scale * w;
            rx_ecef[1] += latest->sat_ecef[1] * scale * w;
            rx_ecef[2] += latest->sat_ecef[2] * scale * w;
            total_weight += w;
        }
        if (total_weight > 0) {
            rx_ecef[0] /= total_weight;
            rx_ecef[1] /= total_weight;
            rx_ecef[2] /= total_weight;
        }

        /* If height aiding, adjust initial position to correct altitude.
         * Must use geodetic conversion (not simple radius scaling) because
         * WGS-84 is an oblate ellipsoid: surface radius varies by ~21 km
         * between equator and poles. */
        if (height_aiding_enabled) {
            double ilat, ilon, ialt;
            ecef_to_geodetic(rx_ecef, &ilat, &ilon, &ialt);
            geodetic_to_ecef(ilat, ilon, height_aiding_m, rx_ecef);
        }
    }

    if (verbose) {
        double lat0, lon0, alt0;
        ecef_to_geodetic(rx_ecef, &lat0, &lon0, &alt0);
        fprintf(stderr, "DOPPLER: init pos=%.4f,%.4f alt=%.0f "
                "n_meas=%d n_sats=%d\n", lat0, lon0, alt0, n_meas, sats_used);
    }

    /* Iterated Weighted Least Squares */
    int converged = 0;
    int use_height = (height_aiding_enabled);
    int rejected = 0;

    for (int iter = 0; iter < MAX_ITERATIONS; iter++) {
        /* Build H matrix and residual vector.
         * State: [dx, dy, dz, d_clock_drift]
         * H is n_rows x 4, stored row-major in flat arrays */
        double HtWH[4][4] = {{0}};
        double HtWy[4] = {0};

        /* Receiver velocity from Earth rotation: v_rx = omega x r_rx */
        double rx_vel[3];
        rx_vel[0] = -OMEGA_EARTH * rx_ecef[1];
        rx_vel[1] =  OMEGA_EARTH * rx_ecef[0];
        rx_vel[2] = 0.0;

        for (int i = 0; i < n_meas; i++) {
            solver_meas_t *m = &all_meas[i];

            /* Line-of-sight vector and range */
            double los[3];
            vec3_sub(m->sat_ecef, rx_ecef, los);
            double rho = vec3_norm(los);
            if (rho < 1.0) continue;

            /* Relative velocity (satellite minus receiver due to Earth rotation) */
            double rel_vel[3];
            rel_vel[0] = m->sat_vel[0] - rx_vel[0];
            rel_vel[1] = m->sat_vel[1] - rx_vel[1];
            rel_vel[2] = m->sat_vel[2] - rx_vel[2];

            /* Predicted range rate including Earth rotation */
            double rho_dot_geom = vec3_dot(los, rel_vel) / rho;
            double rho_dot_pred = rho_dot_geom + clock_drift;

            /* Residual */
            double dy = m->range_rate - rho_dot_pred;

            /* Partial derivatives including Earth rotation cross terms.
             * d(rho_dot)/d(rx_i) = -(v_rel_i)/rho + los_i * rho_dot_geom / rho^2
             *                      + los . d(-v_rx)/d(rx_i) / rho
             * where d(v_rx_x)/d(rx_y) = -omega, d(v_rx_y)/d(rx_x) = omega */
            double H_row[4];
            double rho2 = rho * rho;
            H_row[0] = -rel_vel[0] / rho + los[0] * rho_dot_geom / rho2
                        + OMEGA_EARTH * los[1] / rho;
            H_row[1] = -rel_vel[1] / rho + los[1] * rho_dot_geom / rho2
                        - OMEGA_EARTH * los[0] / rho;
            H_row[2] = -rel_vel[2] / rho + los[2] * rho_dot_geom / rho2;
            H_row[3] = 1.0;  /* clock drift */

            double w = m->weight;

            /* Accumulate H^T W H and H^T W y */
            for (int r = 0; r < 4; r++) {
                for (int c = 0; c < 4; c++)
                    HtWH[r][c] += H_row[r] * w * H_row[c];
                HtWy[r] += H_row[r] * w * dy;
            }
        }

        /* Height aiding: constrain geodetic altitude to height_aiding_m.
         * Uses geodetic altitude (not ECEF radius) because WGS-84 surface
         * radius varies by ~21 km with latitude. The radial unit vector
         * approximates d(altitude)/d(ecef). */
        if (use_height) {
            double r0 = vec3_norm(rx_ecef);
            if (r0 > 0) {
                double hlat, hlon, halt;
                ecef_to_geodetic(rx_ecef, &hlat, &hlon, &halt);
                double dy_h = height_aiding_m - halt;
                double H_h[4];
                H_h[0] = rx_ecef[0] / r0;
                H_h[1] = rx_ecef[1] / r0;
                H_h[2] = rx_ecef[2] / r0;
                H_h[3] = 0.0;

                /* Height constraint weighted heavily */
                double w_h = 100.0;
                for (int r = 0; r < 4; r++) {
                    for (int c = 0; c < 4; c++)
                        HtWH[r][c] += H_h[r] * w_h * H_h[c];
                    HtWy[r] += H_h[r] * w_h * dy_h;
                }
            }
        }

        /* Plain Gauss-Newton (a tiny ridge only keeps the inverse finite).
         * This used Levenberg-Marquardt damping (diagonal x11 for 10
         * iterations, x2 to 50): each step then covered a small fraction of
         * the way, the steps shrank below CONVERGENCE_M within a few km, and
         * the solve "converged" next to its initial guess - often ~2000 km
         * away. The 500 km step limit below still guards the first steps. */
        for (int i = 0; i < 4; i++)
            HtWH[i][i] += 1e-6;

        /* Solve: delta_x = (H^T W H)^-1 * H^T W y */
        double HtWH_copy[4][4];
        memcpy(HtWH_copy, HtWH, sizeof(HtWH));
        double inv[4][4];
        if (mat4_invert(HtWH_copy, inv) != 0) {
            fprintf(stderr, "DOPPLER: solver FAIL - singular matrix at iter %d\n", iter);
            return 0;
        }

        double delta[4] = {0};
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
                delta[i] += inv[i][j] * HtWy[j];

        /* Limit step size to prevent divergence */
        double step = sqrt(delta[0]*delta[0] + delta[1]*delta[1] +
                           delta[2]*delta[2]);
        double max_step = 500000.0;  /* 500 km max step */
        if (step > max_step) {
            double scale = max_step / step;
            delta[0] *= scale;
            delta[1] *= scale;
            delta[2] *= scale;
            delta[3] *= scale;
        }

        /* Apply correction */
        rx_ecef[0] += delta[0];
        rx_ecef[1] += delta[1];
        rx_ecef[2] += delta[2];
        clock_drift += delta[3];

        double correction = sqrt(delta[0]*delta[0] + delta[1]*delta[1] +
                                  delta[2]*delta[2]);

        if (verbose && (iter < 3 || iter == MAX_ITERATIONS - 1)) {
            double lat, lon, alt;
            ecef_to_geodetic(rx_ecef, &lat, &lon, &alt);
            fprintf(stderr, "DOPPLER: iter %d: correction=%.0f m, "
                    "pos=%.4f,%.4f alt=%.0f clk=%.1f\n",
                    iter, correction, lat, lon, alt, clock_drift);
        }

        if (correction < CONVERGENCE_M) {
            converged = 1;
            break;
        }
    }

    if (!converged) {
        if (verbose) {
            double flat, flon, falt;
            ecef_to_geodetic(rx_ecef, &flat, &flon, &falt);
            fprintf(stderr, "DOPPLER: solver FAIL - %d iters, %d meas, %d sats, "
                    "final=%.2f,%.2f alt=%.0fkm clk=%.1f\n",
                    MAX_ITERATIONS, n_meas, sats_used, flat, flon, falt/1000,
                    clock_drift);
        }
        out->n_measurements = n_meas;
        out->n_satellites = sats_used;
        /* Reset previous solution -- starting from a bad position may prevent
         * convergence indefinitely. Let the solver try fresh next time. */
        has_prev_solution = 0;
        return 0;
    }

    /* Outlier rejection: recompute residuals, remove 3-sigma outliers */
    double sum_res2 = 0;
    int n_valid = 0;
    {
        double orx_vel[3];
        orx_vel[0] = -OMEGA_EARTH * rx_ecef[1];
        orx_vel[1] =  OMEGA_EARTH * rx_ecef[0];
        orx_vel[2] = 0.0;

        for (int i = 0; i < n_meas; i++) {
            solver_meas_t *m = &all_meas[i];
            double los[3];
            vec3_sub(m->sat_ecef, rx_ecef, los);
            double rho = vec3_norm(los);
            if (rho < 1.0) { m->weight = 0; continue; }
            double orel[3] = { m->sat_vel[0] - orx_vel[0],
                               m->sat_vel[1] - orx_vel[1],
                               m->sat_vel[2] - orx_vel[2] };
            double rho_dot_pred = vec3_dot(los, orel) / rho + clock_drift;
            double res = m->range_rate - rho_dot_pred;
            sum_res2 += res * res;
            n_valid++;
        }
    }

    if (n_valid > 4) {
        double sigma = sqrt(sum_res2 / (n_valid - 4));
        rejected = 0;
        double orx_vel[3];
        orx_vel[0] = -OMEGA_EARTH * rx_ecef[1];
        orx_vel[1] =  OMEGA_EARTH * rx_ecef[0];
        orx_vel[2] = 0.0;

        for (int i = 0; i < n_meas; i++) {
            solver_meas_t *m = &all_meas[i];
            if (m->weight == 0) continue;
            double los[3];
            vec3_sub(m->sat_ecef, rx_ecef, los);
            double rho = vec3_norm(los);
            if (rho < 1.0) continue;
            double orel[3] = { m->sat_vel[0] - orx_vel[0],
                               m->sat_vel[1] - orx_vel[1],
                               m->sat_vel[2] - orx_vel[2] };
            double rho_dot_pred = vec3_dot(los, orel) / rho + clock_drift;
            double res = fabs(m->range_rate - rho_dot_pred);
            if (res > OUTLIER_SIGMA * sigma) {
                m->weight = 0;
                rejected++;
            }
        }

        /* Re-solve if outliers were rejected.
         * Keep position and clock drift from first solve as starting point. */
        if (rejected > 0 && n_valid - rejected >= MIN_MEASUREMENTS) {
            converged = 0;

            for (int iter = 0; iter < MAX_ITERATIONS; iter++) {
                double HtWH2[4][4] = {{0}};
                double HtWy2[4] = {0};

                double rx_vel2[3];
                rx_vel2[0] = -OMEGA_EARTH * rx_ecef[1];
                rx_vel2[1] =  OMEGA_EARTH * rx_ecef[0];
                rx_vel2[2] = 0.0;

                for (int i = 0; i < n_meas; i++) {
                    solver_meas_t *m = &all_meas[i];
                    if (m->weight == 0) continue;

                    double los[3];
                    vec3_sub(m->sat_ecef, rx_ecef, los);
                    double rho = vec3_norm(los);
                    if (rho < 1.0) continue;

                    double rel_vel2[3];
                    rel_vel2[0] = m->sat_vel[0] - rx_vel2[0];
                    rel_vel2[1] = m->sat_vel[1] - rx_vel2[1];
                    rel_vel2[2] = m->sat_vel[2] - rx_vel2[2];

                    double rho_dot_geom = vec3_dot(los, rel_vel2) / rho;
                    double rho_dot_pred = rho_dot_geom + clock_drift;
                    double dy = m->range_rate - rho_dot_pred;

                    double H_row[4];
                    double rho2 = rho * rho;
                    H_row[0] = -rel_vel2[0] / rho + los[0] * rho_dot_geom / rho2
                                + OMEGA_EARTH * los[1] / rho;
                    H_row[1] = -rel_vel2[1] / rho + los[1] * rho_dot_geom / rho2
                                - OMEGA_EARTH * los[0] / rho;
                    H_row[2] = -rel_vel2[2] / rho + los[2] * rho_dot_geom / rho2;
                    H_row[3] = 1.0;

                    double w = m->weight;
                    for (int r = 0; r < 4; r++) {
                        for (int c = 0; c < 4; c++)
                            HtWH2[r][c] += H_row[r] * w * H_row[c];
                        HtWy2[r] += H_row[r] * w * dy;
                    }
                }

                if (use_height) {
                    double r0 = vec3_norm(rx_ecef);
                    if (r0 > 0) {
                        double hlat2, hlon2, halt2;
                        ecef_to_geodetic(rx_ecef, &hlat2, &hlon2, &halt2);
                        double dy_h = height_aiding_m - halt2;
                        double H_h[4] = { rx_ecef[0]/r0, rx_ecef[1]/r0,
                                           rx_ecef[2]/r0, 0 };
                        double w_h = 100.0;
                        for (int r = 0; r < 4; r++) {
                            for (int c = 0; c < 4; c++)
                                HtWH2[r][c] += H_h[r] * w_h * H_h[c];
                            HtWy2[r] += H_h[r] * w_h * dy_h;
                        }
                    }
                }

                double HtWH2_copy[4][4];
                memcpy(HtWH2_copy, HtWH2, sizeof(HtWH2));
                double inv2[4][4];
                if (mat4_invert(HtWH2_copy, inv2) != 0) {
                    fprintf(stderr, "DOPPLER: re-solve FAIL - singular matrix\n");
                    return 0;
                }

                double delta[4] = {0};
                for (int i = 0; i < 4; i++)
                    for (int j = 0; j < 4; j++)
                        delta[i] += inv2[i][j] * HtWy2[j];

                rx_ecef[0] += delta[0];
                rx_ecef[1] += delta[1];
                rx_ecef[2] += delta[2];
                clock_drift += delta[3];

                double correction = sqrt(delta[0]*delta[0] + delta[1]*delta[1] +
                                          delta[2]*delta[2]);
                if (correction < CONVERGENCE_M) {
                    converged = 1;
                    break;
                }
            }

            if (!converged) {
                fprintf(stderr, "DOPPLER: re-solve FAIL - did not converge\n");
                return 0;
            }

            n_meas = n_valid - rejected;
        }
    }

    /* Per-satellite residual screening: if one satellite's measurements
     * have systematically higher residuals than others, it likely has
     * a wrong channel assignment or corrupted orbital data. Drop it. */
    {
        int n_total_now = n_meas + rejected;

        /* Compute receiver velocity for residual calculation */
        double ps_rx_vel[3];
        ps_rx_vel[0] = -OMEGA_EARTH * rx_ecef[1];
        ps_rx_vel[1] =  OMEGA_EARTH * rx_ecef[0];
        ps_rx_vel[2] = 0.0;

        /* Accumulate per-satellite mean absolute residual */
        double sat_res_sum[MAX_SATELLITES] = {0};
        int sat_res_count[MAX_SATELLITES] = {0};

        for (int i = 0; i < n_total_now; i++) {
            solver_meas_t *m = &all_meas[i];
            if (m->weight == 0) continue;
            if (m->sat_idx < 0 || m->sat_idx >= MAX_SATELLITES) continue;

            double los[3];
            vec3_sub(m->sat_ecef, rx_ecef, los);
            double rho = vec3_norm(los);
            if (rho < 1.0) continue;

            double rel[3] = { m->sat_vel[0] - ps_rx_vel[0],
                              m->sat_vel[1] - ps_rx_vel[1],
                              m->sat_vel[2] - ps_rx_vel[2] };
            double rho_dot_pred = vec3_dot(los, rel) / rho + clock_drift;
            double res = fabs(m->range_rate - rho_dot_pred);

            sat_res_sum[m->sat_idx] += res;
            sat_res_count[m->sat_idx]++;
        }

        /* Collect per-satellite mean residuals */
        double sat_mean_res[MAX_SATELLITES];
        int active_sats[MAX_SATELLITES];
        int n_active = 0;

        for (int s = 0; s < MAX_SATELLITES; s++) {
            if (sat_res_count[s] == 0) continue;
            sat_mean_res[s] = sat_res_sum[s] / sat_res_count[s];
            active_sats[n_active++] = s;
        }

        if (n_active >= 3) {
            /* Find median residual across satellites */
            double sorted_res[MAX_SATELLITES];
            for (int i = 0; i < n_active; i++)
                sorted_res[i] = sat_mean_res[active_sats[i]];
            /* Simple insertion sort for small n */
            for (int i = 1; i < n_active; i++) {
                double key = sorted_res[i];
                int j = i - 1;
                while (j >= 0 && sorted_res[j] > key) {
                    sorted_res[j + 1] = sorted_res[j];
                    j--;
                }
                sorted_res[j + 1] = key;
            }
            double median_res = sorted_res[n_active / 2];

            /* Drop satellites with mean residual > 3x median */
            int sat_dropped = 0;
            for (int i = 0; i < n_active; i++) {
                int s = active_sats[i];
                if (sat_mean_res[s] > 3.0 * median_res && median_res > 0) {
                    if (verbose)
                        fprintf(stderr, "DOPPLER: dropping sat_idx=%d "
                                "(sat_id=%d) residual=%.1f vs median=%.1f\n",
                                s, satellites[s].sat_id,
                                sat_mean_res[s], median_res);
                    /* Zero out all measurements from this satellite */
                    for (int j = 0; j < n_total_now; j++) {
                        if (all_meas[j].sat_idx == s)
                            all_meas[j].weight = 0;
                    }
                    sat_dropped++;
                    sats_used--;
                }
            }

            /* Re-solve if satellites were dropped and enough remain */
            if (sat_dropped > 0) {
                int remaining = 0;
                for (int i = 0; i < n_total_now; i++)
                    if (all_meas[i].weight > 0) remaining++;

                if (remaining >= MIN_MEASUREMENTS && sats_used >= MIN_SATELLITES) {
                    converged = 0;
                    n_meas = remaining;

                    for (int iter = 0; iter < MAX_ITERATIONS; iter++) {
                        double HtWH3[4][4] = {{0}};
                        double HtWy3[4] = {0};

                        double rv3[3];
                        rv3[0] = -OMEGA_EARTH * rx_ecef[1];
                        rv3[1] =  OMEGA_EARTH * rx_ecef[0];
                        rv3[2] = 0.0;

                        for (int i = 0; i < n_total_now; i++) {
                            solver_meas_t *m = &all_meas[i];
                            if (m->weight == 0) continue;

                            double los[3];
                            vec3_sub(m->sat_ecef, rx_ecef, los);
                            double rho = vec3_norm(los);
                            if (rho < 1.0) continue;

                            double rv[3] = { m->sat_vel[0] - rv3[0],
                                             m->sat_vel[1] - rv3[1],
                                             m->sat_vel[2] - rv3[2] };
                            double rho_dot_geom = vec3_dot(los, rv) / rho;
                            double rho_dot_pred = rho_dot_geom + clock_drift;
                            double dy = m->range_rate - rho_dot_pred;

                            double H_row[4];
                            double rho2 = rho * rho;
                            H_row[0] = -rv[0]/rho + los[0]*rho_dot_geom/rho2
                                        + OMEGA_EARTH * los[1] / rho;
                            H_row[1] = -rv[1]/rho + los[1]*rho_dot_geom/rho2
                                        - OMEGA_EARTH * los[0] / rho;
                            H_row[2] = -rv[2]/rho + los[2]*rho_dot_geom/rho2;
                            H_row[3] = 1.0;

                            double w = m->weight;
                            for (int r = 0; r < 4; r++) {
                                for (int c = 0; c < 4; c++)
                                    HtWH3[r][c] += H_row[r] * w * H_row[c];
                                HtWy3[r] += H_row[r] * w * dy;
                            }
                        }

                        if (use_height) {
                            double r0 = vec3_norm(rx_ecef);
                            if (r0 > 0) {
                                double hlat3, hlon3, halt3;
                                ecef_to_geodetic(rx_ecef, &hlat3, &hlon3, &halt3);
                                double dy_h = height_aiding_m - halt3;
                                double H_h[4] = { rx_ecef[0]/r0, rx_ecef[1]/r0,
                                                   rx_ecef[2]/r0, 0 };
                                double w_h = 100.0;
                                for (int r = 0; r < 4; r++) {
                                    for (int c = 0; c < 4; c++)
                                        HtWH3[r][c] += H_h[r] * w_h * H_h[c];
                                    HtWy3[r] += H_h[r] * w_h * dy_h;
                                }
                            }
                        }

                        double HtWH3_copy[4][4];
                        memcpy(HtWH3_copy, HtWH3, sizeof(HtWH3));
                        double inv3[4][4];
                        if (mat4_invert(HtWH3_copy, inv3) != 0) {
                            fprintf(stderr, "DOPPLER: per-sat re-solve "
                                    "FAIL - singular matrix\n");
                            return 0;
                        }

                        double delta[4] = {0};
                        for (int i = 0; i < 4; i++)
                            for (int j = 0; j < 4; j++)
                                delta[i] += inv3[i][j] * HtWy3[j];

                        rx_ecef[0] += delta[0];
                        rx_ecef[1] += delta[1];
                        rx_ecef[2] += delta[2];
                        clock_drift += delta[3];

                        double correction = sqrt(delta[0]*delta[0] +
                                                  delta[1]*delta[1] +
                                                  delta[2]*delta[2]);
                        if (correction < CONVERGENCE_M) {
                            converged = 1;
                            break;
                        }
                    }

                    if (!converged) {
                        if (verbose)
                            fprintf(stderr, "DOPPLER: per-sat re-solve "
                                    "FAIL - did not converge\n");
                        return 0;
                    }
                } else {
                    if (verbose)
                        fprintf(stderr, "DOPPLER: per-sat screening left "
                                "too few measurements (%d meas, %d sats)\n",
                                remaining, sats_used);
                    return 0;
                }
            }
        }
    }

    /* Compute HDOP from the final solution's covariance */
    int n_total = n_meas + rejected;  /* original array size */
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
