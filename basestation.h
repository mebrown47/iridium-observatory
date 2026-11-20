/*
 * BaseStation (SBS) format output for aircraft position feeds
 *
 * Outputs MSG,3 position messages compatible with VRS, tar1090,
 * PlanePlotter, and other ADS-B tracking tools.
 *
 * Copyright (c) 2026 CEMAXECUTER LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef __BASESTATION_H__
#define __BASESTATION_H__

#include <stdint.h>

/*
 * Initialize basestation output.
 * endpoint: "PORT" for TCP server, "HOST:PORT" for TCP client
 * Returns 0 on success, -1 on error.
 */
int basestation_init(const char *endpoint);

/*
 * Send an aircraft position in MSG,3 format to all connected clients
 * (server mode) or to the remote host (client mode).
 *
 * reg:       aircraft registration (looked up in aircraft_db for ICAO hex)
 * flight:    flight ID / callsign (may be empty)
 * lat, lon:  position in decimal degrees
 * alt_ft:    altitude in feet (-99999 if unknown)
 * timestamp: detection time in nanoseconds
 */
void basestation_send_position(const char *reg, const char *flight,
                                double lat, double lon,
                                int alt_ft, uint64_t timestamp_ns);

/*
 * Shut down basestation output (close sockets, stop thread).
 */
/* Aircraft active on ACARS with no reported position: MSG,1 only, so it is
 * listed but not placed. */
int basestation_send_ident(const char *reg, const char *flight);

void basestation_destroy(void);

#endif
