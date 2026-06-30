/*
 * Built-in web map server for Iridium ring alerts and satellites
 *
 * Copyright (c) 2026 CEMAXECUTER LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Built-in web map for Iridium beam tracking, MT positions, and satellites
 *
 * Runs a minimal HTTP server with SSE for real-time map updates.
 * Enable with --web[=PORT] (default port 8888).
 */

#ifndef __WEB_MAP_H__
#define __WEB_MAP_H__

#include <stdint.h>
#include "frame_decode.h"
#include "ida_decode.h"

/* Initialize and start the web map HTTP server on the given port.
 * Spawns a background thread. Returns 0 on success. */
int web_map_init(int port);

/* Shut down the web map server and free resources. */
void web_map_shutdown(void);

/* Add a decoded IRA (ring alert) to the map state. Thread-safe.
 * Routes to beam storage (alt < 100) or satellite storage (700-900). */
void web_map_add_ra(const ira_data_t *ra, uint64_t timestamp,
                     double frequency);

/* Add/update a satellite from a decoded IBC frame. Thread-safe. */
void web_map_add_sat(const ibc_data_t *ibc, uint64_t timestamp);

/* Set estimated receiver position from Doppler positioning. Thread-safe. */
void web_map_set_position(double lat, double lon, double hdop);

/* Add an MT (mobile terminal) position. Thread-safe. */
void web_map_add_mt(double lat, double lon, int alt, uint16_t msg_type,
                     uint64_t timestamp, double frequency);

/* IDA message callback for MT position extraction.
 * Pass to ida_reassemble() when --web is active. */
void mtpos_ida_cb(const uint8_t *data, int len, uint64_t timestamp,
                   double frequency, ir_direction_t direction,
                   float magnitude, void *user);

/* Add a beam-based or ADS-C aircraft position fix from an ACARS message.
 * reg: aircraft registration (tail number, no leading dots).
 * flight: flight number or empty string if unknown.
 * lat/lon: position (ADS-C GPS if adsc_pos=1, else beam center estimate).
 * alt_ft: altitude in feet (-99999 if unknown).
 * adsc_pos: 1 = GPS-quality ADS-C position, 0 = beam estimate (~200 km).
 * oooi: OOOI event string ("OUT","OFF","ON","IN") or NULL if not an event.
 * Thread-safe. */
void web_map_add_aircraft(const char *reg, const char *flight,
                           double lat, double lon,
                           int sat_id, int beam_id,
                           uint64_t timestamp_ns, double frequency,
                           int alt_ft, int adsc_pos,
                           const char *oooi);

/* Append a decoded pager (MSG/IMS) message to the messaging feed. Thread-safe. */
void web_map_add_msg(const msg_data_t *msg, uint64_t timestamp_ns);
void web_map_update_doppler(uint64_t now_ns);

/* Increment the frame-type histogram for the given type label
 * (e.g. "IRA","IBC","MSG","IDA","IDA_UL_FAIL","ISY","IIP","VOC","ITL",
 * "IU3","IU6","RAW"). Unknown labels are ignored. Thread-safe. */
void web_map_count_type(const char *label);

/* Increment the unique-word-failure counter for the given direction.
 * These bursts demodulated but matched no UW, so they never reach
 * classify_frame_label(). Thread-safe. */
void web_map_count_uw_fail(ir_direction_t direction);
void web_map_count_uw_ambiguous(void);

/* Append an ACARS message to the recent messages feed (chronological tab).
 * Heartbeats (label "_d") and empty-text messages should not be passed in --
 * filter at the call site. Thread-safe. */
void web_map_add_acars_message(const char *reg, const char *flight,
                                const char *label, const char *text,
                                int ul, uint64_t timestamp_ns);

#endif
