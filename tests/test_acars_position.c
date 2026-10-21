/*
 * Positions from ACARS text (acars_position.c), used by the Iridium
 * engine. Prints one line per case and exits non-zero on any failure.
 *
 *   test_acars_position FIXES.csv
 *
 * FIXES.csv is written by run_tests.sh: HASTE and GOODY are good fixes,
 * BADFX is a packed-DDMMSS row of the kind older lists carried.
 *
 * Copyright (c) 2026 Mike Brown
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <stdio.h>

#include "acars_position.h"
#include "waypoint_db.h"

static int fails = 0;

/* kind: 't' text position expected, 'w' waypoint expected, '-' neither */
static void check(const char *name, const char *label, const char *text,
                  char kind, double want_lat, double want_lon)
{
    double lat = 0, lon = 0;
    char got = '-';
    if (acars_extract_text_position(label, text, &lat, &lon)) got = 't';
    else if (acars_extract_waypoint_position(label, text, &lat, &lon)) got = 'w';
    int ok = got == kind &&
             (kind == '-' || (fabs(lat - want_lat) < 0.001 && fabs(lon - want_lon) < 0.001));
    printf("%s %s: got %c %.3f %.3f\n", ok ? "PASS" : "FAIL", name, got, lat, lon);
    if (!ok) fails++;
}

int main(int argc, char **argv)
{
    if (argc < 2 || waypoint_db_load(argv[1]) < 0)
        return 2;
    check("H1 POS coordinates", "H1",
          "POSN46247W101478,KSEA,190504,350,HASTE,203912", 't', 46.247, -101.478);
    check("label 20 degrees and minutes", "20",
          "RPT N33521W084123 FL350", 't', 33.868333, -84.205);
    check("tagged LAT/LON", "H1",
          "LAT N 33.917/LON W 77.988", 't', 33.917, -77.988);
    check("tagged degrees and minutes, LONG", "H1",
          "WEATHER/POSITION REPORT\r\nLAT N 46:27.2  LONG W101:39.7  TOTAL FUEL", 't',
          46.453333, -101.661667);
    check("first named fix", "H1",
          "REPORT,HASTE,GOODY", 'w', 46.0, -101.0);
    check("broken fix skipped", "H1",
          "REPORT,BADFX,GOODY", 'w', 10.0, 10.0);
    check("flight plan ignored", "H1",
          "FPN/RI:DA:KSEA:AA:KEWR,HASTE", '-', 0, 0);
    check("other labels ignored", "5Z",
          "POSN46247W101478,HASTE", '-', 0, 0);
    return fails ? 1 : 0;
}
