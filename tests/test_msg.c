/*
 * Copyright (c) 2026 Mike Brown
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Test harness: decode a 0/1 bitstring (full frame incl. 24-bit access code)
 * given on argv[1] via frame_decode(), and print the decoded result.
 *
 * Build is driven by tests/run_tests.sh (compiled with ../frame_decode.c).
 * Not part of the normal application build.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "frame_decode.h"

int use_chase = 0;   /* referenced by frame_decode.c via extern */

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <bits> [freq_hz]\n", argv[0]); return 2; }
    const char *s = argv[1];
    int n = (int)strlen(s);
    static uint8_t bits[8192];
    static float llr[8192];
    if (n > 8192) n = 8192;
    for (int i = 0; i < n; i++) { bits[i] = (s[i] == '1'); llr[i] = 1.0f; }

    frame_decode_init();

    demod_frame_t f;
    memset(&f, 0, sizeof(f));
    f.bits = bits;
    f.llr = llr;
    f.n_bits = n;
    f.center_frequency = (argc >= 3) ? atof(argv[2]) : 1626200000.0;

    decoded_frame_t out;
    if (!frame_decode(&f, &out)) { printf("NODECODE\n"); return 1; }

    if (out.type == FRAME_MSG)
        printf("MSG ric=%d fmt=%d seq=%d ok=%d ctr=%d/%d TXT=[%s]\n",
               out.msg.ric, out.msg.format, out.msg.seq, out.msg.csum_ok,
               out.msg.ctr, out.msg.ctr_max, out.msg.text);
    else if (out.type == FRAME_IRA)
        printf("IRA sat=%d beam=%d\n", out.ira.sat_id, out.ira.beam_id);
    else if (out.type == FRAME_IBC)
        printf("IBC sat=%d beam=%d\n", out.ibc.sat_id, out.ibc.beam_id);
    else
        printf("OTHER type=%d\n", out.type);
    return 0;
}
