/*
 * Reed-Solomon, CRC-24 and checksum as iridium-toolkit's parser uses them -
 * see tk_rs.c.
 *
 * Copyright (c) 2026 Mike Brown
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef __TK_RS_H__
#define __TK_RS_H__

/* rs.rs_fix (RS(47,31) over GF(256), 8 erased ecc bytes appended): for n
 * data bytes, returns the message length (n - 8) with msg and the 8 csum
 * bytes filled, or a negative value when not correctable (-1) or when the
 * toolkit would raise something other than ReedSolomonError (-2). */
int tk_rs8_fix(const int *data, int n, int *msg, int *csum);

/* rs6.rs_fix (RS over GF(64), fcr 54, 10 ecc symbols): as tk_rs8_fix,
 * message length n - 10. */
int tk_rs6_fix(const int *data, int n, int *msg, int *csum);

/* bitsparser.iip_crc24 over n bytes */
unsigned tk_iip_crc24(const unsigned char *d, int n);

/* bitsparser.checksum_16 of a 31-byte message */
unsigned tk_checksum_16(const int *msg);

#endif
