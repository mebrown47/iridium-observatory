/*
 * Built-in web map server for Iridium ring alerts and satellites
 *
 * Minimal HTTP server with SSE (Server-Sent Events) for real-time
 * map updates. Uses Leaflet.js + OpenStreetMap for visualization.
 *
 * Copyright (c) 2026 CEMAXECUTER LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Built-in web map server for Iridium ring alerts and satellites
 *
 * Minimal HTTP server with SSE (Server-Sent Events) for real-time
 * map updates. Uses Leaflet.js + OpenStreetMap for visualization.
 *
 * Two endpoints:
 *   GET /           → embedded HTML/JS map page
 *   GET /api/events → SSE stream (1 Hz JSON updates)
 */

#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include "web_map.h"
#include "ida_decode.h"
#include "doppler_pos.h"

/* Set by option parsing in main.c; gates the Doppler subsystem's init. */
extern int position_enabled;

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- Configuration ---- */

#define MAX_RA_POINTS    2000
#define MAX_BEAM_POINTS  2000
#define MAX_MT_POINTS    500
#define MAX_SATELLITES   100
#define MAX_AIRCRAFT     32
#define MAX_AIRCRAFT_FIXES 8
#define MAX_SSE_CLIENTS  8
#define MAX_ACARS_MSGS   100
#define MAX_PREMIUM_MSGS 100
#define ACARS_TEXT_MAX   256
#define MAX_PAGER_MSGS   200
#define PAGER_TEXT_MAX   168
#define MAX_DOPPLER_SATS 16
/* /api/state serializes the entire live state (aircraft, sats, ACARS/pager
 * rings, beams). 128 KiB silently truncated it on a busy receiver: build_json
 * ran past the buffer, snprintf clamped, and the half-written JSON failed to
 * parse client-side — every tab reading /api/state went blank with no error.
 * 1 MiB clears the worst-case populated state with headroom. */
#define JSON_BUF_SIZE    1048576
#define HTTP_BUF_SIZE    4096

/* ---- SSE client count ---- */
static atomic_int sse_client_count = 0;

/* ---- Shared state ---- */

typedef struct {
    double lat, lon;
    int alt;
    int sat_id, beam_id;
    int n_pages;
    uint32_t tmsi;
    double frequency;
    uint64_t timestamp;
} ra_point_t;

/* Ground beam center position (alt < 100 from IRA frames) */
typedef struct {
    double lat, lon;
    int alt;
    int sat_id, beam_id;
    int n_pages;
    uint32_t tmsi;
    double frequency;
    uint64_t timestamp;
} beam_point_t;

/* MT phone/terminal position from IDA reassembly */
typedef struct {
    double lat, lon;
    int alt;
    uint16_t msg_type;
    uint64_t timestamp;
    double frequency;
} mt_point_t;

typedef struct {
    int sat_id;
    int beam_id;
    uint64_t last_seen;
    int count;
} sat_entry_t;

/* ACARS aircraft position (one entry per tail number) */
typedef struct {
    char reg[16];    /* registration / tail number */
    char flight[8];  /* flight number, may be empty */
    char oooi[8];    /* last OOOI event: "OUT","OFF","ON","IN", or empty */
    int sat_id, beam_id;
    double fix_lat[MAX_AIRCRAFT_FIXES];
    double fix_lon[MAX_AIRCRAFT_FIXES];
    uint64_t fix_t[MAX_AIRCRAFT_FIXES];  /* seconds since epoch */
    int fix_alt[MAX_AIRCRAFT_FIXES];     /* altitude in feet, -99999 if unknown */
    uint8_t fix_adsc[MAX_AIRCRAFT_FIXES]; /* 1=ADS-C GPS position, 0=beam estimate */
    int n_fixes;
    double frequency;
    uint64_t last_seen;  /* ns, for eviction */
} aircraft_entry_t;

/* Recent ACARS message entry for the chronological feed tab */
typedef struct {
    char reg[16];
    char flight[8];
    char label[4];
    char text[ACARS_TEXT_MAX];
    int ul;
    uint64_t timestamp;
} acars_msg_t;
typedef struct {
    char reg[16];
    char flight[16];
    char label[4];
    char text[ACARS_TEXT_MAX];
    int ul;
    uint64_t timestamp;
} premium_msg_t;

/* Recent pager (MSG/IMS) message entry for the messaging feed tab */
typedef struct {
    int ric;            /* pager id */
    int format;         /* 5=ASCII, 3=BCD */
    int seq;
    int csum_ok;
    char text[PAGER_TEXT_MAX];
    uint64_t timestamp;
} pager_msg_t;

typedef struct {
    int sat_id;
    double freq;
    uint64_t timestamp;
} sat_doppler_t;

static struct {
    pthread_mutex_t lock;
    ra_point_t ra[MAX_RA_POINTS];
    int ra_head;
    int ra_count;
    beam_point_t beams[MAX_BEAM_POINTS];
    int beam_head;
    int beam_count;
    mt_point_t mt[MAX_MT_POINTS];
    int mt_head;
    int mt_count;
    sat_entry_t sats[MAX_SATELLITES];
    int n_sats;
    aircraft_entry_t aircraft[MAX_AIRCRAFT];
    int n_aircraft;
    acars_msg_t acars[MAX_ACARS_MSGS];
    int acars_head;
    int acars_count;
    premium_msg_t premium[MAX_PREMIUM_MSGS];
    int premium_head;
    int premium_count;
    unsigned long total_premium_msgs;
    pager_msg_t pager[MAX_PAGER_MSGS];
    int pager_head;
    int pager_count;
    sat_doppler_t doppler[MAX_DOPPLER_SATS];
    int doppler_count;
    unsigned long total_ira;
    unsigned long total_ibc;
    unsigned long total_pages;
    unsigned long total_beams;
    unsigned long total_mt;
    unsigned long total_aircraft;
    unsigned long total_acars_msgs;
    unsigned long total_msg;       /* decoded pager (MSG) messages */
    unsigned long type_counts[16]; /* per-frame-type histogram (see ftype_names) */
    unsigned long uw_fail[3];      /* UW-check failures by dir: [0]=DL [1]=UL [2]=other */
    unsigned long uw_ambiguous;    /* hard UW check matched BOTH DL and UL (direction tie) */
    /* Doppler positioning receiver estimate */
    double rx_lat, rx_lon;
    double rx_hdop;
    int rx_valid;
} state;

/* ---- Server state ---- */

static int server_fd = -1;
static pthread_t server_thread;
static volatile int server_running = 0;

/* ---- State update functions (thread-safe) ---- */

static void add_beam_locked(const ira_data_t *ra, uint64_t timestamp,
                             double frequency)
{
    /* Deduplication: skip if same sat_id already has this lat/lon recently */
    int search = (state.beam_count < 20) ? state.beam_count : 20;
    for (int i = 0; i < search; i++) {
        int idx = (state.beam_head - 1 - i + MAX_BEAM_POINTS) % MAX_BEAM_POINTS;
        beam_point_t *b = &state.beams[idx];
        if (b->sat_id == ra->sat_id &&
            fabs(b->lat - ra->lat) < 0.001 &&
            fabs(b->lon - ra->lon) < 0.001) {
            /* Duplicate -- update timestamp and page info */
            b->timestamp = timestamp;
            if (ra->n_pages > 0) {
                b->n_pages = ra->n_pages;
                b->tmsi = ra->pages[0].tmsi;
                state.total_pages++;
            }
            state.total_beams++;
            return;
        }
    }

    beam_point_t *p = &state.beams[state.beam_head];
    p->lat = ra->lat;
    p->lon = ra->lon;
    p->alt = ra->alt;
    p->sat_id = ra->sat_id;
    p->beam_id = ra->beam_id;
    p->n_pages = ra->n_pages;
    p->tmsi = (ra->n_pages > 0) ? ra->pages[0].tmsi : 0;
    p->frequency = frequency;
    p->timestamp = timestamp;

    state.beam_head = (state.beam_head + 1) % MAX_BEAM_POINTS;
    if (state.beam_count < MAX_BEAM_POINTS)
        state.beam_count++;
    state.total_beams++;
    if (ra->n_pages > 0)
        state.total_pages++;
}

void web_map_add_ra(const ira_data_t *ra, uint64_t timestamp,
                     double frequency)
{
    /* Sanity check coordinates */
    if (ra->lat < -90 || ra->lat > 90 || ra->lon < -180 || ra->lon > 180)
        return;
    if (ra->sat_id == 0 && ra->beam_id == 0 && ra->lat == 0 && ra->lon == 0)
        return;

    /* Ground beam position: alt < 100 km (beam center on earth surface) */
    if (ra->alt >= 0 && ra->alt < 100) {
        pthread_mutex_lock(&state.lock);
        state.total_ira++;
        add_beam_locked(ra, timestamp, frequency);
        pthread_mutex_unlock(&state.lock);
        return;
    }

    /* Satellite orbital position: 700-900 km */
    if (ra->alt < 700 || ra->alt > 900)
        return;

    pthread_mutex_lock(&state.lock);

    ra_point_t *p = &state.ra[state.ra_head];
    p->lat = ra->lat;
    p->lon = ra->lon;
    p->alt = ra->alt;
    p->sat_id = ra->sat_id;
    p->beam_id = ra->beam_id;
    p->n_pages = ra->n_pages;
    p->tmsi = (ra->n_pages > 0) ? ra->pages[0].tmsi : 0;
    p->frequency = frequency;
    p->timestamp = timestamp;

    state.ra_head = (state.ra_head + 1) % MAX_RA_POINTS;
    if (state.ra_count < MAX_RA_POINTS)
        state.ra_count++;
    state.total_ira++;
    if (ra->n_pages > 0)
        state.total_pages++;

    pthread_mutex_unlock(&state.lock);
}

void web_map_add_sat(const ibc_data_t *ibc, uint64_t timestamp)
{
    if (ibc->sat_id == 0) return;

    pthread_mutex_lock(&state.lock);

    /* Find or create satellite entry */
    int idx = -1;
    for (int i = 0; i < state.n_sats; i++) {
        if (state.sats[i].sat_id == ibc->sat_id) {
            idx = i;
            break;
        }
    }

    if (idx < 0 && state.n_sats < MAX_SATELLITES) {
        idx = state.n_sats++;
        state.sats[idx].sat_id = ibc->sat_id;
        state.sats[idx].count = 0;
    }

    if (idx >= 0) {
        state.sats[idx].beam_id = ibc->beam_id;
        state.sats[idx].last_seen = timestamp;
        state.sats[idx].count++;
    }
    state.total_ibc++;

    pthread_mutex_unlock(&state.lock);
}

void web_map_set_position(double lat, double lon, double hdop)
{
    pthread_mutex_lock(&state.lock);
    state.rx_lat = lat;
    state.rx_lon = lon;
    state.rx_hdop = hdop;
    state.rx_valid = 1;
    pthread_mutex_unlock(&state.lock);
}

void web_map_add_mt(double lat, double lon, int alt, uint16_t msg_type,
                     uint64_t timestamp, double frequency)
{
    if (lat < -90 || lat > 90 || lon < -180 || lon > 180)
        return;

    pthread_mutex_lock(&state.lock);

    mt_point_t *p = &state.mt[state.mt_head];
    p->lat = lat;
    p->lon = lon;
    p->alt = alt;
    p->msg_type = msg_type;
    p->timestamp = timestamp;
    p->frequency = frequency;

    state.mt_head = (state.mt_head + 1) % MAX_MT_POINTS;
    if (state.mt_count < MAX_MT_POINTS)
        state.mt_count++;
    state.total_mt++;

    pthread_mutex_unlock(&state.lock);
}

/* ---- MT position extraction from IDA messages ---- */

/* Extract 12-bit signed XYZ from 5 bytes (two's complement encoding).
 * Matches iridium-toolkit util.py xyz() function. */
static int mtpos_xyz(const uint8_t *bytes, int skip,
                      double *lat, double *lon, int *alt)
{
    uint64_t val = 0;
    for (int i = 0; i < 5; i++)
        val = (val << 8) | bytes[i];

    int sb = 4 - skip;

    int x = (int)((val >> (24 + sb)) & 0xFFF);
    int y = (int)((val >> (12 + sb)) & 0xFFF);
    int z = (int)((val >> (0  + sb)) & 0xFFF);

    /* Two's complement sign extension */
    if (x > 0x7FF) x -= 0x1000;
    if (y > 0x7FF) y -= 0x1000;
    if (z > 0x7FF) z -= 0x1000;

    if (x == 0 && y == 0 && z == 0) return 0;

    double xy = sqrt((double)x * x + (double)y * y);
    *lat = atan2((double)z, xy) * 180.0 / M_PI;
    *lon = atan2((double)y, (double)x) * 180.0 / M_PI;

    /* radius is Earth-center distance in km (each unit ~4km) */
    double radius_km = sqrt((double)x * x + (double)y * y +
                            (double)z * z) * 4.0;
    *alt = (int)(radius_km - 6371.0);

    if (*lat < -90 || *lat > 90) return 0;
    /* reject positions with unreasonable Earth-center distance */
    if (radius_km < 5000.0 || radius_km > 7000.0) return 0;
    return 1;
}

void mtpos_ida_cb(const uint8_t *data, int len,
                   uint64_t timestamp, double frequency,
                   ir_direction_t direction, float magnitude,
                   void *user)
{
    (void)magnitude;
    (void)user;

    if (len < 5) return;

    uint16_t msg_type = ((uint16_t)data[0] << 8) | data[1];
    double lat, lon;
    int alt;
    int valid = 0;

    switch (msg_type) {
    case 0x0605:
        /* GSM paging with position at offset 36, marker 0x1b */
        if (len >= 42 && data[36] == 0x1b)
            valid = mtpos_xyz(data + 37, 0, &lat, &lon, &alt);
        break;

    case 0x7605:
        /* SBD paging with position */
        if (len >= 8 && data[2] == 0x00 && (data[3] & 0xf0) == 0x40)
            valid = mtpos_xyz(data + 3, 4, &lat, &lon, &alt);
        break;

    case 0x0600:
        /* Uplink with position */
        if (direction == DIR_UPLINK && len >= 24 &&
            (data[2] == 0x10 || data[2] == 0x40 || data[2] == 0x70) &&
            data[18] == 0x01)
            valid = mtpos_xyz(data + 19, 0, &lat, &lon, &alt);
        break;

    default:
        return;
    }

    if (valid)
        web_map_add_mt(lat, lon, alt, msg_type, timestamp, frequency);
}

/* ---- Aircraft beam-based position ---- */

/* Haversine great-circle distance in km */
static double aircraft_dist_km(double lat1, double lon1,
                                double lat2, double lon2)
{
    const double R = 6371.0;
    double dlat = (lat2 - lat1) * M_PI / 180.0;
    double dlon = (lon2 - lon1) * M_PI / 180.0;
    double a = sin(dlat/2)*sin(dlat/2) +
               cos(lat1*M_PI/180.0)*cos(lat2*M_PI/180.0)*
               sin(dlon/2)*sin(dlon/2);
    return R * 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
}

void web_map_add_aircraft(const char *reg, const char *flight,
                           double lat, double lon,
                           int sat_id, int beam_id,
                           uint64_t timestamp_ns, double frequency,
                           int alt_ft, int adsc_pos,
                           const char *oooi)
{
    if (!reg || !reg[0]) return;

    pthread_mutex_lock(&state.lock);

    /* Find existing entry for this registration */
    int idx = -1;
    for (int i = 0; i < state.n_aircraft; i++) {
        if (strncmp(state.aircraft[i].reg, reg, 15) == 0) {
            idx = i;
            break;
        }
    }

    /* If not found, create new or evict the oldest entry */
    if (idx < 0) {
        if (state.n_aircraft < MAX_AIRCRAFT) {
            idx = state.n_aircraft++;
        } else {
            uint64_t oldest = UINT64_MAX;
            for (int i = 0; i < MAX_AIRCRAFT; i++) {
                if (state.aircraft[i].last_seen < oldest) {
                    oldest = state.aircraft[i].last_seen;
                    idx = i;
                }
            }
            memset(&state.aircraft[idx], 0, sizeof(state.aircraft[idx]));
        }
        strncpy(state.aircraft[idx].reg, reg, 15);
        state.aircraft[idx].reg[15] = '\0';
        state.aircraft[idx].n_fixes = 0;
    }

    aircraft_entry_t *ac = &state.aircraft[idx];

    if (flight && flight[0]) {
        strncpy(ac->flight, flight, 7);
        ac->flight[7] = '\0';
    }

    if (oooi && oooi[0]) {
        strncpy(ac->oooi, oooi, 7);
        ac->oooi[7] = '\0';
    }

    /* Speed plausibility check: reject beam-estimate fixes that would require
     * flying faster than 1200 km/h (commercial max + supersonic buffer).
     * ADS-C GPS positions bypass this check since they're not beam guesses. */
    if (!adsc_pos && ac->n_fixes > 0) {
        int last = ac->n_fixes - 1;
        double dt_s = (double)(timestamp_ns - ac->last_seen) / 1e9;
        if (dt_s > 0.0 && dt_s < 7200.0) {
            double max_km = (dt_s / 3600.0) * 1200.0;
            double dist = aircraft_dist_km(ac->fix_lat[last], ac->fix_lon[last],
                                           lat, lon);
            if (dist > max_km) {
                /* Implausible jump - beam from wrong satellite, discard */
                pthread_mutex_unlock(&state.lock);
                return;
            }
        }
    }

    /* Append fix; shift out oldest when the history is full */
    int nf = ac->n_fixes;
    if (nf >= MAX_AIRCRAFT_FIXES) {
        memmove(&ac->fix_lat[0],  &ac->fix_lat[1],
                (MAX_AIRCRAFT_FIXES - 1) * sizeof(ac->fix_lat[0]));
        memmove(&ac->fix_lon[0],  &ac->fix_lon[1],
                (MAX_AIRCRAFT_FIXES - 1) * sizeof(ac->fix_lon[0]));
        memmove(&ac->fix_t[0],    &ac->fix_t[1],
                (MAX_AIRCRAFT_FIXES - 1) * sizeof(ac->fix_t[0]));
        memmove(&ac->fix_alt[0],  &ac->fix_alt[1],
                (MAX_AIRCRAFT_FIXES - 1) * sizeof(ac->fix_alt[0]));
        memmove(&ac->fix_adsc[0], &ac->fix_adsc[1],
                (MAX_AIRCRAFT_FIXES - 1) * sizeof(ac->fix_adsc[0]));
        nf = MAX_AIRCRAFT_FIXES - 1;
    }
    ac->fix_lat[nf]  = lat;
    ac->fix_lon[nf]  = lon;
    ac->fix_t[nf]    = timestamp_ns / 1000000000ULL;
    ac->fix_alt[nf]  = alt_ft;
    ac->fix_adsc[nf] = (uint8_t)adsc_pos;
    ac->n_fixes = nf + 1;
    ac->sat_id    = sat_id;
    ac->beam_id   = beam_id;
    ac->frequency = frequency;
    ac->last_seen = timestamp_ns;
    state.total_aircraft++;

    pthread_mutex_unlock(&state.lock);
}

void web_map_add_acars_message(const char *reg, const char *flight,
                                const char *label, const char *text,
                                int ul, uint64_t timestamp_ns)
{
    if (!reg || !label || !text || !text[0]) return;

    pthread_mutex_lock(&state.lock);

    acars_msg_t *m = &state.acars[state.acars_head];

    /* Strip leading dots from registration */
    const char *r = reg;
    while (*r == '.') r++;
    strncpy(m->reg, r, sizeof(m->reg) - 1);
    m->reg[sizeof(m->reg) - 1] = '\0';

    m->flight[0] = '\0';
    if (flight && flight[0]) {
        strncpy(m->flight, flight, sizeof(m->flight) - 1);
        m->flight[sizeof(m->flight) - 1] = '\0';
    }

    strncpy(m->label, label, sizeof(m->label) - 1);
    m->label[sizeof(m->label) - 1] = '\0';

    strncpy(m->text, text, sizeof(m->text) - 1);
    m->text[sizeof(m->text) - 1] = '\0';

    m->ul = ul ? 1 : 0;
    m->timestamp = timestamp_ns;

    state.acars_head = (state.acars_head + 1) % MAX_ACARS_MSGS;
    if (state.acars_count < MAX_ACARS_MSGS)
        state.acars_count++;
    state.total_acars_msgs++;
    pthread_mutex_unlock(&state.lock);
}

void web_map_add_premium_msg(const char *reg, const char *flight,
                              const char *label, const char *text,
                              int ul, uint64_t timestamp_ns)
{
    if (!reg || !label || !text || !text[0]) return;
    pthread_mutex_lock(&state.lock);
    premium_msg_t *m = &state.premium[state.premium_head];
    const char *r = reg;
    while (*r == '.') r++;
    strncpy(m->reg, r, sizeof(m->reg) - 1);
    m->reg[sizeof(m->reg) - 1] = '\0';
    m->flight[0] = '\0';
    if (flight && flight[0]) {
        strncpy(m->flight, flight, sizeof(m->flight) - 1);
        m->flight[sizeof(m->flight) - 1] = '\0';
    }
    strncpy(m->label, label, sizeof(m->label) - 1);
    m->label[sizeof(m->label) - 1] = '\0';
    strncpy(m->text, text, sizeof(m->text) - 1);
    m->text[sizeof(m->text) - 1] = '\0';
    m->ul = ul ? 1 : 0;
    m->timestamp = timestamp_ns;
    state.premium_head = (state.premium_head + 1) % MAX_PREMIUM_MSGS;
    if (state.premium_count < MAX_PREMIUM_MSGS)
        state.premium_count++;
    state.total_premium_msgs++;
    pthread_mutex_unlock(&state.lock);
}

/* Frame-type histogram labels. Order is fixed and mirrored in the JSON and
 * the HTML panel. Index returned by ftype_index(); -1 if unknown. */
static const char *ftype_names[] = {
    "IRA", "IBC", "MSG", "IDA", "IDA_UL_FAIL",
    "ISY", "IIP", "VOC", "ITL", "IU3", "IU6", "RAW"
};
#define N_FTYPES ((int)(sizeof(ftype_names) / sizeof(ftype_names[0])))

void web_map_count_type(const char *label)
{
    if (!label) return;
    int idx = -1;
    for (int i = 0; i < N_FTYPES; i++)
        if (strcmp(label, ftype_names[i]) == 0) { idx = i; break; }
    if (idx < 0) return;
    pthread_mutex_lock(&state.lock);
    state.type_counts[idx]++;
    pthread_mutex_unlock(&state.lock);
}
void web_map_count_uw_fail(ir_direction_t direction)
{
    int i = (direction == DIR_DOWNLINK) ? 0 :
            (direction == DIR_UPLINK)   ? 1 : 2;
    pthread_mutex_lock(&state.lock);
    state.uw_fail[i]++;
    pthread_mutex_unlock(&state.lock);
}

void web_map_count_uw_ambiguous(void)
{
    pthread_mutex_lock(&state.lock);
    state.uw_ambiguous++;
    pthread_mutex_unlock(&state.lock);
}

void web_map_add_msg(const msg_data_t *msg, uint64_t timestamp_ns)
{
    if (!msg || !msg->text[0]) return;

    pthread_mutex_lock(&state.lock);

    pager_msg_t *m = &state.pager[state.pager_head];
    m->ric = msg->ric;
    m->format = msg->format;
    m->seq = msg->seq;
    m->csum_ok = msg->csum_ok;
    strncpy(m->text, msg->text, sizeof(m->text) - 1);
    m->text[sizeof(m->text) - 1] = '\0';
    m->timestamp = timestamp_ns;

    state.pager_head = (state.pager_head + 1) % MAX_PAGER_MSGS;
    if (state.pager_count < MAX_PAGER_MSGS)
        state.pager_count++;
    state.total_msg++;
    pthread_mutex_unlock(&state.lock);
}

void web_map_update_doppler(uint64_t now_ns)
{
    int sat_ids[MAX_DOPPLER_SATS];
    double freqs[MAX_DOPPLER_SATS];
    uint64_t timestamps[MAX_DOPPLER_SATS];
    int n = doppler_pos_get_active_sats(sat_ids, freqs, timestamps,
                                          now_ns, MAX_DOPPLER_SATS);
    pthread_mutex_lock(&state.lock);
    for (int i = 0; i < n; i++) {
        state.doppler[i].sat_id = sat_ids[i];
        state.doppler[i].freq = freqs[i];
        state.doppler[i].timestamp = timestamps[i];
    }
    state.doppler_count = n;
    pthread_mutex_unlock(&state.lock);
}

/* ---- JSON serialization ---- */

static int build_json(char *buf, int bufsize)
{
    pthread_mutex_lock(&state.lock);

    int off = 0;
    off += snprintf(buf + off, bufsize - off,
                    "{\"total_ira\":%lu,\"total_ibc\":%lu,\"total_pages\":%lu,"
                    "\"total_beams\":%lu,\"total_mt\":%lu,\"total_aircraft\":%lu,",
                    state.total_ira, state.total_ibc, state.total_pages,
                    state.total_beams, state.total_mt, state.total_aircraft);

    /* Satellite orbital positions (most recent first, max 500) */
    off += snprintf(buf + off, bufsize - off, "\"ra\":[");
    int n_out = 0;
    for (int i = 0; i < state.ra_count && n_out < 500; i++) {
        int idx = (state.ra_head - 1 - i + MAX_RA_POINTS) % MAX_RA_POINTS;
        ra_point_t *p = &state.ra[idx];
        if (n_out > 0) off += snprintf(buf + off, bufsize - off, ",");
        off += snprintf(buf + off, bufsize - off,
            "{\"lat\":%.4f,\"lon\":%.4f,\"alt\":%d,"
            "\"sat\":%d,\"beam\":%d,\"pages\":%d,"
            "\"tmsi\":%u,\"freq\":%.0f,\"t\":%llu}",
            p->lat, p->lon, p->alt,
            p->sat_id, p->beam_id, p->n_pages,
            p->tmsi, p->frequency,
            (unsigned long long)(p->timestamp / 1000000000ULL));
        n_out++;
        if (off >= bufsize - 512) break;
    }
    off += snprintf(buf + off, bufsize - off, "],");

    /* Ground beam positions (most recent first, max 300) */
    off += snprintf(buf + off, bufsize - off, "\"beams\":[");
    n_out = 0;
    for (int i = 0; i < state.beam_count && n_out < 300; i++) {
        int idx = (state.beam_head - 1 - i + MAX_BEAM_POINTS) % MAX_BEAM_POINTS;
        beam_point_t *p = &state.beams[idx];
        if (n_out > 0) off += snprintf(buf + off, bufsize - off, ",");
        off += snprintf(buf + off, bufsize - off,
            "{\"lat\":%.4f,\"lon\":%.4f,\"alt\":%d,"
            "\"sat\":%d,\"beam\":%d,\"pages\":%d,"
            "\"tmsi\":%u,\"freq\":%.0f,\"t\":%llu}",
            p->lat, p->lon, p->alt,
            p->sat_id, p->beam_id, p->n_pages,
            p->tmsi, p->frequency,
            (unsigned long long)(p->timestamp / 1000000000ULL));
        n_out++;
        if (off >= bufsize - 512) break;
    }
    off += snprintf(buf + off, bufsize - off, "],");

    /* MT phone/terminal positions (most recent first, max 200) */
    off += snprintf(buf + off, bufsize - off, "\"mt\":[");
    n_out = 0;
    for (int i = 0; i < state.mt_count && n_out < 200; i++) {
        int idx = (state.mt_head - 1 - i + MAX_MT_POINTS) % MAX_MT_POINTS;
        mt_point_t *p = &state.mt[idx];
        if (n_out > 0) off += snprintf(buf + off, bufsize - off, ",");
        off += snprintf(buf + off, bufsize - off,
            "{\"lat\":%.4f,\"lon\":%.4f,\"alt\":%d,"
            "\"type\":%u,\"freq\":%.0f,\"t\":%llu}",
            p->lat, p->lon, p->alt,
            (unsigned)p->msg_type, p->frequency,
            (unsigned long long)(p->timestamp / 1000000000ULL));
        n_out++;
        if (off >= bufsize - 512) break;
    }
    off += snprintf(buf + off, bufsize - off, "],");

    /* Active satellites (only those seen in last 15 minutes) */
    uint64_t max_ts = 0;
    for (int i = 0; i < state.n_sats; i++) {
        if (state.sats[i].last_seen > max_ts)
            max_ts = state.sats[i].last_seen;
    }
    uint64_t sat_window = 15ULL * 60 * 1000000000ULL;  /* 15 minutes in ns */

    off += snprintf(buf + off, bufsize - off, "\"sats\":[");
    int first_sat = 1;
    for (int i = 0; i < state.n_sats; i++) {
        if (max_ts > sat_window && state.sats[i].last_seen < max_ts - sat_window)
            continue;
        if (!first_sat) off += snprintf(buf + off, bufsize - off, ",");
        off += snprintf(buf + off, bufsize - off,
            "{\"id\":%d,\"beam\":%d,\"count\":%d}",
            state.sats[i].sat_id, state.sats[i].beam_id,
            state.sats[i].count);
        first_sat = 0;
    }
    off += snprintf(buf + off, bufsize - off, "]");

    /* ACARS aircraft positions (ADS-C or beam-based) */
    off += snprintf(buf + off, bufsize - off, ",\"aircraft\":[");
    int first_ac = 1;
    for (int i = 0; i < state.n_aircraft; i++) {
        aircraft_entry_t *ac = &state.aircraft[i];
        if (ac->n_fixes == 0) continue;
        if (!first_ac) off += snprintf(buf + off, bufsize - off, ",");
        first_ac = 0;
        off += snprintf(buf + off, bufsize - off,
            "{\"reg\":\"%s\",\"flight\":\"%s\",\"oooi\":\"%s\","
            "\"sat\":%d,\"beam\":%d,\"freq\":%.0f,\"fixes\":[",
            ac->reg, ac->flight, ac->oooi,
            ac->sat_id, ac->beam_id, ac->frequency);
        for (int j = 0; j < ac->n_fixes; j++) {
            if (j > 0) off += snprintf(buf + off, bufsize - off, ",");
            off += snprintf(buf + off, bufsize - off,
                "{\"lat\":%.5f,\"lon\":%.5f,\"t\":%llu,\"alt\":%d,\"adsc\":%d}",
                ac->fix_lat[j], ac->fix_lon[j],
                (unsigned long long)ac->fix_t[j],
                ac->fix_alt[j], (int)ac->fix_adsc[j]);
        }
        off += snprintf(buf + off, bufsize - off, "]}");
        if (off >= bufsize - 512) break;
    }
    off += snprintf(buf + off, bufsize - off, "]");

    /* Receiver position estimate (Doppler positioning) */
    if (state.rx_valid) {
        off += snprintf(buf + off, bufsize - off,
            ",\"rx\":{\"lat\":%.6f,\"lon\":%.6f,\"hdop\":%.1f}",
            state.rx_lat, state.rx_lon, state.rx_hdop);
    }

    /* Recent ACARS messages (chronological feed) */
    off += snprintf(buf + off, bufsize - off,
        ",\"total_acars_msgs\":%lu,\"acars\":[", state.total_acars_msgs);
    int n_msgs = state.acars_count;
    int n_emit = 0;
    for (int i = n_msgs - 1; i >= 0 && off < bufsize - 1024; i--) {
        int idx = (state.acars_head - 1 - (n_msgs - 1 - i) + MAX_ACARS_MSGS)
                  % MAX_ACARS_MSGS;
        const acars_msg_t *m = &state.acars[idx];
        if (n_emit > 0)
            off += snprintf(buf + off, bufsize - off, ",");
        /* Escape quotes and backslashes in text/reg/flight */
        char esc_text[ACARS_TEXT_MAX * 2 + 1];
        int e = 0;
        for (int k = 0; m->text[k] && e < (int)sizeof(esc_text) - 2; k++) {
            char c = m->text[k];
            if (c == '"' || c == '\\') esc_text[e++] = '\\';
            if (c < 0x20 || c == 0x7f) c = ' ';
            esc_text[e++] = c;
        }
        esc_text[e] = '\0';
        off += snprintf(buf + off, bufsize - off,
            "{\"t\":%llu,\"reg\":\"%s\",\"flt\":\"%s\","
            "\"lbl\":\"%s\",\"ul\":%d,\"txt\":\"%s\"}",
            (unsigned long long)(m->timestamp / 1000000000ULL),
            m->reg, m->flight, m->label, m->ul, esc_text);
        n_emit++;
    }
    off += snprintf(buf + off, bufsize - off, "]");
    /* TEST/EVAL premium flight messages feed */
    off += snprintf(buf + off, bufsize - off,
        ",\"total_premium_msgs\":%lu,\"premium\":[", state.total_premium_msgs);
    int n_pr = state.premium_count;
    int pr_emit = 0;
    for (int i = n_pr - 1; i >= 0 && off < bufsize - 1024; i--) {
        int idx = (state.premium_head - 1 - (n_pr - 1 - i) + MAX_PREMIUM_MSGS)
                  % MAX_PREMIUM_MSGS;
        const premium_msg_t *m = &state.premium[idx];
        if (pr_emit > 0)
            off += snprintf(buf + off, bufsize - off, ",");
        char esc_text[ACARS_TEXT_MAX * 2 + 1];
        int e = 0;
        for (int k = 0; m->text[k] && e < (int)sizeof(esc_text) - 2; k++) {
            char c = m->text[k];
            if (c == '"' || c == '\\') esc_text[e++] = '\\';
            if (c < 0x20 || c == 0x7f) c = ' ';
            esc_text[e++] = c;
        }
        esc_text[e] = '\0';
        off += snprintf(buf + off, bufsize - off,
            "{\"t\":%llu,\"reg\":\"%s\",\"flt\":\"%s\","
            "\"lbl\":\"%s\",\"ul\":%d,\"txt\":\"%s\"}",
            (unsigned long long)(m->timestamp / 1000000000ULL),
            m->reg, m->flight, m->label, m->ul, esc_text);
        pr_emit++;
    }
    off += snprintf(buf + off, bufsize - off, "]");

    /* Recent pager (MSG) messages (chronological feed) */
    off += snprintf(buf + off, bufsize - off,
        ",\"total_msg\":%lu,\"msg\":[", state.total_msg);
    int n_pg = state.pager_count;
    int pg_emit = 0;
    for (int i = n_pg - 1; i >= 0 && off < bufsize - 1024; i--) {
        int idx = (state.pager_head - 1 - (n_pg - 1 - i) + MAX_PAGER_MSGS)
                  % MAX_PAGER_MSGS;
        const pager_msg_t *m = &state.pager[idx];
        if (pg_emit > 0)
            off += snprintf(buf + off, bufsize - off, ",");
        char esc[PAGER_TEXT_MAX * 2 + 1];
        int e = 0;
        for (int k = 0; m->text[k] && e < (int)sizeof(esc) - 2; k++) {
            char c = m->text[k];
            if (c == '"' || c == '\\') esc[e++] = '\\';
            if (c < 0x20 || c == 0x7f) c = ' ';
            esc[e++] = c;
        }
        esc[e] = '\0';
        off += snprintf(buf + off, bufsize - off,
            "{\"t\":%llu,\"ric\":%d,\"fmt\":%d,\"seq\":%d,\"ok\":%d,\"txt\":\"%s\"}",
            (unsigned long long)(m->timestamp / 1000000000ULL),
            m->ric, m->format, m->seq, m->csum_ok, esc);
        pg_emit++;
    }
    off += snprintf(buf + off, bufsize - off, "]");
    /* Active satellite Doppler summary (overview grid) */
    off += snprintf(buf + off, bufsize - off, ",\"doppler\":[");
    for (int i = 0; i < state.doppler_count && off < bufsize - 256; i++) {
        const sat_doppler_t *d = &state.doppler[i];
        if (i > 0)
            off += snprintf(buf + off, bufsize - off, ",");
        off += snprintf(buf + off, bufsize - off,
            "{\"sat\":%d,\"freq\":%.1f,\"t\":%llu}",
            d->sat_id, d->freq,
            (unsigned long long)(d->timestamp / 1000000000ULL));
    }
    off += snprintf(buf + off, bufsize - off, "]");

    /* Frame-type histogram */
    off += snprintf(buf + off, bufsize - off, ",\"ftypes\":{");
    for (int i = 0; i < N_FTYPES; i++) {
        off += snprintf(buf + off, bufsize - off, "%s\"%s\":%lu",
                        i ? "," : "", ftype_names[i], state.type_counts[i]);
    }
    off += snprintf(buf + off, bufsize - off, "}");

    /* UW-check failures by direction (bursts dropped before classification) */
    off += snprintf(buf + off, bufsize - off,
        ",\"uw_fail\":{\"DL\":%lu,\"UL\":%lu,\"other\":%lu},\"uw_ambiguous\":%lu",
        state.uw_fail[0], state.uw_fail[1], state.uw_fail[2], state.uw_ambiguous);

    off += snprintf(buf + off, bufsize - off, "}");

    pthread_mutex_unlock(&state.lock);
    return off;
}

/* ---- Doppler pass history feed (read-only) ----
 *
 * Publishes the per-satellite frequency/time history the positioning engine
 * already keeps for its own solver, so an external viewer can draw Doppler
 * S-curves. Strictly a reader: it calls the existing mutex-protected
 * accessors in doppler_pos.c and mutates no state.
 *
 * Emitted times are milliseconds RELATIVE to now_ms (negative = in the past).
 * Absolute CLOCK_REALTIME nanoseconds (~1.8e18) exceed the 2^53 range in
 * which JSON consumers can hold integers exactly, so they are not sent raw.
 */

/* Matches MEAS_PER_SAT, the per-satellite buffer depth in doppler_pos.c. */
#define DOPPLER_HIST_MAX  200
#define DOPPLER_BUF_SIZE  262144

static int build_doppler_json(char *buf, int bufsize)
{
    /* doppler_pos_init() only runs under --position; without it the module's
     * lock is never initialized. Report cleanly instead of reaching into an
     * uninitialized subsystem. */
    if (!position_enabled)
        return snprintf(buf, bufsize,
            "{\"enabled\":false,\"now_ms\":0,\"sats\":[],"
            "\"note\":\"Doppler positioning is off — "
            "restart iridium-sniffer with --position\"}");

    struct timespec rt;
    clock_gettime(CLOCK_REALTIME, &rt);
    uint64_t now_ns = (uint64_t)rt.tv_sec * 1000000000ULL + rt.tv_nsec;

    int sat_ids[MAX_DOPPLER_SATS];
    double latest_freq[MAX_DOPPLER_SATS];
    uint64_t latest_ts[MAX_DOPPLER_SATS];
    int n = doppler_pos_get_active_sats(sat_ids, latest_freq, latest_ts,
                                        now_ns, MAX_DOPPLER_SATS);

    int off = snprintf(buf, bufsize, "{\"enabled\":true,\"now_ms\":%llu,"
                       "\"sats\":[",
                       (unsigned long long)(now_ns / 1000000ULL));

    int emitted = 0;
    for (int i = 0; i < n && off < bufsize - 512; i++) {
        double freqs[DOPPLER_HIST_MAX];
        uint64_t stamps[DOPPLER_HIST_MAX];
        int h = doppler_pos_get_history(sat_ids[i], freqs, stamps,
                                        DOPPLER_HIST_MAX);
        if (h <= 0)
            continue;

        off += snprintf(buf + off, bufsize - off, "%s{\"sat\":%d,\"n\":%d,"
                        "\"dt\":[", emitted ? "," : "", sat_ids[i], h);
        for (int j = 0; j < h && off < bufsize - 64; j++)
            off += snprintf(buf + off, bufsize - off, "%s%lld", j ? "," : "",
                            ((long long)stamps[j] - (long long)now_ns)
                                / 1000000LL);
        off += snprintf(buf + off, bufsize - off, "],\"f\":[");
        for (int j = 0; j < h && off < bufsize - 64; j++)
            off += snprintf(buf + off, bufsize - off, "%s%.1f", j ? "," : "",
                            freqs[j]);
        off += snprintf(buf + off, bufsize - off, "]}");
        emitted++;
    }
    off += snprintf(buf + off, bufsize - off, "]}");
    return off;
}

/* ---- Embedded HTML/JS ---- */

static const char HTML_PAGE[] =
"<!DOCTYPE html>\n"
"<html><head>\n"
"<meta charset=\"utf-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
"<title>iridium-sniffer</title>\n"
"<link rel=\"stylesheet\" href=\"https://unpkg.com/leaflet@1.9.4/dist/leaflet.css\">\n"
"<script src=\"https://unpkg.com/leaflet@1.9.4/dist/leaflet.js\"></script>\n"
"<style>\n"
"*{margin:0;padding:0;box-sizing:border-box}\n"
"body{font-family:system-ui,-apple-system,sans-serif;background:#0f172a}\n"
"#map{width:100vw;height:calc(100vh - 44px)}\n"
"#bar{height:44px;background:#1e293b;color:#e2e8f0;display:flex;\n"
"  align-items:center;padding:0 16px;gap:20px;font-size:13px;\n"
"  border-bottom:1px solid #334155}\n"
"#bar .title{font-weight:600;color:#f8fafc;letter-spacing:0.5px}\n"
".stat{color:#94a3b8}\n"
".val{color:#38bdf8;font-weight:600;font-variant-numeric:tabular-nums}\n"
"#status{margin-left:auto;font-size:12px}\n"
".leaflet-popup-content{font-family:'SF Mono',Consolas,monospace;\n"
"  font-size:12px;line-height:1.6}\n"
".popup-title{font-weight:700;font-size:13px;margin-bottom:4px;\n"
"  padding-bottom:4px;border-bottom:1px solid #e2e8f0}\n"
".popup-page{color:#dc2626;font-weight:600}\n"
".popup-mt{color:#f59e0b;font-weight:600}\n"
".popup-ac{font-weight:600}\n"
".legend{position:absolute;bottom:28px;right:10px;z-index:1000;\n"
"  background:rgba(15,23,42,0.92);color:#e2e8f0;padding:10px 14px;\n"
"  border-radius:6px;font-size:12px;line-height:2;\n"
"  border:1px solid #334155;pointer-events:auto}\n"
".legend-title{font-weight:700;font-size:11px;text-transform:uppercase;\n"
"  letter-spacing:1px;color:#94a3b8;margin-bottom:2px}\n"
".legend-row{display:flex;align-items:center;gap:8px}\n"
".legend-swatch{display:inline-block}\n"
"#ftypes-box{top:10px;bottom:auto;line-height:1.5;min-width:120px}\n"
"#ftypes .legend-row{justify-content:space-between;gap:14px}\n"
"#ftypes .ftc{font-weight:600;color:#38bdf8;font-variant-numeric:tabular-nums}\n"
".leaflet-container{background:#0f172a}\n"
".leaflet-control-layers{background:rgba(15,23,42,0.92)!important;\n"
"  color:#e2e8f0!important;border:1px solid #334155!important}\n"
".leaflet-control-layers label{color:#e2e8f0}\n"
"#main{display:flex;width:100vw;height:calc(100vh - 44px)}\n"
"#map{flex:1;height:100%;width:auto;position:relative}\n"
"#acars-side{width:380px;height:100%;background:#0f172a;\n"
"  border-left:1px solid #334155;overflow-y:auto;color:#e2e8f0;\n"
"  font-family:'SF Mono',Consolas,monospace;font-size:11px}\n"
"#acars-side .header{padding:8px 12px;background:#1e293b;\n"
"  border-bottom:1px solid #334155;font-size:11px;\n"
"  text-transform:uppercase;letter-spacing:1px;color:#94a3b8;\n"
"  font-weight:600;position:sticky;top:0;z-index:10}\n"
"#acars-side .header .hint{display:block;font-size:9px;\n"
"  text-transform:none;letter-spacing:0;color:#64748b;\n"
"  font-weight:400;margin-top:2px}\n"
"#acars-side .tabs{display:flex;position:sticky;top:0;z-index:11;\n"
"  background:#1e293b;border-bottom:1px solid #334155}\n"
"#acars-side .tab{flex:1;text-align:center;padding:7px 0;cursor:pointer;\n"
"  font-size:10px;text-transform:uppercase;letter-spacing:1px;\n"
"  color:#64748b;font-weight:600;border-bottom:2px solid transparent}\n"
"#acars-side .tab.active{color:#38bdf8;border-bottom-color:#38bdf8}\n"
"html.light #acars-side .tabs{background:#f1f5f9;border-bottom-color:#cbd5e1}\n"
".amsg{padding:6px 10px;border-bottom:1px solid #1e293b;line-height:1.5}\n"
".amsg:hover{background:#1e293b}\n"
".amsg .row1{display:flex;gap:8px;color:#64748b;font-size:10px;\n"
"  margin-bottom:2px}\n"
".amsg .reg{color:#38bdf8;font-weight:600}\n"
".amsg .flt{color:#94a3b8}\n"
".amsg .lbl{color:#fbbf24}\n"
".amsg .dir{color:#475569}\n"
".amsg .txt{color:#e2e8f0;white-space:pre-wrap;word-break:break-word}\n"
".no-msgs{text-align:center;color:#64748b;padding:32px 12px;font-size:11px}\n"
".btn-bar{background:#334155;color:#e2e8f0;border:1px solid #475569;\n"
"  border-radius:4px;padding:2px 8px;cursor:pointer;font-size:11px;\n"
"  margin-left:4px}\n"
"#theme-toggle{background:transparent;color:inherit;\n"
"  border:1px solid currentColor;border-radius:4px;padding:0 8px;\n"
"  cursor:pointer;font-size:14px;line-height:24px;height:26px;\n"
"  opacity:0.6;margin-left:4px}\n"
"#theme-toggle:hover{opacity:1}\n"
/* ---- Light theme overrides (gated by html.light, dark stays default) ---- */
"html.light body{background:#f8fafc;color:#1e293b}\n"
"html.light #bar{background:#ffffff;color:#1e293b;\n"
"  border-bottom-color:#cbd5e1}\n"
"html.light #bar .title{color:#0f172a}\n"
"html.light .stat{color:#475569}\n"
"html.light .val{color:#0284c7}\n"
"html.light .leaflet-container{background:#f8fafc}\n"
"html.light .leaflet-control-layers{background:rgba(255,255,255,0.95)!important;\n"
"  color:#1e293b!important;border-color:#cbd5e1!important}\n"
"html.light .leaflet-control-layers label{color:#1e293b}\n"
"html.light .legend{background:rgba(255,255,255,0.95);color:#1e293b;\n"
"  border-color:#cbd5e1}\n"
"html.light .legend-title{color:#475569}\n"
"html.light .popup-title{border-bottom-color:#cbd5e1}\n"
"html.light #acars-side{background:#ffffff;color:#1e293b;\n"
"  border-left-color:#cbd5e1}\n"
"html.light #acars-side .header{background:#f1f5f9;color:#475569;\n"
"  border-bottom-color:#cbd5e1}\n"
"html.light #acars-side .header .hint{color:#64748b}\n"
"html.light .amsg{border-bottom-color:#e2e8f0}\n"
"html.light .amsg:hover{background:#f1f5f9}\n"
"html.light .amsg .row1{color:#64748b}\n"
"html.light .amsg .reg{color:#0284c7}\n"
"html.light .amsg .flt{color:#475569}\n"
"html.light .amsg .lbl{color:#b45309}\n"
"html.light .amsg .dir{color:#94a3b8}\n"
"html.light .amsg .txt{color:#1e293b}\n"
"html.light .no-msgs{color:#94a3b8}\n"
"html.light .btn-bar{background:#e2e8f0;color:#1e293b;\n"
"  border-color:#cbd5e1}\n"
"</style></head><body>\n"
"<div id=\"bar\">\n"
"  <span class=\"title\">iridium-sniffer</span>\n"
"  <span class=\"stat\">Beams <span id=\"n-beams\" class=\"val\">0</span></span>\n"
"  <span class=\"stat\">MT <span id=\"n-mt\" class=\"val\">0</span></span>\n"
"  <span class=\"stat\">Aircraft <span id=\"n-ac\" class=\"val\">0</span></span>\n"
"  <span class=\"stat\">Pages <span id=\"n-pages\" class=\"val\">0</span></span>\n"
"  <button id=\"btn-export\" class=\"btn-bar\" onclick=\"exportPages()\">Export Pages CSV</button>\n"
"  <span class=\"stat\">Sats <span id=\"n-sats\" class=\"val\">0</span></span>\n"
"  <span class=\"stat\">IRA <span id=\"n-ira\" class=\"val\">0</span></span>\n"
"  <span class=\"stat\">ACARS <span id=\"n-acars\" class=\"val\">0</span></span>\n"
"  <span class=\"stat\">MSG <span id=\"n-msg\" class=\"val\">0</span></span>\n"
"  <button id=\"theme-toggle\" onclick=\"toggleTheme()\" title=\"Toggle light/dark\">\\u263E</button>\n"
"  <span id=\"status\" style=\"color:#64748b\">connecting...</span>\n"
"</div>\n"
"<div id=\"main\">\n"
"  <div id=\"map\">\n"
"<div class=\"legend\">\n"
"  <div class=\"legend-title\">Map</div>\n"
"  <div class=\"legend-row\">\n"
"    <span class=\"legend-swatch\" style=\"width:16px;height:16px;\n"
"      border-radius:50%;border:1px solid #3b82f6;\n"
"      background:rgba(59,130,246,0.12)\"></span>\n"
"    Beam footprint\n"
"  </div>\n"
"  <div class=\"legend-row\">\n"
"    <span class=\"legend-swatch\" style=\"width:10px;height:10px;\n"
"      border-radius:50%;background:#f59e0b\"></span>\n"
"    MT position\n"
"  </div>\n"
"  <div class=\"legend-row\">\n"
"    <span class=\"legend-swatch\" style=\"width:10px;height:10px;\n"
"      border-radius:50%;background:#22d3ee\"></span>\n"
"    Aircraft (ACARS beam)\n"
"  </div>\n"
"  <div class=\"legend-row\">\n"
"    <span class=\"legend-swatch\" style=\"width:10px;height:10px;\n"
"      border-radius:50%;background:#ef4444\"></span>\n"
"    Paging event\n"
"  </div>\n"
"  <div class=\"legend-row\">\n"
"    <span class=\"legend-swatch\" style=\"width:10px;height:10px;\n"
"      border-radius:50%;background:#22c55e\"></span>\n"
"    Receiver position\n"
"  </div>\n"
"</div>\n"
"<div class=\"legend\" id=\"ftypes-box\">\n"
"  <div class=\"legend-title\">Frame types</div>\n"
"  <div id=\"ftypes\"><div class=\"legend-row\">waiting...</div></div>\n"
"</div>\n"
"  </div>\n"
"  <div id=\"acars-side\">\n"
"    <div class=\"tabs\">\n"
"      <span id=\"tab-acars\" class=\"tab active\" onclick=\"showTab('acars')\">ACARS</span>\n"
"      <span id=\"tab-msg\" class=\"tab\" onclick=\"showTab('msg')\">Pager</span>\n"
"      <span id=\"tab-premium\" class=\"tab\" onclick=\"showTab('premium')\">AT1</span>\n"
"    </div>\n"
"    <div id=\"pane-acars\">\n"
"      <div class=\"header\">Recent ACARS<span class=\"hint\">heartbeats filtered</span></div>\n"
"      <div id=\"acars-list\">\n"
"        <div class=\"no-msgs\">No ACARS messages yet<br>(heartbeats filtered)</div>\n"
"      </div>\n"
"    </div>\n"
"    <div id=\"pane-msg\" style=\"display:none\">\n"
"      <div class=\"header\">Pager messages<span class=\"hint\">simplex MSG/IMS channel</span></div>\n"
"      <div id=\"msg-list\">\n"
"        <div class=\"no-msgs\">No pager messages yet</div>\n"
"      </div>\n"
"    </div>\n"
"    <div id=\"pane-premium\" style=\"display:none\">\n"
"      <div class=\"header\">AT1<span class=\"hint\">structured flight messages</span></div>\n"
"      <div id=\"premium-list\">\n"
"        <div class=\"no-msgs\">No messages yet</div>\n"
"      </div>\n"
"    </div>\n"
"  </div>\n"
"</div>\n"
"<script>\n"
"var map=L.map('map',{zoomControl:true}).setView([20,0],2);\n"
"var tileLayer=null;\n"
"function setTheme(name){\n"
"  var html=document.documentElement;\n"
"  if(name==='light'){html.classList.add('light')}else{html.classList.remove('light')}\n"
"  try{localStorage.setItem('theme',name)}catch(e){}\n"
"  if(tileLayer)map.removeLayer(tileLayer);\n"
"  var url=(name==='light')?'https://{s}.basemaps.cartocdn.com/light_all/{z}/{x}/{y}{r}.png'\n"
"                          :'https://{s}.basemaps.cartocdn.com/dark_all/{z}/{x}/{y}{r}.png';\n"
"  tileLayer=L.tileLayer(url,{attribution:'CartoDB',maxZoom:18,\n"
"    subdomains:'abcd'}).addTo(map);\n"
"  var btn=document.getElementById('theme-toggle');\n"
"  if(btn)btn.textContent=(name==='light')?'\\u263C':'\\u263E';\n"
"}\n"
"function toggleTheme(){\n"
"  var cur=document.documentElement.classList.contains('light')?'light':'dark';\n"
"  setTheme(cur==='light'?'dark':'light');\n"
"}\n"
"setTheme((function(){try{return localStorage.getItem('theme')||'dark'}catch(e){return 'dark'}})());\n"
"\n"
"var C=[\n"
"  '#3b82f6','#22d3ee','#10b981','#a78bfa','#f472b6',\n"
"  '#fb923c','#facc15','#4ade80','#818cf8','#f87171',\n"
"  '#2dd4bf','#c084fc','#38bdf8','#fb7185','#a3e635'];\n"
"function sc(id){return C[id%C.length]}\n"
"\n"
"function fmtTime(t){\n"
"  var d=new Date(t*1000);\n"
"  var h=String(d.getUTCHours()).padStart(2,'0');\n"
"  var m=String(d.getUTCMinutes()).padStart(2,'0');\n"
"  var s=String(d.getUTCSeconds()).padStart(2,'0');\n"
"  return h+':'+m+':'+s+'Z';\n"
"}\n"
"\n"
"function renderAcars(msgs){\n"
"  var box=document.getElementById('acars-list');\n"
"  if(!msgs||msgs.length===0){\n"
"    box.innerHTML='<div class=\"no-msgs\">No ACARS messages yet<br>(heartbeats filtered)</div>';\n"
"    return;\n"
"  }\n"
"  var html='';\n"
"  msgs.forEach(function(m){\n"
"    var txt=(m.txt||'').replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');\n"
"    html+='<div class=\"amsg\">'+\n"
"      '<div class=\"row1\">'+\n"
"      '<span>'+fmtTime(m.t)+'</span>'+\n"
"      '<span class=\"reg\">'+m.reg+'</span>'+\n"
"      (m.flt?'<span class=\"flt\">'+m.flt+'</span>':'')+\n"
"      '<span class=\"lbl\">'+m.lbl+'</span>'+\n"
"      '<span class=\"dir\">'+(m.ul?'UL':'DL')+'</span>'+\n"
"      '</div>'+\n"
"      '<div class=\"txt\">'+txt+'</div></div>';\n"
"  });\n"
"  box.innerHTML=html;\n"
"}\n"
"\n"
"function renderFtypes(ft,uw){\n"
"  var box=document.getElementById('ftypes');\n"
"  if(!ft){return;}\n"
"  var keys=Object.keys(ft);\n"
"  var total=0; keys.forEach(function(k){total+=ft[k];});\n"
"  var html='';\n"
"  if(uw){\n"
"    var ulc=(uw.UL>0)?'#f87171':'#64748b';\n"
"    html+='<div class=\"legend-row\" title=\"bursts demodulated but no unique word matched (pre-classification)\">'+\n"
"      '<span>UW-fail DL/UL</span><span class=\"ftc\">'+\n"
"      '<span style=\"color:#64748b\">'+(uw.DL||0)+'</span>/'+\n"
"      '<span style=\"color:'+ulc+'\">'+(uw.UL||0)+'</span></span></div>';\n"
"  }\n"
"  keys.forEach(function(k){\n"
"    if(ft[k]>0){\n"
"      var c=(k==='IDA_UL_FAIL')?'#f87171':'#38bdf8';\n"
"      html+='<div class=\"legend-row\"><span>'+k+'</span>'+\n"
"        '<span class=\"ftc\" style=\"color:'+c+'\">'+ft[k]+'</span></div>';\n"
"    }\n"
"  });\n"
"  box.innerHTML=html||'<div class=\"legend-row\">waiting...</div>';\n"
"}\n"
"\n"
"function showTab(name){\n"
"  var panes=['acars','msg','premium'];\n"
"  panes.forEach(function(p){\n"
"    document.getElementById('pane-'+p).style.display=(p===name?'':'none');\n"
"    document.getElementById('tab-'+p).classList.toggle('active',p===name);\n"
"  });\n"
"}\n"
"\n"
"function renderMsg(msgs){\n"
"  var box=document.getElementById('msg-list');\n"
"  if(!msgs||msgs.length===0){\n"
"    box.innerHTML='<div class=\"no-msgs\">No pager messages yet</div>';\n"
"    return;\n"
"  }\n"
"  var html='';\n"
"  msgs.forEach(function(m){\n"
"    var txt=(m.txt||'').replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');\n"
"    html+='<div class=\"amsg\">'+\n"
"      '<div class=\"row1\">'+\n"
"      '<span>'+fmtTime(m.t)+'</span>'+\n"
"      '<span class=\"reg\">RIC '+m.ric+'</span>'+\n"
"      '<span class=\"lbl\">'+(m.fmt===3?'BCD':'ASCII')+'</span>'+\n"
"      '<span class=\"dir\">'+(m.ok?'C:OK':'C:--')+'</span>'+\n"
"      '</div>'+\n"
"      '<div class=\"txt\">'+txt+'</div></div>';\n"
"  });\n"
"  box.innerHTML=html;\n"
"}\n"

"function renderPremium(msgs){\n"
"  var box=document.getElementById('premium-list');\n"
"  if(!msgs||msgs.length===0){\n"
"    box.innerHTML='<div class=\"no-msgs\">No messages yet</div>';\n"
"    return;\n"
"  }\n"
"  var html='';\n"
"  msgs.forEach(function(m){\n"
"    var txt=(m.txt||'').replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');\n"
"    var dir=m.ul?'UL':'DL';\n"
"    html+='<div class=\"amsg\">'+\n"
"      '<div class=\"row1\">'+\n"
"      '<span>'+fmtTime(m.t)+'</span>'+\n"
"      '<span class=\"reg\">'+m.reg+'</span>'+\n"
"      (m.flt?'<span class=\"flt\">'+m.flt+'</span>':'')+\n"
"      '<span class=\"lbl\">'+m.lbl+'</span>'+\n"
"      '<span class=\"dir\">'+dir+'</span>'+\n"
"      '</div>'+\n"
"      '<div class=\"txt\">'+txt+'</div></div>';\n"
"  });\n"
"  box.innerHTML=html;\n"
"}\n"
"\n"
"var allPages=[];\n"
"function exportPages(){\n"
"  if(allPages.length===0){alert('No paging events collected yet.');return;}\n"
"  var csv='timestamp,tmsi_hex,satellite,beam,lat,lon\\n';\n"
"  allPages.forEach(function(p){\n"
"    var d=new Date(p.t*1000);\n"
"    csv+=d.toISOString()+',0x'+(p.tmsi>>>0).toString(16).toUpperCase()\n"
"      +','+p.sat+','+p.beam+','+p.lat.toFixed(4)+','+p.lon.toFixed(4)+'\\n';\n"
"  });\n"
"  var blob=new Blob([csv],{type:'text/csv'});\n"
"  var a=document.createElement('a');\n"
"  a.href=URL.createObjectURL(blob);\n"
"  a.download='iridium_pages_'+Date.now()+'.csv';\n"
"  a.click();\n"
"}\n"
"\n"
"var beamLy=L.layerGroup().addTo(map);\n"
"var mtLy=L.layerGroup().addTo(map);\n"
"var acarsLy=L.layerGroup();\n"
"var pageLy=L.layerGroup().addTo(map);\n"
"var rxLy=L.layerGroup().addTo(map);\n"
"var satLy=L.layerGroup();\n"
"var coverLy=L.layerGroup();\n"
"L.control.layers(null,{\n"
"  'Beam footprints':beamLy,'MT positions':mtLy,\n"
"  'Aircraft (ACARS)':acarsLy,\n"
"  'Paging events':pageLy,'Receiver':rxLy,\n"
"  'Satellite tracks':satLy,'Sat coverage':coverLy\n"
"},{collapsed:false,position:'bottomleft'}).addTo(map);\n"
"\n"
"var centered=false;\n"
"var TW=300;\n"
"var popupOpen=false;\n"
"map.on('popupopen',function(){popupOpen=true});\n"
"map.on('popupclose',function(){popupOpen=false});\n"
"\n"
"function update(d){\n"
"  document.getElementById('n-ira').textContent=d.total_ira;\n"
"  document.getElementById('n-beams').textContent=d.total_beams||0;\n"
"  document.getElementById('n-mt').textContent=d.total_mt||0;\n"
"  document.getElementById('n-ac').textContent=d.total_aircraft||0;\n"
"  document.getElementById('n-pages').textContent=d.total_pages;\n"
"  document.getElementById('n-acars').textContent=d.total_acars_msgs||0;\n"
"  document.getElementById('n-msg').textContent=d.total_msg||0;\n"
"  document.getElementById('status').style.color='#22c55e';\n"
"  document.getElementById('status').textContent='live';\n"
"  renderAcars(d.acars||[]);\n"
"  renderMsg(d.msg||[]);\n"
"  renderPremium(d.premium||[]);\n"
"  renderFtypes(d.ftypes,d.uw_fail);\n"
"\n"
"  if(popupOpen)return;\n"
"\n"
"  beamLy.clearLayers();\n"
"  mtLy.clearLayers();\n"
"  acarsLy.clearLayers();\n"
"  pageLy.clearLayers();\n"
"  satLy.clearLayers();\n"
"  coverLy.clearLayers();\n"
"\n"
"  var now=0;\n"
"  if(d.beams)d.beams.forEach(function(p){if(p.t>now)now=p.t});\n"
"  if(d.ra)d.ra.forEach(function(p){if(p.t>now)now=p.t});\n"
"  if(d.mt)d.mt.forEach(function(p){if(p.t>now)now=p.t});\n"
"  if(d.aircraft)d.aircraft.forEach(function(ac){\n"
"    if(ac.fixes)ac.fixes.forEach(function(f){if(f.t>now)now=f.t});\n"
"  });\n"
"  var cut=now-TW;\n"
"\n"
"  /* --- Ground beam footprints (primary) --- */\n"
"  var nBeamSat=0;\n"
"  if(d.beams&&d.beams.length>0){\n"
"    var bySat={};\n"
"    d.beams.forEach(function(p){\n"
"      if(p.t<cut)return;\n"
"      if(!bySat[p.sat])bySat[p.sat]=[];\n"
"      bySat[p.sat].push(p);\n"
"      if(p.pages>0){\n"
"        var isDup=allPages.some(function(e){return e.t===p.t&&e.tmsi===p.tmsi&&e.sat===p.sat});\n"
"        if(!isDup)allPages.push({t:p.t,tmsi:p.tmsi,sat:p.sat,beam:p.beam,lat:p.lat,lon:p.lon});\n"
"        var pm=L.circleMarker([p.lat,p.lon],{\n"
"          radius:7,color:'#ef4444',fillColor:'#ef4444',\n"
"          fillOpacity:0.8,weight:2\n"
"        });\n"
"        pm.bindPopup('<div class=\"popup-title popup-page\">Paging</div>'\n"
"          +'Satellite: '+p.sat+'<br>'\n"
"          +'Beam: '+p.beam+'<br>'\n"
"          +'TMSI: 0x'+(p.tmsi>>>0).toString(16).toUpperCase()+'<br>'\n"
"          +'Position: '+p.lat.toFixed(4)+', '+p.lon.toFixed(4));\n"
"        pm.addTo(pageLy);\n"
"      }\n"
"    });\n"
"    Object.keys(bySat).forEach(function(sid){\n"
"      var pts=bySat[sid].sort(function(a,b){return a.t-b.t});\n"
"      if(!pts.length)return;\n"
"      nBeamSat++;\n"
"      var col=sc(parseInt(sid));\n"
"      pts.forEach(function(pt){\n"
"        var age=(now-pt.t)/TW;\n"
"        L.circle([pt.lat,pt.lon],{radius:200000,\n"
"          stroke:true,color:col,weight:1,\n"
"          fillColor:col,fillOpacity:0.10*(1-age)\n"
"        }).addTo(beamLy);\n"
"      });\n"
"      var last=pts[pts.length-1];\n"
"      var m=L.circleMarker([last.lat,last.lon],{\n"
"        radius:5,color:col,fillColor:col,fillOpacity:0.9,weight:2\n"
"      });\n"
"      m.bindTooltip('Sat '+sid+' B'+last.beam,\n"
"        {direction:'top',offset:[0,-8]});\n"
"      m.bindPopup('<div class=\"popup-title\">Beam Center</div>'\n"
"        +'Satellite: '+sid+'<br>'\n"
"        +'Beam: '+last.beam+'<br>'\n"
"        +'Position: '+last.lat.toFixed(4)+', '+last.lon.toFixed(4)+'<br>'\n"
"        +'Frequency: '+last.freq.toFixed(0)+' Hz');\n"
"      m.addTo(beamLy);\n"
"    });\n"
"  }\n"
"  document.getElementById('n-sats').textContent=nBeamSat;\n"
"\n"
"  /* --- MT phone/terminal positions --- */\n"
"  if(d.mt&&d.mt.length>0){\n"
"    d.mt.forEach(function(p){\n"
"      if(p.t<cut)return;\n"
"      var age=(now-p.t)/TW;\n"
"      var ts=p.type==0x0605?'GSM Page':\n"
"             p.type==0x7605?'SBD Page':\n"
"             p.type==0x0600?'Uplink':'Unknown';\n"
"      var pm=L.circleMarker([p.lat,p.lon],{\n"
"        radius:5,color:'#f59e0b',fillColor:'#f59e0b',\n"
"        fillOpacity:0.8*(1-age*0.5),weight:2\n"
"      });\n"
"      pm.bindPopup('<div class=\"popup-title popup-mt\">MT Position</div>'\n"
"        +'Type: '+ts+'<br>'\n"
"        +'Position: '+p.lat.toFixed(4)+', '+p.lon.toFixed(4)+'<br>'\n"
"        +'Frequency: '+p.freq.toFixed(0)+' Hz');\n"
"      pm.addTo(mtLy);\n"
"    });\n"
"  }\n"
"\n"
"  /* --- Aircraft beam-based positions (ACARS) --- */\n"
"  var TW_AC=3600;\n"
"  function hav(a,b){\n"
"    var R=6371,dr=(b.lat-a.lat)*Math.PI/180,dl=(b.lon-a.lon)*Math.PI/180;\n"
"    var s=Math.sin(dr/2)*Math.sin(dr/2)+Math.cos(a.lat*Math.PI/180)*Math.cos(b.lat*Math.PI/180)*Math.sin(dl/2)*Math.sin(dl/2);\n"
"    return R*2*Math.atan2(Math.sqrt(s),Math.sqrt(1-s));\n"
"  }\n"
"  if(d.aircraft&&d.aircraft.length>0){\n"
"    d.aircraft.forEach(function(ac,idx){\n"
"      if(!ac.fixes||ac.fixes.length===0)return;\n"
"      var last=ac.fixes[ac.fixes.length-1];\n"
"      if(now-last.t>TW_AC)return;\n"
"      var col=sc(idx);\n"
"      if(ac.fixes.length>1){\n"
"        /* Split track into segments; skip any jump > 2500 km (satellite beam shift) */\n"
"        var segs=[],cur=[ac.fixes[0]];\n"
"        for(var i=1;i<ac.fixes.length;i++){\n"
"          if(hav(ac.fixes[i-1],ac.fixes[i])<2500){\n"
"            cur.push(ac.fixes[i]);\n"
"          }else{\n"
"            if(cur.length>1)segs.push(cur);\n"
"            cur=[ac.fixes[i]];\n"
"          }\n"
"        }\n"
"        if(cur.length>1)segs.push(cur);\n"
"        segs.forEach(function(seg){\n"
"          L.polyline(seg.map(function(f){return[f.lat,f.lon];}),\n"
"            {color:col,weight:2,opacity:0.5,dashArray:'6,4'}).addTo(acarsLy);\n"
"        });\n"
"      }\n"
"      var label=ac.reg+(ac.flight?' / '+ac.flight:'')+(ac.oooi?' ['+ac.oooi+']':'');\n"
"      var m=L.circleMarker([last.lat,last.lon],{\n"
"        radius:7,color:col,fillColor:col,fillOpacity:0.9,weight:2\n"
"      });\n"
"      m.bindTooltip(label,{direction:'top',offset:[0,-8]});\n"
"      var ts=new Date(last.t*1000).toUTCString().replace(/.*?([0-9]{2}:[0-9]{2}:[0-9]{2}).*/,'$1')+' UTC';\n"
"      var altStr=last.alt>-99999?'<b>Altitude:</b> '+(last.alt).toLocaleString()+' ft<br>':'';\n"
"      var posStr=last.adsc?'<span style=\"color:#22c55e;font-size:11px\">ADS-C GPS position</span>':'<span style=\"color:#94a3b8;font-size:11px\">~200 km beam estimate</span>';\n"
"      var ooiStr=ac.oooi?'<b>Event:</b> <span style=\"color:#f59e0b;font-weight:600\">'+ac.oooi+'</span><br>':'';\n"
"      m.bindPopup('<div class=\"popup-title popup-ac\" style=\"color:'+col+'\">Aircraft</div>'\n"
"        +'<b>Reg:</b> '+ac.reg+'<br>'\n"
"        +(ac.flight?'<b>Flight:</b> '+ac.flight+'<br>':'')\n"
"        +ooiStr\n"
"        +altStr\n"
"        +'<b>Sat:</b> '+ac.sat+'  Beam: '+ac.beam+'<br>'\n"
"        +'<b>Position:</b> '+last.lat.toFixed(last.adsc?4:2)+', '+last.lon.toFixed(last.adsc?4:2)+'<br>'\n"
"        +'<b>Last msg:</b> '+ts+'<br>'\n"
"        +posStr);\n"
"      m.addTo(acarsLy);\n"
"    });\n"
"  }\n"
"\n"
"  /* --- Satellite orbital tracks (toggleable, off by default) --- */\n"
"  if(d.ra&&d.ra.length>0){\n"
"    var satBySat={};\n"
"    d.ra.forEach(function(p){\n"
"      if(p.t<cut)return;\n"
"      if(!satBySat[p.sat])satBySat[p.sat]=[];\n"
"      satBySat[p.sat].push(p);\n"
"    });\n"
"    Object.keys(satBySat).forEach(function(sid){\n"
"      var pts=satBySat[sid].sort(function(a,b){return a.t-b.t});\n"
"      if(!pts.length)return;\n"
"      var col=sc(parseInt(sid));\n"
"      pts.forEach(function(pt){\n"
"        var age=(now-pt.t)/TW;\n"
"        L.circle([pt.lat,pt.lon],{radius:400000,\n"
"          stroke:false,fillColor:col,fillOpacity:0.08*(1-age)\n"
"        }).addTo(coverLy);\n"
"      });\n"
"      var last=pts[pts.length-1];\n"
"      var m=L.circleMarker([last.lat,last.lon],{\n"
"        radius:4,color:'#64748b',fillColor:'#64748b',\n"
"        fillOpacity:0.7,weight:1\n"
"      });\n"
"      m.bindTooltip('Sat '+sid+' (orbit)',\n"
"        {direction:'top',offset:[0,-8]});\n"
"      m.bindPopup('<div class=\"popup-title\">Satellite '+sid+'</div>'\n"
"        +'Beam: '+last.beam+'<br>'\n"
"        +'Position: '+last.lat.toFixed(2)+', '+last.lon.toFixed(2)+'<br>'\n"
"        +'Altitude: '+last.alt+' km<br>'\n"
"        +'Frequency: '+last.freq.toFixed(0)+' Hz');\n"
"      m.addTo(satLy);\n"
"    });\n"
"  }\n"
"\n"
"  /* --- Receiver position --- */\n"
"  rxLy.clearLayers();\n"
"  if(d.rx){\n"
"    var rm=L.circleMarker([d.rx.lat,d.rx.lon],{\n"
"      radius:8,color:'#22c55e',fillColor:'#22c55e',\n"
"      fillOpacity:0.9,weight:3\n"
"    });\n"
"    rm.bindPopup('<div class=\"popup-title\">Receiver Position</div>'\n"
"      +'Estimated: '+d.rx.lat.toFixed(6)+', '+d.rx.lon.toFixed(6)+'<br>'\n"
"      +'HDOP: '+d.rx.hdop.toFixed(1));\n"
"    rm.addTo(rxLy);\n"
"    if(d.rx.hdop<50){\n"
"      L.circle([d.rx.lat,d.rx.lon],{radius:d.rx.hdop*20,\n"
"        color:'#22c55e',fillColor:'#22c55e',\n"
"        fillOpacity:0.1,weight:1,dashArray:'4'}).addTo(rxLy);\n"
"    }\n"
"  }\n"
"\n"
"  /* --- Auto-center --- */\n"
"  if(!centered){\n"
"    if(d.rx){\n"
"      map.setView([d.rx.lat,d.rx.lon],8);\n"
"      centered=true;\n"
"    }else if(d.beams&&d.beams.length>0){\n"
"      map.setView([d.beams[0].lat,d.beams[0].lon],8);\n"
"      centered=true;\n"
"    }else if(d.ra&&d.ra.length>0){\n"
"      map.setView([d.ra[0].lat,d.ra[0].lon],3);\n"
"      centered=true;\n"
"    }\n"
"  }\n"
"}\n"
"\n"
"function connect(){\n"
"  var base=window.location.href.split('#')[0].split('?')[0].replace(/\\/?$/,'/');\n"
"  var es=new EventSource(base+'api/events');\n"
"  es.addEventListener('update',function(e){\n"
"    try{update(JSON.parse(e.data))}catch(err){}\n"
"  });\n"
"  es.onerror=function(){\n"
"    document.getElementById('status').style.color='#ef4444';\n"
"    document.getElementById('status').textContent='reconnecting...';\n"
"    es.close();\n"
"    setTimeout(connect,2000);\n"
"  };\n"
"}\n"
"connect();\n"
"</script></body></html>\n";

/* ---- HTTP request handling ---- */

static void send_response(int fd, const char *status, const char *content_type,
                            const char *body, int body_len)
{
    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n", status, content_type, body_len);
    if (write(fd, header, hlen) < 0) return;
    if (body_len > 0) {
        if (write(fd, body, body_len) < 0) return;
    }
}

static void handle_sse(int fd)
{
    char header[256];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: keep-alive\r\n"
        "X-Accel-Buffering: no\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n");
    if (write(fd, header, hlen) < 0) return;

    char *json = malloc(JSON_BUF_SIZE);
    if (!json) return;

    while (server_running) {
        usleep(1000000);  /* 1 Hz updates */
        if (!server_running) break;

        int jlen = build_json(json, JSON_BUF_SIZE - 64);

        char prefix[] = "event: update\ndata: ";
        char suffix[] = "\n\n";

        struct iovec iov[3];
        iov[0].iov_base = prefix;
        iov[0].iov_len = sizeof(prefix) - 1;
        iov[1].iov_base = json;
        iov[1].iov_len = jlen;
        iov[2].iov_base = suffix;
        iov[2].iov_len = sizeof(suffix) - 1;

        /* Use writev for atomic write */
        if (writev(fd, iov, 3) < 0) break;
    }

    free(json);
}

static void *client_thread(void *arg)
{
    int fd = (int)(intptr_t)arg;

    char buf[HTTP_BUF_SIZE];
    int n = read(fd, buf, sizeof(buf) - 1);
    if (n <= 0) { close(fd); return NULL; }
    buf[n] = '\0';

    /* Parse GET path */
    if (strncmp(buf, "GET ", 4) != 0) {
        send_response(fd, "405 Method Not Allowed", "text/plain", "405", 3);
        close(fd);
        return NULL;
    }

    char *path = buf + 4;
    char *end = strchr(path, ' ');
    if (end) *end = '\0';

    /* Strip query string — reverse proxies may append ?... parameters */
    char *qs = strchr(path, '?');
    if (qs) *qs = '\0';

    if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
        send_response(fd, "200 OK", "text/html",
                       HTML_PAGE, sizeof(HTML_PAGE) - 1);
        close(fd);
    } else if (strcmp(path, "/api/events") == 0) {
        if (atomic_fetch_add(&sse_client_count, 1) >= MAX_SSE_CLIENTS) {
            atomic_fetch_sub(&sse_client_count, 1);
            send_response(fd, "503 Service Unavailable", "text/plain",
                          "too many clients", 16);
        } else {
            handle_sse(fd);
            atomic_fetch_sub(&sse_client_count, 1);
        }
        close(fd);
    } else if (strcmp(path, "/api/state") == 0) {
        char *json = malloc(JSON_BUF_SIZE);
        if (json) {
            int jlen = build_json(json, JSON_BUF_SIZE);
            send_response(fd, "200 OK", "application/json", json, jlen);
            free(json);
        }
        close(fd);
    } else if (strcmp(path, "/api/doppler") == 0) {
        char *json = malloc(DOPPLER_BUF_SIZE);
        if (json) {
            int jlen = build_doppler_json(json, DOPPLER_BUF_SIZE);
            send_response(fd, "200 OK", "application/json", json, jlen);
            free(json);
        }
        close(fd);
    } else {
        send_response(fd, "404 Not Found", "text/plain", "404", 3);
        close(fd);
    }

    return NULL;
}

/* ---- Server thread ---- */

static void *server_thread_fn(void *arg)
{
    (void)arg;

    while (server_running) {
        struct sockaddr_in addr;
        socklen_t addrlen = sizeof(addr);
        int client = accept(server_fd, (struct sockaddr *)&addr, &addrlen);
        if (client < 0) {
            if (server_running) usleep(10000);
            continue;
        }

        /* Set SO_KEEPALIVE for SSE connections */
        int keepalive = 1;
        setsockopt(client, SOL_SOCKET, SO_KEEPALIVE,
                    &keepalive, sizeof(keepalive));

        /* Spawn client handler thread (detached) */
        pthread_t tid;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&tid, &attr, client_thread,
                            (void *)(intptr_t)client) != 0) {
            close(client);
        }
        pthread_attr_destroy(&attr);
    }

    return NULL;
}

/* ---- Public interface ---- */

int web_map_init(int port)
{
    memset(&state, 0, sizeof(state));
    pthread_mutex_init(&state.lock, NULL);

    /* Ignore SIGPIPE (broken SSE connections) */
    signal(SIGPIPE, SIG_IGN);

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("web_map: socket");
        return -1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(port),
    };

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("web_map: bind");
        close(server_fd);
        server_fd = -1;
        return -1;
    }

    if (listen(server_fd, 16) < 0) {
        perror("web_map: listen");
        close(server_fd);
        server_fd = -1;
        return -1;
    }

    server_running = 1;
    if (pthread_create(&server_thread, NULL, server_thread_fn, NULL) != 0) {
        perror("web_map: pthread_create");
        close(server_fd);
        server_fd = -1;
        return -1;
    }

    fprintf(stderr, "Web map: http://localhost:%d/\n", port);
    return 0;
}

void web_map_shutdown(void)
{
    if (!server_running) return;
    server_running = 0;

    if (server_fd >= 0) {
        shutdown(server_fd, SHUT_RDWR);
        close(server_fd);
        server_fd = -1;
    }

    pthread_join(server_thread, NULL);
    pthread_mutex_destroy(&state.lock);
}
