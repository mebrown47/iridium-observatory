/*
 * SigMF metadata reader for auto-configuring from .sigmf-meta files
 *
 * Copyright (c) 2026 CEMAXECUTER LLC
 * Modifications Copyright (c) 2026 Mike Brown
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef __SIGMF_H__
#define __SIGMF_H__

#include <stdint.h>

/*
 * Parse a .sigmf-meta JSON file and extract sample rate, center frequency,
 * and data format. Returns 0 on success, -1 on failure.
 *
 * Output parameters are only written when the corresponding field is present
 * in the metadata. Callers should initialize them to sentinel values before
 * calling to detect which fields were populated.
 */
int sigmf_read_meta(const char *meta_path,
                    double *sample_rate,
                    double *center_freq,
                    int *iq_format);

/*
 * Parse an ISO 8601 UTC time ("2026-09-27T10:22:59.128579Z", fraction
 * optional) into Unix nanoseconds. Returns 0 on success, -1 if malformed.
 */
int sigmf_parse_datetime(const char *s, uint64_t *unix_ns);

/*
 * Read captures[0].core:datetime from a .sigmf-meta file into Unix
 * nanoseconds. Returns 0 on success, -1 if the file or the field is missing
 * or malformed.
 */
int sigmf_read_datetime(const char *meta_path, uint64_t *unix_ns);

#endif
