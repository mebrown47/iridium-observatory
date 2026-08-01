/*
 * Copyright (c) 2026 Mike Brown
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Test harness: print the LCW frame-type (ft) recovered by ida_lcw_ft() for a
 * 0/1 bitstring (full frame incl. 24-bit access code) on argv[1].
 *
 * Exercises the real shipped classification path. Compiled by run_tests.sh
 * with ../ida_decode.c and ../frame_decode.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "qpsk_demod.h"
#include "ida_decode.h"
#include "frame_decode.h"

int use_chase = 0;

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <bits>\n", argv[0]); return 2; }
    const char *s = argv[1];
    int n = (int)strlen(s);
    static uint8_t bits[8192];
    if (n > 8192) n = 8192;
    for (int i = 0; i < n; i++) bits[i] = (s[i] == '1');

    frame_decode_init();
    ida_decode_init();

    demod_frame_t f;
    memset(&f, 0, sizeof(f));
    f.bits = bits;
    f.n_bits = n;
    f.direction = DIR_DOWNLINK;

    printf("ft=%d\n", ida_lcw_ft(&f));
    return 0;
}
