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
    check("TRP, one record", "H1",
          "TRP 110814  32.8998  -97.0403 289 142.5 459   2065 0.01952\r\n"
          "  20.00   33.75 1.028  0.992  0.002 -0.005 -29  0 00 00 291 8    1969\r\n",
          't', 32.8998, -97.0403);
    check("TRP records /A1 to /A3, newest last", "H1",
          "TRP KMIA KDEN  7 4\r\n"
          "/A1 013046  29.8682  -83.7857 320 166.2 478 0.000000\r\n"
          "0.000000  34.00  -63.28  26.0 -38  8.11 0\r\n"
          "/A2 013346  30.0744  -84.1349 320 165.9 478 0.000000\r\n"
          "0.000000  38.00  -54.14  25.8 -38  8.11 0\r\n"
          "/A3 013646  30.2773  -84.4830 320 165.6 471 0.000000\r\n",
          't', 30.2773, -84.4830);
    check("TRP records with commas", "H1",
          "A321,172463,1,1,TB000000/REP291,00,00,4/\r\nTRP KSEA KCLT  8 8\r\n"
          "/A1 215402, 45.7750,-102.8185,330,178.2,459,0.000200,\r\n"
          "0.000120, 23.62,-175.63, 24.2,-44, 7.75,0\r\n"
          "/A2 215702, 45.6875,-102.2838,330,177.8,463,0.000191,\r\n",
          't', 45.6875, -102.2838);
    check("TRP with # separators", "H1",
          "\r\nTRP#190829#045.7379#-099.3047#350#160.3#\r\n454#0002329#0.02207#",
          't', 45.7379, -99.3047);
    check("TRP without a record", "H1",
          "A38/A31938,1,1/C1TRP,110740,KMEM,KMIA,08,8,61038/C2324491,-0866412,370",
          '-', 0, 0);
    check("degrees and decimal minutes, newest last", "H1",
          "0142,40004,-51.5,293,031,ER,00000,0,\r\n"
          "N3006.4,W08531.3,210145,40004,-51.3,296,029,ER,00000,0,\r\n"
          "N3011.2,W08555.9,210148,40003,-51.0,297,030,ER,00000,0,\r\n"
          "N3015.9,W08620.5,210151,40005,-51.0,305,033,ER,00000,0",
          't', 30.265, -86.341667);
    check("label 32, hemisphere letters", "32",
          "P,R,2026-09-26 19:00:28,7984,63200,N45.773 W101.368,25654,234,22,-25,410,430,701,,",
          't', 45.773, -101.368);
    check("label 83, PR packed degrees and minutes", "83",
          "001PR26190048N4500.6W10043.34304280140", 't', 45.01, -100.721667);
    check("label 83, origin destination time", "83",
          "EHAM,KLAX,261903, 46.79,-101.63,37999,272,-142.4, 18200", 't', 46.79, -101.63);
    check("label 83 without a position", "83",
          "EHAM,KLAX,261903,FL380", '-', 0, 0);
    check("other labels ignored", "5Z",
          "POSN46247W101478,HASTE", '-', 0, 0);

    /* the rules both engines apply: aircraft-sent only, fix only in POS */
    double lat, lon;
    int r;
    r = acars_aircraft_position("H1", "POSN46247W101478,KSEA", 1, &lat, &lon);
    printf("%s aircraft-sent coordinates: got %d\n", r == ACARS_POS_TEXT ? "PASS" : "FAIL", r);
    fails += r != ACARS_POS_TEXT;
    r = acars_aircraft_position("H1", "POSN46247W101478,KSEA", 0, &lat, &lon);
    printf("%s uplink ignored: got %d\n", r == 0 ? "PASS" : "FAIL", r);
    fails += r != 0;
    r = acars_aircraft_position("H1", "#M1BPOS,HASTE,190504", 1, &lat, &lon);
    printf("%s POS report naming a fix: got %d\n", r == ACARS_POS_FIX ? "PASS" : "FAIL", r);
    fails += r != ACARS_POS_FIX;
    r = acars_aircraft_position("H1", "PWI/WD430,HASTE,149009,430M62", 1, &lat, &lon);
    printf("%s fix outside a POS report ignored: got %d\n", r == 0 ? "PASS" : "FAIL", r);
    fails += r != 0;
    return fails ? 1 : 0;
}
