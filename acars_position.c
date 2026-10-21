/*
 * Aircraft position from ACARS message text
 *
 * Coordinates written in the text, or failing that the first named fix,
 * for the labels known to report the aircraft's current position. Used
 * by the Iridium engine (sbd_acars.c).
 *
 * Copyright (c) 2026 CEMAXECUTER LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdlib.h>
#include <string.h>

#include "acars_position.h"
#include "waypoint_db.h"

/*
 * Extract aircraft position from ACARS free text body.
 *
 * Parses known position formats embedded in message text, gated by label
 * to avoid extracting destination waypoints or flight plan coordinates.
 * Only labels where embedded coordinates are known to represent the
 * aircraft's current position are accepted.
 *
 * Returns 1 if position was extracted, 0 otherwise.
 */
/* Decimal degrees, or degrees:minutes ("46:27.2"); *end as strtod's.
 * Minutes of 60 or more leave *end at s, as no number. */
static double tagged_degrees(const char *s, char **end)
{
    double d = strtod(s, end);
    if (*end == s || **end != ':')
        return d;
    char *mend;
    double m = strtod(*end + 1, &mend);
    if (mend == *end + 1 || m < 0 || m >= 60) {
        *end = (char *)s;
        return 0;
    }
    *end = mend;
    return d + m / 60.0;
}

static int trp_sep(char c)
{
    return c == ' ' || c == ',' || c == '#' || c == '/' ||
           c == '\r' || c == '\n' || c == '\t';
}

/* Signed decimal degrees with exactly 4 places, e.g. "-099.3047" */
static int deg4(const char *s, int n, double lim, double *v)
{
    int i = 0, d = 0;
    if (i < n && s[i] == '-') i++;
    while (i < n && s[i] >= '0' && s[i] <= '9') { i++; d++; }
    if (d < 1 || d > 3 || i + 5 != n || s[i] != '.')
        return 0;
    for (int k = i + 1; k < n; k++)
        if (s[k] < '0' || s[k] > '9')
            return 0;
    *v = strtod(s, NULL);
    return *v >= -lim && *v <= lim;
}

/* The last HHMMSS LAT LON record in a TRP report (Format 4 below). */
static int trp_position(const char *text, double *lat, double *lon)
{
    const char *tok[3] = {0};
    int len[3] = {0}, found = 0;
    const char *s = text;
    while (*s) {
        while (*s && trp_sep(*s)) s++;
        if (!*s) break;
        const char *b = s;
        while (*s && !trp_sep(*s)) s++;
        tok[0] = tok[1]; len[0] = len[1];
        tok[1] = tok[2]; len[1] = len[2];
        tok[2] = b; len[2] = (int)(s - b);
        double la, lo;
        int time_ok = tok[0] && len[0] == 6;
        for (int k = 0; time_ok && k < 6; k++)
            time_ok = tok[0][k] >= '0' && tok[0][k] <= '9';
        if (time_ok && deg4(tok[1], len[1], 90, &la) &&
            deg4(tok[2], len[2], 180, &lo)) {
            *lat = la;
            *lon = lo;
            found = 1;
        }
    }
    return found;
}

/* The last [NS]DDMM.m,[EW]DDDMM.m record (Format 5 below). */
static int ddmm_position(const char *text, double *lat, double *lon)
{
    int found = 0;
    for (const char *s = text; *s; s++) {
        if ((*s != 'N' && *s != 'S') || (s > text && !trp_sep(s[-1])))
            continue;
        const char *p = s + 1;
        int k = 0;
        while (k < 4 && p[k] >= '0' && p[k] <= '9') k++;
        if (k != 4 || p[4] != '.' || p[5] < '0' || p[5] > '9')
            continue;
        char *end;
        double a = strtod(p, &end);
        if (*end != ',' || (end[1] != 'E' && end[1] != 'W'))
            continue;
        const char *q = end + 2;
        k = 0;
        while (k < 5 && q[k] >= '0' && q[k] <= '9') k++;
        if (k != 5 || q[5] != '.' || q[6] < '0' || q[6] > '9')
            continue;
        double o = strtod(q, NULL);
        double am = a - 100 * (int)(a / 100), om = o - 100 * (int)(o / 100);
        double la = (int)(a / 100) + am / 60.0, lo = (int)(o / 100) + om / 60.0;
        if (am >= 60 || om >= 60 || la > 90 || lo > 180)
            continue;
        *lat = *s == 'S' ? -la : la;
        *lon = end[1] == 'W' ? -lo : lo;
        found = 1;
    }
    return found;
}

int acars_extract_text_position(const char *label, const char *text,
                                        double *lat, double *lon)
{
    if (!label || !text || !text[0])
        return 0;

    /* Labels known to carry current aircraft position in text:
     *   H1  - position/met report (POS preamble)
     *   20  - position report (POS preamble)
     *   44  - OOOI + position
     *   4J  - position and weather
     *   15  - general aviation position report
     *   SA  - in-range/fuel status (some carriers include position)
     *
     * NOT included (carry waypoints/destinations, not current position):
     *   H1/FPN - flight plan (future route)
     *   5Z     - too variable, airline-specific
     */
    int label_ok = 0;
    if (label[0] == 'H' && label[1] == '1') label_ok = 1;
    else if (label[0] == '2' && label[1] == '0') label_ok = 1;
    else if (label[0] == '4' && label[1] == '4') label_ok = 1;
    else if (label[0] == '4' && label[1] == 'J') label_ok = 1;
    else if (label[0] == '1' && label[1] == '5') label_ok = 1;
    else if (label[0] == 'S' && label[1] == 'A') label_ok = 1;

    if (!label_ok)
        return 0;

    /* Skip H1 flight plan messages (FPN preamble = future route, not
     * current position) */
    if (label[0] == 'H' && label[1] == '1') {
        if (strncmp(text, "FPN", 3) == 0)
            return 0;
    }

    /*
     * Format 1: POS[NS]ddddd[EW]dddddd  (degrees * 1000)
     *   Example: POSN43312W123174 = 43.312N, 123.174W
     *   Used in H1/POS and label 20/POS messages.
     */
    const char *p = strstr(text, "POS");
    if (p) {
        p += 3;
        char lat_dir = *p;
        if (lat_dir == 'N' || lat_dir == 'S') {
            p++;
            /* Parse 5-digit latitude (degrees * 1000) */
            int lat_digits = 0;
            const char *lp = p;
            while (*p >= '0' && *p <= '9' && lat_digits < 5) {
                lat_digits++;
                p++;
            }
            if (lat_digits == 5) {
                char lon_dir = *p;
                if (lon_dir == 'E' || lon_dir == 'W') {
                    p++;
                    int lon_digits = 0;
                    const char *lop = p;
                    while (*p >= '0' && *p <= '9' && lon_digits < 6) {
                        lon_digits++;
                        p++;
                    }
                    if (lon_digits == 6) {
                        char lat_buf[6], lon_buf[7];
                        memcpy(lat_buf, lp, 5); lat_buf[5] = '\0';
                        memcpy(lon_buf, lop, 6); lon_buf[6] = '\0';
                        double la = atof(lat_buf) / 1000.0;
                        double lo = atof(lon_buf) / 1000.0;
                        if (la >= 0 && la <= 90 && lo >= 0 && lo <= 180) {
                            *lat = (lat_dir == 'S') ? -la : la;
                            *lon = (lon_dir == 'W') ? -lo : lo;
                            return 1;
                        }
                    }
                }
            }
        }
    }

    /*
     * Format 2: [NS]dddmm[EW]dddmm  (degrees + minutes, no decimal)
     *   Example: N33521W084123 = 33d52.1'N, 084d12.3'W = 33.868N, 84.205W
     *   Common across many ACARS labels and carriers.
     *   Latitude: 2 digits degrees + 3 digits (minutes * 10)
     *   Longitude: 3 digits degrees + 3 digits (minutes * 10)
     */
    for (const char *s = text; *s; s++) {
        if ((*s == 'N' || *s == 'S') && s[1] >= '0' && s[1] <= '9') {
            char lat_dir = *s;
            s++;
            /* Need 5 digits for lat (DDmmm) then [EW] then 6 digits for lon */
            int n = 0;
            const char *lat_start = s;
            while (s[n] >= '0' && s[n] <= '9') n++;
            if (n == 5 && (s[n] == 'E' || s[n] == 'W')) {
                char lon_dir = s[n];
                const char *lon_start = s + n + 1;
                int m = 0;
                while (lon_start[m] >= '0' && lon_start[m] <= '9') m++;
                if (m == 6) {
                    /* Parse lat: DD + mmm (minutes * 10) */
                    int lat_deg = (lat_start[0] - '0') * 10 +
                                  (lat_start[1] - '0');
                    int lat_min10 = (lat_start[2] - '0') * 100 +
                                    (lat_start[3] - '0') * 10 +
                                    (lat_start[4] - '0');
                    /* Parse lon: DDD + mmm (minutes * 10) */
                    int lon_deg = (lon_start[0] - '0') * 100 +
                                  (lon_start[1] - '0') * 10 +
                                  (lon_start[2] - '0');
                    int lon_min10 = (lon_start[3] - '0') * 100 +
                                    (lon_start[4] - '0') * 10 +
                                    (lon_start[5] - '0');

                    if (lat_deg <= 90 && lat_min10 < 600 &&
                        lon_deg <= 180 && lon_min10 < 600) {
                        double la = lat_deg + lat_min10 / 600.0;
                        double lo = lon_deg + lon_min10 / 600.0;
                        *lat = (lat_dir == 'S') ? -la : la;
                        *lon = (lon_dir == 'W') ? -lo : lo;
                        return 1;
                    }
                }
            }
            /* Reset scan position if this wasn't a match */
            s = lat_start - 1;
        }
    }

    /*
     * Format 3: LAT [NS] dd.ddd/LON [EW] ddd.ddd  (decimal degrees, tagged)
     *   Example: LAT N 33.917/LON W 77.988
     *   Also LONG for LON, and degrees:minutes for either value:
     *   LAT N 46:27.2  LONG W101:39.7 (a weather/position report).
     *   Used in some structured position reports.
     */
    p = strstr(text, "LAT");
    if (p) {
        p += 3;
        while (*p == ' ') p++;
        char lat_dir = *p;
        if (lat_dir == 'N' || lat_dir == 'S') {
            p++;
            while (*p == ' ') p++;
            char *end;
            double la = tagged_degrees(p, &end);
            if (end != p && la >= 0 && la <= 90) {
                /* Find LON (or LONG) */
                const char *lp = strstr(end, "LON");
                if (lp) {
                    lp += 3;
                    if (*lp == 'G') lp++;
                    while (*lp == ' ') lp++;
                    char lon_dir = *lp;
                    if (lon_dir == 'E' || lon_dir == 'W') {
                        lp++;
                        while (*lp == ' ') lp++;
                        double lo = tagged_degrees(lp, &end);
                        if (end != lp && lo >= 0 && lo <= 180) {
                            *lat = (lat_dir == 'S') ? -la : la;
                            *lon = (lon_dir == 'W') ? -lo : lo;
                            return 1;
                        }
                    }
                }
            }
        }
    }

    /*
     * Format 4: TRP reports, records of HHMMSS LAT LON in signed decimal
     * degrees to 4 places, oldest first, separated by spaces, commas or '#':
     *   TRP 110814  32.8998  -97.0403 289 142.5 ...
     *   /A1 215402, 45.7750,-102.8185,330,...  (records /A1 to /A5)
     *   TRP#190829#045.7379#-099.3047#350#...
     * The last record is the newest.
     */
    if (strstr(text, "TRP") && trp_position(text, lat, lon))
        return 1;

    /*
     * Format 5: [NS]DDMM.m,[EW]DDDMM.m records, oldest first, then the
     * time and the weather:
     *   N3006.4,W08531.3,210145,40004,-51.3,296,029,ER,00000,0,
     * The last record is the newest.
     */
    if (ddmm_position(text, lat, lon))
        return 1;

    return 0;
}

/*
 * Extract aircraft position from waypoint name in ACARS text.
 * Only uses the FIRST waypoint reference (current position fix),
 * not subsequent ones (which are "next" or destination waypoints).
 *
 * Scans for 5-letter uppercase sequences that match the waypoint database.
 * Returns 1 if a valid waypoint was found and resolved, 0 otherwise.
 */
int acars_extract_waypoint_position(const char *label, const char *text,
                                            double *lat, double *lon)
{
    if (!label || !text || !text[0])
        return 0;

    /* Same label gate as coordinate extraction */
    int label_ok = 0;
    if (label[0] == 'H' && label[1] == '1') label_ok = 1;
    else if (label[0] == '2' && label[1] == '0') label_ok = 1;
    else if (label[0] == '4' && label[1] == '4') label_ok = 1;
    else if (label[0] == '4' && label[1] == 'J') label_ok = 1;
    else if (label[0] == '1' && label[1] == '5') label_ok = 1;
    else if (label[0] == 'S' && label[1] == 'A') label_ok = 1;
    if (!label_ok) return 0;

    /* Skip FPN (flight plan) preamble */
    if (label[0] == 'H' && label[1] == '1' && strncmp(text, "FPN", 3) == 0)
        return 0;

    /* In POS messages, the waypoint after the coordinate block is the
     * current fix. Format: POS[coords],WAYPOINT,time,...
     * If we get here, coordinate extraction already failed, so look for
     * the first comma-separated 5-letter token after "POS". */
    const char *scan = text;

    /* If text starts with POS, skip past it to find waypoint fields */
    if (strncmp(scan, "POS", 3) == 0)
        scan += 3;

    /* Scan for 5-letter uppercase tokens (comma or space delimited) */
    while (*scan) {
        /* Skip to next potential start of a token */
        while (*scan && *scan != ',' && *scan != ' ' && *scan != '/')
            scan++;
        while (*scan == ',' || *scan == ' ' || *scan == '/')
            scan++;

        /* Check if we have a 5-letter uppercase sequence */
        if (scan[0] >= 'A' && scan[0] <= 'Z' &&
            scan[1] >= 'A' && scan[1] <= 'Z' &&
            scan[2] >= 'A' && scan[2] <= 'Z' &&
            scan[3] >= 'A' && scan[3] <= 'Z' &&
            scan[4] >= 'A' && scan[4] <= 'Z' &&
            (scan[5] == ',' || scan[5] == ' ' || scan[5] == '/' ||
             scan[5] == '\0' || scan[5] == '\r' || scan[5] == '\n')) {

            char ident[6];
            memcpy(ident, scan, 5);
            ident[5] = '\0';

            /* Look up in waypoint database. A position off the globe is a
             * broken entry (older lists carried packed DDMMSS rows read as
             * latitudes like 242917), so keep scanning past it. */
            double la, lo;
            if (waypoint_db_lookup(ident, &la, &lo) &&
                la >= -90 && la <= 90 && lo >= -180 && lo <= 180) {
                *lat = la;
                *lon = lo;
                return 1;
            }
        }

        /* Move past this token */
        while (*scan && *scan != ',' && *scan != ' ' && *scan != '/')
            scan++;
    }

    return 0;
}

int acars_aircraft_position(const char *label, const char *text,
                            int from_aircraft, double *lat, double *lon)
{
    if (!from_aircraft || !text || !text[0])
        return 0;
    if (acars_extract_text_position(label, text, lat, lon))
        return ACARS_POS_TEXT;
    const char *t = text;
    if (t[0] == '#' && strlen(t) > 4)
        t += 4;
    if (strncmp(t, "POS", 3) == 0 &&
        acars_extract_waypoint_position(label, t, lat, lon))
        return ACARS_POS_FIX;
    return 0;
}
