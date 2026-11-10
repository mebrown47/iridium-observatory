#!/usr/bin/env python3
"""
attribution_check.py - score Doppler satellite attribution on a live station.

Reads the receiver's per-frame feed (GET /api/frames) and the Observatory's
sky state (GET /api/sky) for a while, attributes every downlink burst to a
satellite by Doppler (orbit.identify_frame), and scores it against ground
truth: ring alerts on the ring-alert channel whose satellite number the
Observatory has already named from the satellite position they carry.

    python3 observatory/tools/attribution_check.py --minutes 30
    python3 observatory/tools/attribution_check.py --sniffer http://127.0.0.1:8888 \
        --observatory http://127.0.0.1:8890 --tle-dir /var/lib/iridium-observatory/tle

Copyright (c) 2026 Mike Brown
SPDX-License-Identifier: GPL-3.0-or-later
"""

import argparse
import collections
import json
import os
import statistics
import sys
import time
import urllib.request

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import orbit  # noqa: E402


def fetch(url, timeout=10):
    with urllib.request.urlopen(url, timeout=timeout) as r:
        return json.loads(r.read().decode("utf-8"))


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description="score Doppler satellite attribution live")
    ap.add_argument("--sniffer", default="http://127.0.0.1:8888")
    ap.add_argument("--observatory", default="http://127.0.0.1:8890")
    ap.add_argument("--tle-dir", default=None,
                    help="folder with Iridium *.tle (default: the Observatory's tle/ caches)")
    ap.add_argument("--minutes", type=float, default=30)
    ap.add_argument("--poll", type=float, default=5)
    args = ap.parse_args()

    tle = None
    for d in filter(None, [args.tle_dir, os.path.join(here, "..", "tle"),
                           "/var/lib/iridium-observatory/tle", os.path.expanduser("~/iridium-tle")]):
        tle = orbit.newest_tle_in(d)
        if tle:
            break
    if not tle:
        sys.exit("no *.tle found; pass --tle-dir")
    c = orbit.Constellation()
    c.load_file(tle)
    by_name = {s.name: i for i, s in enumerate(c.sats)}
    print("TLEs: %s (%d satellites)" % (tle, len(c.sats)))

    seq, sky, sky_t = 0, None, 0.0
    offsets = collections.deque()           # (true t, rx offset sample)
    truth = collections.Counter()           # right / wrong / ambiguous / none on named IRAs
    types = collections.defaultdict(collections.Counter)
    resid = []
    wrong_examples = []
    lost = 0
    t_end = time.time() + args.minutes * 60
    first = True
    while time.time() < t_end:
        if time.time() - sky_t > 30:
            try:
                sky = fetch(args.observatory + "/api/sky")
                sky_t = time.time()
            except Exception as e:
                print("cannot read %s/api/sky: %s" % (args.observatory, e))
        try:
            d = fetch("%s/api/frames?since=%d" % (args.sniffer, seq))
        except Exception as e:
            sys.exit("cannot read %s/api/frames: %s (is the receiver new enough?)" % (args.sniffer, e))
        if first:                            # start from now, not the ring's backlog
            seq, first = d["head"], False
            time.sleep(args.poll)
            continue
        seq, lost = d["seq"], lost + d.get("lost", 0)
        names = (sky or {}).get("names", {})
        fix = (sky or {}).get("fix")
        clock = (sky or {}).get("clock_offset_s")
        if not fix or clock is None:
            print("waiting for the Observatory to fix the station position and clock offset...")
            time.sleep(args.poll)
            continue
        st = (fix["lat"], fix["lon"], 0.0)
        tnames = d["types"]
        fail_type = tnames.index("FAIL")
        ira_type = tnames.index("IRA")
        frames = d["f"]

        # receiver offset from named ring alerts on S.07, short window
        for s_, tms, f, mag, ty, dr, sid in frames:
            if ty == ira_type and sid >= 0 and abs(f - orbit.RA_CENTER_HZ) <= orbit.RA_WINDOW_HZ:
                i = by_name.get(names.get(str(sid)))
                if i is not None:
                    t = tms / 1000.0 + clock
                    v = c.view(i, t, st)
                    if v:
                        offsets.append((t, f - orbit.RA_CENTER_HZ * (1 - v["range_rate_kms"] / orbit.C_KMS)))
        while offsets and frames and offsets[0][0] < frames[-1][1] / 1000.0 + clock - 120:
            offsets.popleft()
        if not offsets:
            print("waiting for named ring alerts (receiver offset)...")
            time.sleep(args.poll)
            continue
        rx = statistics.median(o for _, o in offsets)

        cand_cache = {}
        for s_, tms, f, mag, ty, dr, sid in frames:
            if dr == 1:                      # uplink: not a satellite's transmission
                continue
            t = tms / 1000.0 + clock
            b = int(t // 10)
            if b not in cand_cache:
                cand_cache[b] = [v["index"] for v in c.visible(b * 10 + 5, st, -4.0)]
            r = orbit.identify_frame(c, t, f, st, rx, cand_cache[b])
            label = tnames[ty] if 0 <= ty < len(tnames) else "?"
            types[label]["frames"] += 1
            if r is None:
                types[label]["none"] += 1
            elif r["ambiguous"]:
                types[label]["ambiguous"] += 1
            else:
                types[label]["attributed"] += 1
                resid.append(r["residual_hz"])
            # ground truth: named ring alerts on the ring-alert channel
            if ty == ira_type and sid >= 0 and abs(f - orbit.RA_CENTER_HZ) <= orbit.RA_WINDOW_HZ:
                want = by_name.get(names.get(str(sid)))
                if want is None:
                    continue
                if r is None:
                    truth["none"] += 1
                elif r["ambiguous"]:
                    truth["ambiguous"] += 1
                elif r["sat"] == want:
                    truth["right"] += 1
                else:
                    truth["wrong"] += 1
                    if len(wrong_examples) < 5:
                        wrong_examples.append((c.sats[want].name, c.sats[r["sat"]].name,
                                               round(r["residual_hz"]), round(r["runner_up_hz"])))
        decided = truth["right"] + truth["wrong"]
        print("%s  rx %+6.0f Hz  named IRAs: %d right, %d wrong, %d ambiguous, %d none%s" % (
            time.strftime("%H:%M:%S"), rx, truth["right"], truth["wrong"], truth["ambiguous"],
            truth["none"], ("  (%.1f%% right when decided)" % (100.0 * truth["right"] / decided)) if decided else ""))
        time.sleep(args.poll)

    print("\n=== summary ===")
    decided = truth["right"] + truth["wrong"]
    total = sum(truth.values())
    if total:
        print("ground truth (named ring alerts on S.07): %d frames" % total)
        print("  right %d, wrong %d, ambiguous %d, none %d" % (truth["right"], truth["wrong"], truth["ambiguous"], truth["none"]))
        if decided:
            print("  accuracy when decided: %.2f%%   decided: %.1f%% of frames" % (
                100.0 * truth["right"] / decided, 100.0 * decided / total))
    for w in wrong_examples:
        print("  wrong: truth %s, got %s (residual %+d Hz, runner-up %d Hz)" % w)
    print("all downlink frames by type (attributed / ambiguous / none):")
    for label, cnt in sorted(types.items(), key=lambda kv: -kv[1]["frames"]):
        n = cnt["frames"]
        print("  %-12s %7d   %5.1f%% / %5.1f%% / %5.1f%%" % (
            label, n, 100.0 * cnt["attributed"] / n, 100.0 * cnt["ambiguous"] / n, 100.0 * cnt["none"] / n))
    if resid:
        a = sorted(abs(x) for x in resid)
        print("attributed residuals: median %.0f Hz, 95th percentile %.0f Hz" % (a[len(a) // 2], a[int(len(a) * 0.95)]))
    if lost:
        print("records lost from the ring between polls: %d (poll faster)" % lost)


if __name__ == "__main__":
    main()
