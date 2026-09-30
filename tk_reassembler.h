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

typedef enum { TKR_OFF = 0, TKR_IDA, TKR_SBD, TKR_ACARS, TKR_LIBACARS } tkr_mode_t;

/* "ida", "sbd", "acars" or (built with libacars-2) "libacars" -> mode;
 * TKR_OFF if unknown */
tkr_mode_t tkr_mode_from_name(const char *name);

/* reassembler.py -a options: json, showerrs, nopings, perfect (accepted,
 * no effect in these modes). Returns 0, or -1 for an unknown one. */
int tkr_set_arg(const char *arg);

/* ---- Instances (several can run side by side) ---- */

typedef struct tkr tkr_t;

/* A reassembler in `mode` printing to out. */
tkr_t *tkr_new(tkr_mode_t mode, FILE *out);
/* reassembler.py -a option (as tkr_set_arg). */
int tkr_arg(tkr_t *t, const char *arg);
/* Tagged output instead of out: every message's line goes to emit as
 * "RSM: <name> <n> <t1>,...,<tn> | <reassembler.py line>" (RSM: no
 * iridium-parser.py line starts so - MSG: is its pager type), where t1..tn are
 * the Unix times (s, 6 decimals) of the IDA frames the message was built
 * from; no summary lines at the end. */
void tkr_set_emit(tkr_t *t, const char *name, void (*emit)(const char *line, void *ctx), void *ctx);
/* One parser line. */
void tkr_feed(tkr_t *t, const char *line);
/* End of input: the summary lines (untagged instances), then free t. */
void tkr_finish(tkr_t *t);

/* ---- The single instance of --reassemble (and tests/reasm_check) ---- */

/* Start a reassembler writing to out. Not thread-safe: one caller. */
void tkr_init(tkr_mode_t mode, FILE *out);

/* One parser line (with or without the trailing newline). */
void tkr_line(const char *line);

/* End of input: flush and print the summary lines (Reassemble.end etc.). */
void tkr_end(void);

#endif
