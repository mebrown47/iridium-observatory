/*
 * Aircraft position from ACARS message text
 *
 * Copyright (c) 2026 CEMAXECUTER LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef __ACARS_POSITION_H__
#define __ACARS_POSITION_H__

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Coordinates written in the text of a position-reporting label (H1, 20,
 * 44, 4J, 15, SA; not H1 flight plans): POS[NS]ddddd[EW]dddddd,
 * [NS]DDmmm[EW]DDDmmm, or LAT [NS] dd.ddd/LON [EW] ddd.ddd.
 * Returns 1 and fills lat/lon when found, else 0.
 */
int acars_extract_text_position(const char *label, const char *text,
                                double *lat, double *lon);

/*
 * The first 5-letter fix named in the text of the same labels, looked up
 * in the waypoint database (waypoint_db_load first). Meant as the fallback
 * after acars_extract_text_position. Returns 1 when found, else 0.
 */
int acars_extract_waypoint_position(const char *label, const char *text,
                                    double *lat, double *lon);

#ifdef __cplusplus
}
#endif

#endif
