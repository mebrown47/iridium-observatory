#!/usr/bin/env python3
# Copyright (c) 2026 Mike Brown
# SPDX-License-Identifier: GPL-3.0-or-later
# Encode a valid Iridium MSG (messaging) frame and emit the full bitstring
# (access code + messaging header + payload) on stdout.
#
# This inverts iridium-toolkit bitsparser.py's MSG decode path, so the same
# bits can be fed to BOTH the reference parser and this repo's C decoder for a
# byte-for-byte cross-check. See tests/run_tests.sh and tests/README.md.
import sys

access_dl = "001100000011000011110011"
header_messaging = "00110011111100110011001111110011"
POLY = 1897  # messaging_bch_poly


def gf2_rem(poly, val_bits):
    v = int(val_bits, 2)
    pb = poly.bit_length()
    for i in range(len(val_bits) - 1, pb - 2, -1):
        if v & (1 << i):
            v ^= poly << (i - pb + 1)
    return v


def bch_encode_31_21(data21):
    # systematic: codeword = data<<10 | remainder, then an even-parity bit
    d = int(data21, 2)
    cw = d << 10
    rem = gf2_rem(POLY, format(cw, '031b'))
    code31 = format(cw | rem, '031b')
    return code31 + str(code31.count('1') % 2)   # 32 bits


def inv_de_interleave(odd, even):
    # rebuild the 64-bit interleaved group from the two 32-bit halves
    symbols = [None] * 32
    for j in range(16):
        symbols[31 - 2 * j] = odd[2 * j:2 * j + 2]
        symbols[30 - 2 * j] = even[2 * j:2 * j + 2]
    group = [''] * 64
    for i in range(32):
        group[2 * i] = symbols[i][1]
        group[2 * i + 1] = symbols[i][0]
    return ''.join(group)


def bits(n, width):
    return format(n, '0%db' % width)


def encode(ric, text, seq=10, block=3, frame=5):
    fmt = 5  # ASCII
    ric_rev = bits(ric, 22)[::-1]
    prefix = ric_rev + bits(fmt, 5) + bits(seq, 6) + "0000" + bits(0, 6)
    prefix += "0000" + "0" + "0" + bits(0, 7)   # pkt_cs2/len_bit/zero2/checksum
    plen = len(prefix)  # 56

    # find T multiple of 20, T/20 odd, with char region in [7*len, 7*len+6]
    t = list(text)
    while True:
        lo = plen + 7 * len(t)
        T = next((c for c in range(lo, lo + 7)
                  if c % 20 == 0 and (c // 20) % 2 == 1), None)
        if T is not None:
            break
        t.append(' ')
    text = ''.join(t)

    char_bits = ''.join(bits(ord(c), 7) for c in text)
    rest = prefix + char_bits
    rest += '0' * (T - len(rest))
    assert len(rest) == T

    nbody = T // 20
    body_blocks = ['0' + rest[i * 20:(i + 1) * 20] for i in range(nbody)]

    bch_blocks = (1 + nbody) // 2
    hdr = "0" + "0000" + bits(block, 4) + bits(frame, 6) + bits(bch_blocks, 4) + "00"
    assert len(hdr) == 21
    blocks21 = [hdr] + body_blocks
    assert len(blocks21) % 2 == 0

    bch32 = [bch_encode_31_21(b) for b in blocks21]
    payload = ''
    for i in range(0, len(bch32), 2):
        payload += inv_de_interleave(bch32[i], bch32[i + 1])

    # RAW (pre-symbol_reverse) convention: parser swaps pairs before decoding,
    # matching the C demod output. access+header are swap-invariant (00/11).
    rev = list(payload)
    for i in range(0, len(rev) - 1, 2):
        rev[i], rev[i + 1] = rev[i + 1], rev[i]
    return access_dl + header_messaging + ''.join(rev), text


if __name__ == "__main__":
    ric = int(sys.argv[1]) if len(sys.argv) > 1 else 1234567
    text = sys.argv[2] if len(sys.argv) > 2 else "HELLO IRIDIUM"
    fb, final_text = encode(ric, text)
    print(fb)
    sys.stderr.write("RIC=%d TEXT=[%s]\n" % (ric, final_text))
