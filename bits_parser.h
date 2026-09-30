/*
 * Native port of iridium-toolkit's iridium-parser.py / bitsparser.py:
 * turns one RAW: line into the line iridium-parser.py prints for it
 * (docs/NATIVE_PARSER_PLAN.md). Default parser options only (line output,
 * frequency classification on, no --harder / --uw-ec / filters).
 *
 * Frame formats ported from iridium-toolkit (https://github.com/muccc/iridium-toolkit),
 * (c) Sec & schneider, 2-Clause BSD License - see LICENSES/BSD-2-Clause-iridium-toolkit.txt
 *
 * Copyright (c) 2026 Mike Brown
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef __BITS_PARSER_H__
#define __BITS_PARSER_H__

#include <stddef.h>

/* Parse one RAW: line (without the trailing newline) and write what
 * iridium-parser.py prints for it - pretty(), plus " ERR:..." when the
 * frame has errors - into out (NUL-terminated, truncated to outsz).
 * Returns the full length (as snprintf). Thread-safe. */
int bp_parse_line(const char *raw_line, char *out, size_t outsz);

#endif
