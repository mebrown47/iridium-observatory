#!/usr/bin/env python3
# Copyright (c) 2026 Mike Brown
# SPDX-License-Identifier: GPL-3.0-or-later
"""Synthetic IDA lines (iridium-parser.py's format) for testing the native
reassembler (tk_reassembler.c) against iridium-toolkit's reassembler.py on
paths recordings rarely hold: multi-packet SBD, uplink ACARS with sequence /
flight numbers, NAK, ETB continuation, broken parity / CRC / ETX,
duplicates, orphan fragments, expiry, interleaved transfers.

    tests/reasm_fuzz.py [SEED] > lines.txt
    build/reasm_check sbd < lines.txt          vs
    reassembler.py -m sbd < lines.txt          (iridium-toolkit)
"""
import random, sys

random.seed(int(sys.argv[1]) if len(sys.argv) > 1 else 3)
T0 = 1790000000


def kermit(b):
    crc = 0
    for x in b:
        crc ^= x
        for _ in range(8):
            crc = (crc >> 1) ^ 0x8408 if crc & 1 else crc >> 1
    return crc


def odd(c):                      # set bit 7 so the byte has odd parity
    c &= 0x7f
    return c | 0x80 if bin(c).count("1") % 2 == 0 else c


def acars(ul):
    reg = random.choice([".N123AB", "N939AJ.", "..G-ABCD", "N20T...", "".join(random.choice("ABCDEFGHJKLMNPQRSTUVWXYZ0123456789") for _ in range(7))])[:7].ljust(7, ".")
    label = random.choice(["_\x7f", "H1", "52", "C1", "Q0", "5Z", "B6", "SA"])
    ack = random.choice(["\x15", "0", "4", "A"])
    body = random.choice("2A") + reg + ack + label + random.choice("0123ABCDEFGHXYZ")
    # uplink always has STX: without it reassembler.py fails (AttributeError on seqn)
    if ul or random.random() < 0.8:
        if ul:
            body += "\x02" + "M%02dA" % random.randint(0, 99) + random.choice(["AB1234", "XY0042", "..9999"])
        else:
            body += "\x02"
        body += "".join(random.choice("ABCDEFGHIJKLMNOPQRSTUVWXYZ 0123456789/.-:,\r\n") for _ in range(random.randint(0, 120)))
    body += random.choice(["\x03", "\x03", "\x03", "\x17"])
    b = bytes(odd(ord(c)) for c in body)
    if random.random() < 0.05:                                   # break parity
        k = random.randrange(len(b)); b = b[:k] + bytes([b[k] ^ 0x01]) + b[k + 1:]
    crc = kermit(b)
    csum = bytes([crc & 0xff, crc >> 8])
    if random.random() < 0.05:                                   # break the CRC
        csum = bytes([csum[0] ^ 0x55, csum[1]])
    hdr = bytes([0x03]) + bytes(random.randrange(256) for _ in range(7)) if random.random() < 0.5 else b""
    tail = csum + b"\x7f" if random.random() > 0.05 else b""      # sometimes no checksum
    return b"\x01" + hdr + b + tail


def sbd_packets():
    """list of (ul, l2-payload) for one logical message"""
    ul = random.random() < 0.3
    kind = random.random()
    content = acars(ul) if random.random() < 0.6 else bytes(random.randrange(256) for _ in range(random.randint(0, 60)))
    if kind < 0.12:                                              # HELLO
        pre = bytes([0x20] + [random.randrange(256) for _ in range(14)] + [random.choice([0, 1])] + [random.randrange(256) for _ in range(13)])
        return [(ul, b"\x06\x00" + pre + content[:20])]
    if kind < 0.18:                                              # not SBD
        return [(ul, bytes(random.randrange(256) for _ in range(random.randint(1, 50))))]
    if kind < 0.45 and not ul:                                   # multi-packet DL
        n = random.randint(2, 4)
        parts = [content[i::n] for i in range(n)]
        out = []
        for i, p in enumerate(parts):
            if i == 0:
                pre = bytes([0x26, 0, 0, n, 0, random.randrange(256), random.randrange(256)])
                out.append((ul, b"\x76\x08" + pre + bytes([0x10, len(p), 1]) + p))
            else:
                out.append((ul, bytes([0x76, random.choice([0x09, 0x0a])]) + bytes([0x10, len(p), i + 1]) + p))
        return out
    if ul:
        pre = bytes([random.choice([0x50, 0x51]), random.randrange(256), random.randrange(256)]) if random.random() < 0.5 else b""
        # msgno 0: the toolkit has no single-message case for msgno 1 without a count (it raises)
        return [(ul, bytes([0x76, random.choice([0x0c, 0x0d, 0x0e])]) + pre + bytes([0x10, len(content), 0]) + content)]
    pre = bytes([0x26, 0, 0, 1, 0, random.randrange(256), random.randrange(256)]) if random.random() < 0.8 else bytes([0x20, 0, 0, 1, 0])
    msg = bytes([0x10, len(content), 1]) + content if random.random() < 0.9 else b""
    return [(ul, b"\x76\x08" + pre + msg)]


def ida_line(t, freq, ul, cont, ctr, data):
    ms = (t - T0) * 1000
    hexs = ".".join("%02x" % x for x in data)
    lcw = "%-110s " % "LCW(2,T:maint,C:<silent>,000000000000000000000)"
    body = " %s cont=%d %d ctr=%s %s len=%02d 0:0000 [" % ("000", cont, 0, format(ctr, "03b"), "000", len(data))
    body += "%-60s" % (hexs + "]")
    body += " %04x/0000 CRC:OK 0000 SBD: x" % random.randrange(65536)
    return "IDA: p-%d-e000 %014.4f %010d %3d%% %06.2f|%07.2f|%05.2f %03d %s %s%s" % (
        T0, ms, freq, random.randint(80, 100), -random.uniform(20, 60), -random.uniform(90, 110), random.uniform(10, 30),
        179, "UL" if ul else "DL", lcw, body)


events = []                      # (time, line)
t = T0 + 10.0
for m in range(6000):
    t += random.uniform(0.05, 3.0)
    freq = random.randint(1616100000, 1626000000)
    tt = t
    for ul, l2 in sbd_packets():
        frags = [l2[i:i + 20] for i in range(0, len(l2), 20)] or [b""]
        for k, fr in enumerate(frags):
            cont = 1 if k < len(frags) - 1 else 0
            ctr = k % 8
            f = freq + random.randint(-100, 100)
            if random.random() < 0.02:                       # lost fragment -> orphans later
                tt += random.uniform(0.09, 0.27)
                continue
            events.append((tt, ida_line(tt, f, ul, cont, ctr, fr)))
            if random.random() < 0.05:                       # duplicate
                events.append((tt + random.uniform(0, 0.5), ida_line(tt, f + random.randint(-150, 150), ul, cont, ctr, fr)))
            tt += random.uniform(0.09, 0.27) if random.random() > 0.01 else random.uniform(0.3, 2.0)
        tt += random.uniform(0.2, 1.5)
events.sort(key=lambda e: e[0])
for _, line in events:
    print(line)
