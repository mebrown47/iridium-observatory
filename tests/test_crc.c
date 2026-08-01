/*
 * Copyright (c) 2026 Mike Brown
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Standalone regression check for the IIP CRC-24.
 *
 * The constants below MUST match ida_decode.c's iip_crc24(). The expected
 * outputs were verified bit-exact against iridium-toolkit's crcmod definition
 * (crcmod.mkCrcFun(poly=0x1BBA1B5, initCrc=0xffffff^0x0c91b6, rev=True,
 *  xorOut=0x0c91b6)). Exits non-zero on any mismatch.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define IIP_REFPOLY 0xad85ddu
#define IIP_INIT    0xffffffu
#define IIP_XOROUT  0x0c91b6u

static uint32_t iip_crc24(const uint8_t *b, int n)
{
    uint32_t crc = IIP_INIT;
    for (int i = 0; i < n; i++) {
        crc ^= b[i];
        for (int k = 0; k < 8; k++)
            crc = (crc & 1) ? (crc >> 1) ^ IIP_REFPOLY : crc >> 1;
    }
    return (crc ^ IIP_XOROUT) & 0xFFFFFFu;
}

int main(void)
{
    uint8_t z39[39] = {0}, seq39[39];
    uint8_t one0 = 0, one1 = 1;
    uint8_t irid[7] = {0x49, 0x52, 0x49, 0x44, 0x49, 0x55, 0x4d};
    for (int i = 0; i < 39; i++) seq39[i] = (uint8_t)i;

    struct { const uint8_t *d; int n; uint32_t exp; } v[] = {
        { (const uint8_t *)"", 0, 0xf36e49 },
        { &one0, 1, 0x41f364 },
        { &one1, 1, 0x975412 },
        { z39, 39, 0x38c005 },
        { seq39, 39, 0x28b591 },
        { irid, 7, 0xf7a2e9 },
    };

    int fail = 0;
    for (int i = 0; i < (int)(sizeof(v) / sizeof(v[0])); i++) {
        uint32_t got = iip_crc24(v[i].d, v[i].n);
        if (got != v[i].exp) {
            printf("CRC FAIL #%d: got %06x want %06x\n", i, got, v[i].exp);
            fail = 1;
        }
    }
    if (!fail) printf("CRC: 6/6 vectors OK\n");
    return fail;
}
