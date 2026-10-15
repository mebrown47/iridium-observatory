#!/usr/bin/env python3
"""
orbit.py - Iridium satellites from TLEs, and the receiver's position from
ring alerts. A port of iridium-analyzer's orbit.cpp and IraAnchor
(identify.cpp), checked against the same skyfield fixtures
(tests/test_orbit.py).

SGP4 comes from the vendored pure-Python python-sgp4 in ./sgp4, so this
stays stdlib-only. Times are Unix seconds (UTC); distances km.

Ring alerts (IRA) arrive from iridium-sniffer's /api/doppler stamped with
the sniffer's frame clock, which runs behind true time by however many
samples were dropped since it started (up to about a minute a day). SGP4
needs true time: 1 s is 7.5 km along the orbit. Each IRA also carries the
position of the satellite that sent it, so the offset is recovered by
sliding each satellite along its TLE orbit until it sits where the IRA
says it was.

Copyright (c) 2026 Mike Brown
SPDX-License-Identifier: GPL-3.0-or-later
"""

import math
import os
import time
import urllib.request

from sgp4.model import Satrec, WGS72
from sgp4.propagation import gstime

PI = math.pi
DEG = PI / 180.0
C_KMS = 299792.458
WGS_A = 6378.137
WGS_F = 1.0 / 298.257223563
OMEGA_EARTH = 7.292115146706979e-5      # rad/s
JD_UNIX_EPOCH = 2440587.5

# Iridium channel plan (iridium-analyzer iridium.h)
IR_BASE = 1616e6
IR_CHAN_W = 1e7 / 240
IR_RING_ALERT = 246                     # S.07
RA_CENTER_HZ = IR_BASE + (IR_RING_ALERT + 0.5) * IR_CHAN_W

KEEP_S = 600.0                          # ring alerts used for a fix (IraAnchor kKeepS)

TLE_URL = "https://celestrak.org/NORAD/elements/gp.php?GROUP=iridium-NEXT&FORMAT=tle"
TLE_MAX_AGE_S = 24 * 3600


# ---------------------------------------------------------------- geometry

def geodetic_to_ecef_km(lat_deg, lon_deg, h_m):
    e2 = WGS_F * (2 - WGS_F)
    la, lo, h = lat_deg * DEG, lon_deg * DEG, h_m / 1000.0
    n = WGS_A / math.sqrt(1 - e2 * math.sin(la) ** 2)
    return ((n + h) * math.cos(la) * math.cos(lo),
            (n + h) * math.cos(la) * math.sin(lo),
            (n * (1 - e2) + h) * math.sin(la))


def ecef_to_geodetic(r):
    e2 = WGS_F * (2 - WGS_F)
    p = math.hypot(r[0], r[1])
    la, h = math.atan2(r[2], p * (1 - e2)), 0.0
    for _ in range(6):
        n = WGS_A / math.sqrt(1 - e2 * math.sin(la) ** 2)
        h = p / math.cos(la) - n
        la = math.atan2(r[2], p * (1 - e2 * n / (n + h)))
    return la / DEG, math.atan2(r[1], r[0]) / DEG, h


def gmst_rad(t_unix):
    return gstime(JD_UNIX_EPOCH + t_unix / 86400.0)


def doppler_hz(f_hz, range_rate_kms):
    return -f_hz * range_rate_kms / C_KMS


def gc_km(la1, lo1, la2, lo2):
    a = math.sin((la2 - la1) * DEG / 2)
    b = math.sin((lo2 - lo1) * DEG / 2)
    return 2 * 6371 * math.asin(math.sqrt(a * a + math.cos(la1 * DEG) * math.cos(la2 * DEG) * b * b))


# ---------------------------------------------------------------- constellation

class Sat:
    __slots__ = ("name", "norad", "epoch_unix", "rec")


class Constellation:
    def __init__(self):
        self.sats = []
        self.source = ""

    def load_text(self, text):
        lines = [l.rstrip("\r\n") for l in text.splitlines() if l.strip()]
        sats, i = [], 0
        while i < len(lines):
            # name line optional: accept "1 ..."/"2 ..." pairs with or without it
            name = ""
            if not lines[i].startswith("1 "):
                name = lines[i].strip()
                i += 1
            if i + 1 >= len(lines) or not lines[i].startswith("1 ") or not lines[i + 1].startswith("2 "):
                i += 1
                continue
            l1, l2 = lines[i], lines[i + 1]
            i += 2
            try:
                rec = Satrec.twoline2rv(l1, l2, WGS72)
            except Exception:
                continue
            if rec.error:
                continue
            s = Sat()
            s.norad = int(l1[2:7])
            s.name = name or "NORAD %d" % s.norad
            s.epoch_unix = (rec.jdsatepoch - JD_UNIX_EPOCH + rec.jdsatepochF) * 86400.0
            s.rec = rec
            sats.append(s)
        self.sats = sats
        return len(sats)

    def load_file(self, path):
        with open(path, encoding="ascii", errors="replace") as f:
            n = self.load_text(f.read())
        self.source = path
        return n

    def newest_epoch(self):
        return max((s.epoch_unix for s in self.sats), default=0.0)

    def state_ecef(self, i, t):
        """Earth-fixed position (km) and velocity (km/s) of satellite i at t."""
        s = self.sats[i]
        err, rt, vt = s.rec.sgp4_tsince((t - s.epoch_unix) / 60.0)
        if err:
            return None
        # TEME -> pseudo Earth-fixed: rotate by GMST (polar motion ignored, < 20 m)
        g = gmst_rad(t)
        c, sn = math.cos(g), math.sin(g)
        r = (c * rt[0] + sn * rt[1], -sn * rt[0] + c * rt[1], rt[2])
        vr = (c * vt[0] + sn * vt[1], -sn * vt[0] + c * vt[1], vt[2])
        # the frame turns with the Earth: v_ecef = R v_teme - w x r_ecef
        v = (vr[0] + OMEGA_EARTH * r[1], vr[1] - OMEGA_EARTH * r[0], vr[2])
        return r, v

    def view(self, i, t, st):
        """Look from station st = (lat_deg, lon_deg, h_m); None if propagation fails."""
        sv = self.state_ecef(i, t)
        if sv is None:
            return None
        r, v = sv
        g = geodetic_to_ecef_km(*st)
        d = (r[0] - g[0], r[1] - g[1], r[2] - g[2])
        rng = math.sqrt(d[0] ** 2 + d[1] ** 2 + d[2] ** 2)
        la, lo = st[0] * DEG, st[1] * DEG
        e = -math.sin(lo) * d[0] + math.cos(lo) * d[1]
        n = (-math.sin(la) * math.cos(lo) * d[0] - math.sin(la) * math.sin(lo) * d[1]
             + math.cos(la) * d[2])
        u = (math.cos(la) * math.cos(lo) * d[0] + math.cos(la) * math.sin(lo) * d[1]
             + math.sin(la) * d[2])
        sub_lat, sub_lon, alt = ecef_to_geodetic(r)
        return {
            "index": i,
            "range_km": rng,
            "range_rate_kms": (d[0] * v[0] + d[1] * v[1] + d[2] * v[2]) / rng,  # + = receding
            "el": math.asin(u / rng) / DEG,
            "az": math.fmod(math.atan2(e, n) / DEG + 360.0, 360.0),
            "sub_lat": sub_lat, "sub_lon": sub_lon, "alt_km": alt,
            "ecef": r,
        }

    def visible(self, t, st, min_el=0.0):
        """Every satellite above min_el, highest first."""
        out = []
        for i in range(len(self.sats)):
            v = self.view(i, t, st)
            if v and v["el"] >= min_el:
                out.append(v)
        out.sort(key=lambda v: -v["el"])
        return out

    def passes(self, i, t0, t1, st, min_el=0.0):
        """Passes of satellite i above min_el between t0 and t1: dicts of
        rise, culm, set (Unix s) and max_el. A pass under way at t0, or still
        up at t1, is clipped there. 10 s scan, then bisection and a
        golden-section search for the peak, as orbit.cpp."""
        def el(t):
            v = self.view(i, t, st)
            return v["el"] - min_el if v else -999.0

        def edge(a, b):
            ea = el(a)
            for _ in range(20):
                if b - a <= 0.05:
                    break
                m = (a + b) / 2
                em = el(m)
                if (em >= 0) == (ea >= 0):
                    a, ea = m, em
                else:
                    b = m
            return (a + b) / 2

        def peak(a, b):
            for _ in range(40):
                if b - a <= 0.05:
                    break
                m1, m2 = a + (b - a) * 0.382, a + (b - a) * 0.618
                if el(m1) < el(m2):
                    a = m1
                else:
                    b = m2
            return (a + b) / 2

        out = []
        step = 10.0
        prev_t, prev_e = t0, el(t0)
        rise = t0 if prev_e >= 0 else None
        t = t0
        while True:
            t = min(t + step, t1)
            e = el(t)
            if prev_e < 0 and e >= 0:
                rise = edge(prev_t, t)
            if e < 0 and prev_e >= 0 and rise is not None:
                st_ = edge(prev_t, t)
                c = peak(rise, st_)
                out.append({"sat": i, "rise": rise, "culm": c, "set": st_, "max_el": el(c) + min_el})
                rise = None
            elif e < 0 and prev_e < 0 and t - prev_t <= step:
                # both below: a grazing pass could hide in between; probe the middle
                m = (prev_t + t) / 2
                if el(m) >= 0:
                    r, se = edge(prev_t, m), edge(m, t)
                    c = peak(r, se)
                    out.append({"sat": i, "rise": r, "culm": c, "set": se, "max_el": el(c) + min_el})
            prev_t, prev_e = t, e
            if t >= t1:
                break
        if rise is not None:                       # still up at t1
            c = peak(rise, t1)
            out.append({"sat": i, "rise": rise, "culm": c, "set": t1, "max_el": el(c) + min_el})
        return out


def short_name(name):
    return name.rsplit(" ", 1)[-1]


# ---------------------------------------------------------------- TLEs

def newest_tle_in(directory):
    try:
        names = sorted(n for n in os.listdir(directory) if n.endswith(".tle"))
    except OSError:
        return None
    return os.path.join(directory, names[-1]) if names else None


def refresh_tles(cache_dir, url=TLE_URL, max_age_s=TLE_MAX_AGE_S, timeout=20):
    """Keep <cache_dir>/iridium-YYYYMMDD.tle no older than max_age_s, fetched
    from CelesTrak (whose guidance is to fetch at most every couple of
    hours). Returns (path or None, note). A failed fetch keeps the old file."""
    try:
        os.makedirs(cache_dir, exist_ok=True)
    except OSError as e:
        return None, "cannot create %s: %s" % (cache_dir, e)
    cur = newest_tle_in(cache_dir)
    if cur and time.time() - os.path.getmtime(cur) < max_age_s:
        return cur, ""
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "iridium-observatory"})
        with urllib.request.urlopen(req, timeout=timeout) as r:
            text = r.read().decode("ascii", "replace")
        if text.count("\n1 ") < 10:
            raise ValueError("response has no TLEs")
        path = os.path.join(cache_dir, time.strftime("iridium-%Y%m%d.tle", time.gmtime()))
        tmp = path + ".tmp"
        with open(tmp, "w") as f:
            f.write(text)
        os.replace(tmp, path)
        return path, ""
    except Exception as e:
        return cur, "TLE download failed (%s)%s" % (e, "; using " + os.path.basename(cur) if cur else "")


# ---------------------------------------------------------------- ring alerts

def _dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def _slide(c, i, p, t, iters=4):
    """Slide satellite i along its orbit from time t to where it is nearest
    the IRA position p (ECEF km). Returns (dt, distance_km) or None."""
    dt = 0.0
    for _ in range(iters):
        sv = c.state_ecef(i, t + dt)
        if sv is None:
            return None
        r, v = sv
        d = (p[0] - r[0], p[1] - r[1], p[2] - r[2])
        step = _dot(d, v) / _dot(v, v)
        dt += step
        if abs(step) < 0.01:
            break
    sv = c.state_ecef(i, t + dt)
    if sv is None:
        return None
    r = sv[0]
    return dt, math.sqrt((p[0] - r[0]) ** 2 + (p[1] - r[1]) ** 2 + (p[2] - r[2]) ** 2)


def _median(v):
    s = sorted(v)
    n = len(s)
    return (s[n // 2] if n % 2 else (s[n // 2 - 1] + s[n // 2]) / 2) if n else 0.0


class RingAlerts:
    """Ring alerts from iridium-sniffer, accumulated across /api/doppler
    polls. Each: frame time (Unix s, sniffer frame clock), frequency (Hz),
    the sniffer's satellite number, and the satellite position the IRA
    carries (ECEF km)."""

    MATCH_KM = 60.0         # IRA positions are 4 km steps; TLEs good to a few km

    def __init__(self):
        self.obs = {}           # (sniffer sat, frame time ms) -> (t, freq, (x, y, z))
        self.id_to_sat = {}     # sniffer sat number -> constellation index
        self.clock_offset = None
        self.offset_spread = None

    def add(self, sniffer_sat, t, freq, ecef):
        """Each /api/doppler poll repeats the last minute of every satellite,
        stamped in whole milliseconds against a reference that moves between
        polls, so the same IRA comes back up to 1 ms apart. One satellite's
        IRAs are seconds apart: anything within 50 ms is a repeat."""
        ms = int(round(t * 1000))
        for k in range(ms - 50, ms + 51):
            if (sniffer_sat, k) in self.obs:
                return
        self.obs[(sniffer_sat, ms)] = (t, freq, ecef)

    def prune(self, keep_s=KEEP_S):
        if not self.obs:
            return
        newest = max(o[0] for o in self.obs.values())
        self.obs = {k: o for k, o in self.obs.items() if o[0] >= newest - keep_s}

    def count(self):
        return len(self.obs)

    def identify(self, c, per_sat=6):
        """Name the satellites and find the frame clock's offset from true
        time. For a few IRAs of each sniffer satellite, every TLE satellite
        is slid along its orbit to the IRA's position; the one that reaches
        it is the sender, and how far it had to slide is the clock offset.
        Returns the number of sniffer satellites named."""
        by_id = {}
        for (sid, _), o in self.obs.items():
            by_id.setdefault(sid, []).append(o)
        offsets = []
        self.id_to_sat = {}
        for sid, lst in by_id.items():
            lst.sort(key=lambda o: o[0])
            pick = [lst[int(k * (len(lst) - 1) / max(1, per_sat - 1))] for k in range(min(per_sat, len(lst)))]
            votes = {}
            for t, _, p in pick:
                best = None
                for i in range(len(c.sats)):
                    # only satellites in the IRA's half of the sky can reach it
                    sv = c.state_ecef(i, t)
                    if sv is None or _dot(sv[0], p) < 0:
                        continue
                    m = _slide(c, i, p, t)
                    if m and abs(m[0]) < 1800 and (best is None or m[1] < best[2]):
                        best = (i, m[0], m[1])
                if best and best[2] < self.MATCH_KM:
                    votes.setdefault(best[0], []).append(best[1])
            if votes:
                i, dts = max(votes.items(), key=lambda kv: len(kv[1]))
                if len(dts) * 2 > len(pick):        # a clear majority
                    self.id_to_sat[sid] = i
                    offsets.extend(dts)
        if offsets:
            med = _median(offsets)
            self.clock_offset = med
            self.offset_spread = _median([abs(x - med) for x in offsets])
        else:
            self.clock_offset = self.offset_spread = None
        return len(self.id_to_sat)

    def observations(self):
        """(true time, freq, constellation index) for every IRA from a named
        satellite, once identify() has found the clock offset."""
        if self.clock_offset is None:
            return []
        out = []
        for (sid, _), (t, f, _) in self.obs.items():
            i = self.id_to_sat.get(sid)
            if i is not None:
                out.append((t + self.clock_offset, f, i))
        out.sort()
        return out

    def locate(self, c, window_s=KEEP_S, max_grid_obs=200):
        """Where the receiver is: the position at which the ring alerts of
        the last window_s imply one receiver frequency offset
        (IraAnchor::locate). Returns dict(lat, lon, rms_hz, n_obs, n_sats,
        rx_offset_hz) or None."""
        obs = self.observations()
        if obs:
            obs = [o for o in obs if o[0] >= obs[-1][0] - window_s]
        if len({o[2] for o in obs}) < 2:
            return None
        pre = []
        for t, f, i in obs:
            sv = c.state_ecef(i, t)
            if sv:
                pre.append((sv[0], sv[1], f))
        if len(pre) < 6:
            return None

        def cost(la, lo, data):
            e2 = WGS_F * (2 - WGS_F)
            sl, cl = math.sin(la * DEG), math.cos(la * DEG)
            so, co = math.sin(lo * DEG), math.cos(lo * DEG)
            n = WGS_A / math.sqrt(1 - e2 * sl * sl)
            g0, g1, g2 = n * cl * co, n * cl * so, n * (1 - e2) * sl
            u0, u1, u2 = cl * co, cl * so, sl
            off = []
            for r, v, f in data:
                d0, d1, d2 = r[0] - g0, r[1] - g1, r[2] - g2
                rng = math.sqrt(d0 * d0 + d1 * d1 + d2 * d2)
                if (d0 * u0 + d1 * u1 + d2 * u2) / rng < -0.02:   # below ~-1 deg: not heard
                    return 1e12, 0.0
                rr = (d0 * v[0] + d1 * v[1] + d2 * v[2]) / rng
                off.append(f - RA_CENTER_HZ * (1.0 - rr / C_KMS))
            med = sorted(off)[len(off) // 2]
            return math.sqrt(sum((x - med) ** 2 for x in off) / len(off)), med

        # where the satellites were heard: around the mean of their sub-points
        sx = sy = sz = 0.0
        for r, _, _ in pre:
            k = math.sqrt(_dot(r, r))
            sx += r[0] / k
            sy += r[1] / k
            sz += r[2] / k
        la0 = math.atan2(sz, math.hypot(sx, sy)) / DEG
        lo0 = math.atan2(sy, sx) / DEG

        # the 0.5 deg grid on an even thinning of the IRAs, then the fine
        # steps on all of them (the analyzer grids on all; same minimum)
        stride = max(1, len(pre) // max_grid_obs)
        thin = pre[::stride]
        bl, bo, bc = la0, lo0, 1e18
        la = la0 - 25
        while la <= la0 + 25 + 1e-9:
            if -89 <= la <= 89:
                lo = lo0 - 35
                while lo <= lo0 + 35 + 1e-9:
                    v = cost(la, lo, thin)[0]
                    if v < bc:
                        bc, bl, bo = v, la, lo
                    lo += 0.5
            la += 0.5
        bc = cost(bl, bo, pre)[0]
        for step in (0.1, 0.02, 0.005):
            l0, o0 = bl, bo
            for a in range(-8, 9):
                for b in range(-8, 9):
                    la, lo = l0 + a * step, o0 + b * step
                    v = cost(la, lo, pre)[0]
                    if v < bc:
                        bc, bl, bo = v, la, lo
        if bc >= 1e11:
            return None
        bo = bo - 360 if bo > 180 else bo + 360 if bo < -180 else bo
        return {"lat": bl, "lon": bo, "rms_hz": bc, "n_obs": len(pre),
                "n_sats": len({o[2] for o in obs}), "rx_offset_hz": cost(bl, bo, pre)[1]}
