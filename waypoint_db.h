/*
 * Aviation waypoint/fix database lookup
 *
 * Copyright (c) 2026 CEMAXECUTER LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef __WAYPOINT_DB_H__
#define __WAYPOINT_DB_H__

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Load waypoint database from CSV (IDENT,LAT,LON format).
 * Returns number of entries loaded, or -1 on error.
 */
int waypoint_db_load(const char *path);

/*
 * Look up a waypoint by 5-letter IDENT.
 * Returns 1 if found (lat/lon populated), 0 if not found.
 */
int waypoint_db_lookup(const char *ident, double *lat, double *lon);

/*
 * Free database resources.
 */
void waypoint_db_destroy(void);

#ifdef __cplusplus
}
#endif

#endif
