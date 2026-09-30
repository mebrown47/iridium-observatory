/*
 * Native port of iridium-toolkit's reassembler.py modes ida, sbd and acars
 * (docs/NATIVE_PARSER_PLAN.md, phase 2). Input: the lines iridium-parser.py
 * prints (--parsed=full); output: what `reassembler.py -m MODE` prints.
 *
 * Ported from iridium-toolkit (https://github.com/muccc/iridium-toolkit),
 * (c) Sec & schneider, 2-Clause BSD License - see LICENSES/BSD-2-Clause-iridium-toolkit.txt
 *
 * Copyright (c) 2026 Mike Brown
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef __TK_REASSEMBLER_H__
#define __TK_REASSEMBLER_H__

#include <stdio.h>

typedef enum { TKR_OFF = 0, TKR_IDA, TKR_SBD, TKR_ACARS } tkr_mode_t;

/* "ida", "sbd" or "acars" -> mode; TKR_OFF if unknown */
tkr_mode_t tkr_mode_from_name(const char *name);

/* Start a reassembler writing to out. Not thread-safe: one caller. */
void tkr_init(tkr_mode_t mode, FILE *out);

/* One parser line (with or without the trailing newline). */
void tkr_line(const char *line);

/* End of input: flush and print the summary lines (Reassemble.end etc.). */
void tkr_end(void);

#endif
