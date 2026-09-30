#!/usr/bin/env python3
# Copyright (c) 2026 Mike Brown
# SPDX-License-Identifier: GPL-3.0-or-later
"""Synthetic RAW: lines for the frame types a recording rarely holds, for
tests/parser_equiv.sh (docs/NATIVE_PARSER_PLAN.md).

    tests/parser_fuzz.py TOOLKIT_DIR CORPUS_DIR OUT_DIR [SEED]

Writes OUT_DIR/NAME.raw and, by running TOOLKIT_DIR/iridium-parser.py on
it, OUT_DIR/NAME.expected.txt:

  aq     uplink BPSK frames (IAQ, CRC right and wrong; some not BPSK)
  nxt    frames with the NXT access codes
  rand   random frames: any access code, length and frequency
  msg    ASCII pager messages (tests/enc_msg.py), some with bit errors (MSG)
  bcd    BCD pager messages (format 3) (MS3)
  rare   voice/data payloads put into real VOC/IIU frames from
         CORPUS_DIR/fc10m.*: an RS codeword (VOD), a CRC-24-valid payload
         (VDA), an RS codeword with checksum_16 = 0 (IIR)

Needs the toolkit's Python (its reedsolo.py, crcmod).
"""
import inspect, os, random, struct, subprocess, sys

toolkit, corpus, out = sys.argv[1], sys.argv[2], sys.argv[3]
seed = int(sys.argv[4]) if len(sys.argv) > 4 else 7
sys.path.insert(0, toolkit)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import reedsolo, crcmod, enc_msg  # noqa: E402

iip_crc24 = crcmod.mkCrcFun(poly=0x1BBA1B5, initCrc=0xffffff ^ 0x0c91b6, rev=True, xorOut=0x0c91b6)
DL = "001100000011000011110011"; UL = "110011000011110011111100"
NDL = "110011110011111111111100"; NUL = "001111000000000011111111"
random.seed(seed)


def swap(b):   # RAW: lines are symbol-reversed relative to the parser's bits
    r = list(b)
    for i in range(0, len(r) - 1, 2):
        r[i], r[i + 1] = r[i + 1], r[i]
    return "".join(r)


def rbits(n):
    return "".join(random.choice("01") for _ in range(n))


def line(bits, freq, t, i, conf=None, snr=None):
    snr = random.uniform(5, 40) if snr is None else snr
    conf = random.randint(40, 100) if conf is None else conf
    lvl = random.choice([random.uniform(0.0001, 0.2), 0.0])
    return "RAW: i-1790500000-t1 %012.4f %010d N:%05.2f%+06.2f I:%011d %3d%% %.5f %3d %s" % (
        t, freq, snr, -random.uniform(80, 115), i, conf, lvl, max(0, len(bits) // 2 - 12), bits)


def checksum_16(msg):
    c = sum(struct.unpack("<14HBH", bytes(msg)))
    return ((c & 0xffff) + (c >> 16)) ^ 0xffff


def tail_for_zero_crc(d):
    """3 bytes x with iip_crc24(d + x) == 0: the CRC is affine in x over GF(2)."""
    def crc_x(x):
        return iip_crc24(bytes(d + [x & 0xff, (x >> 8) & 0xff, (x >> 16) & 0xff]))
    b = crc_x(0)
    cols = [crc_x(1 << i) ^ b for i in range(24)]
    rows = [[sum(((cols[i] >> r) & 1) << i for i in range(24)), (b >> r) & 1] for r in range(24)]
    used, piv = set(), []
    for c in range(24):
        p = next((k for k in range(24) if (rows[k][0] >> c) & 1 and k not in used), None)
        if p is None:
            continue
        for k in range(24):
            if k != p and (rows[k][0] >> c) & 1:
                rows[k][0] ^= rows[p][0]
                rows[k][1] ^= rows[p][1]
        used.add(p)
        piv.append((p, c))
    x = sum(1 << c for p, c in piv if rows[p][1])
    t = [x & 0xff, (x >> 8) & 0xff, (x >> 16) & 0xff]
    return t if iip_crc24(bytes(d + t)) == 0 else None


sets = {k: [] for k in ("aq", "nxt", "rand", "msg", "bcd", "rare")}
encode3_src = inspect.getsource(enc_msg.encode).replace("fmt = 5  # ASCII", "fmt = 3  # BCD").replace("def encode(", "def encode3(")
ns = dict(enc_msg.__dict__)
exec(encode3_src, ns)

t = 100.0
for i in range(5000):
    t += random.uniform(0.1, 50)
    n = random.choice(range(26, 50))
    sy = "".join(random.choice(["00", "11"]) if random.random() > 0.03 else random.choice(["01", "10"]) for _ in range(n))
    sets["aq"].append(line(swap(UL + sy + rbits(random.choice([0, 0, 1, 3]))), random.randint(1616000000, 1625900000), t, i))
    sets["nxt"].append(line(swap(random.choice([NDL, NUL]) + rbits(random.randint(50, 420))), random.randint(1616000000, 1626500000), t, i))
    acc = random.choice([DL, DL, UL, NDL, NUL, rbits(24)])
    sets["rand"].append(line(swap(acc + rbits(random.randint(0, 840))),
                             random.choice([random.randint(1616000000, 1626500000), 1626270833, 1625904393, 1625904394, 1626178601, 1626178602]), t, i))
    for name, enc, alphabet in (("msg", enc_msg.encode, "ABCDEFGHIJKLMNOPQRSTUVWXYZ 0123456789.,-"), ("bcd", ns["encode3"], "0123456789")):
        txt = "".join(random.choice(alphabet) for _ in range(random.randint(1, 60)))
        fb, _ = enc(random.randint(0, 4194303), txt, seq=random.randint(0, 61), block=random.randint(0, 15), frame=random.randint(0, 63))
        fb = list(fb)
        for _ in range(random.choice([0, 0, 0, 1, 2, 4])):
            k = random.randrange(24, len(fb))
            fb[k] = "1" if fb[k] == "0" else "0"
        sets[name].append(line("".join(fb) + rbits(random.choice([0, 0, 64])), random.randint(1626110000, 1626500000), t, i))

# rare: payloads in real frames (the 312 payload bits follow the 46 LCW bits)
raw = open(os.path.join(corpus, "fc10m.raw")).read().split("\n")
exp = open(os.path.join(corpus, "fc10m.expected.txt")).read().split("\n")
voc = [raw[k] for k, e in enumerate(exp) if e.startswith("VOC:")][:200]
iiu = [raw[k] for k, e in enumerate(exp) if e.startswith("IIU:")][:200]


def with_payload(tl, pbytes_f):
    head, bits = tl.rsplit(" ", 1)
    b = swap(bits)
    b = b[:24 + 46] + "".join(format(x, "08b") for x in pbytes_f) + b[24 + 46 + 312:]
    return head + " " + swap(b)


for i in range(1500):
    m = [random.randrange(256) for _ in range(31)]
    sets["rare"].append(with_payload(random.choice(voc), list(reedsolo.rs_encode_msg(m, 16, fcr=0))[:39]))    # VOD
    d = [random.randrange(256) for _ in range(36)]
    tl = tail_for_zero_crc(d)
    if tl:
        sets["rare"].append(with_payload(random.choice(voc), [int(format(x, "08b")[::-1], 2) for x in d + tl]))   # VDA
    m = [random.randrange(256) for _ in range(29)]
    mm = next(m + [v & 0xff, v >> 8] for v in range(65536) if checksum_16(m + [v & 0xff, v >> 8]) == 0)
    cw = list(reedsolo.rs_encode_msg(mm, 16, fcr=0))[:39]
    for _ in range(random.choice([0, 0, 1, 2])):
        cw[random.randrange(39)] = random.randrange(256)
    sets["rare"].append(with_payload(random.choice(iiu), cw))                                                  # IIR

os.makedirs(out, exist_ok=True)
for name, lines in sets.items():
    rawp = os.path.join(out, name + ".raw")
    open(rawp, "w").write("\n".join(lines) + "\n")
    with open(rawp) as fi, open(os.path.join(out, name + ".expected.txt"), "w") as fo:
        subprocess.run([sys.executable, os.path.join(toolkit, "iridium-parser.py")], stdin=fi, stdout=fo,
                       stderr=subprocess.DEVNULL, check=True)
    print("%-5s %6d lines" % (name, len(lines)))
