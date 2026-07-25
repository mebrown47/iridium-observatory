/*
 * JSONL archive of classified frames and decoded messages
 *
 * Copyright (c) 2026 CEMAXECUTER LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Append-only observability archive. Enable with --archive[=DIR]
 * (default directory: ./archive). Writes one JSON object per line to
 * iridium-YYYYMMDD.jsonl, rotating daily at UTC midnight.
 *
 * Event types:
 *   {"e":"start","t":<ms>}                          process started
 *   {"e":"fmix","t":<ms>,"dl":{...},"ul":{...},     per-minute frame-type
 *    "uw":[<dl_fail>,<ul_fail>],"sats":{...}}       counts (rollup)
 *   {"e":"acars","t":<ms>,"dir":"DL","freq":<hz>,   decoded ACARS message,
 *    "reg":"..","flt":"..","lbl":"..","txt":"..",   "dec" holds the full
 *    "dec":{...}}                                   libacars JSON tree
 *   {"e":"pager","t":<ms>,"ric":<n>,"fmt":"ASCII",  decoded pager message
 *    "ok":0|1,"txt":".."}
 *
 * Frame traffic is rolled up per minute rather than logged per frame:
 * at typical rates (~100 frames/s) per-frame lines would exceed 1 GB/day
 * while the rollup carries the same information for trend analysis.
 */

#ifndef __ARCHIVE_H__
#define __ARCHIVE_H__

#include <stdint.h>

/* Open the archive directory (created if missing) and today's file.
 * Returns 0 on success, -1 on failure. Thread-safe after return. */
int archive_init(const char *dir);

/* Flush pending rollup counters and close the archive file. */
void archive_shutdown(void);

/* Count one classified frame into the current minute's rollup.
 * type: label from classify_frame_label(). uplink: 1 = UL burst.
 * sat_id: satellite id if known (IRA/IBC), else -1. */
void archive_count_frame(const char *type, int uplink, int sat_id);

/* Count a demodulated burst that matched no unique word. */
void archive_count_uw_fail(int uplink);

/* Log a decoded ACARS message. decoded_json: libacars JSON tree string
 * (embedded verbatim as the "dec" field), or NULL to omit. */
void archive_log_acars(uint64_t timestamp_ns, int uplink, double freq_hz,
                       const char *reg, const char *flight,
                       const char *label, const char *text,
                       const char *decoded_json);

/* Log a decoded pager (MSG/IMS) message. fmt: 5 = ASCII, 3 = BCD. */
void archive_log_pager(uint64_t timestamp_ns, int ric, int fmt,
                       int csum_ok, const char *text);

#endif
