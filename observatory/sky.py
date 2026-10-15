#!/usr/bin/env python3
"""
sky.py - the satellites over the receiver and where the receiver is, for
the Doppler page (GET /api/sky).

A background thread polls iridium-sniffer's /api/doppler, keeps the last
20 minutes of ring alerts, and every couple of minutes names their
satellites, recovers the frame clock's offset and fits the receiver's
position (orbit.RingAlerts, the iridium-analyzer method). From that
position, or a --station the user gave, it lists the satellites above the
horizon and their passes from 20 minutes ago to 40 minutes ahead, as
iridium-analyzer's Satellites window does.

TLEs: CelesTrak's Iridium NEXT set, cached in --tle-dir and refreshed
daily; without internet, the newest *.tle there or in ~/iridium-tle (the
analyzer's folder) is used.

Copyright (c) 2026 Mike Brown
SPDX-License-Identifier: GPL-3.0-or-later
"""

import json
import os
import threading
import time
import urllib.request

import orbit

POLL_S = 20             # /api/doppler keeps ~60 s of each satellite; poll well inside it
FIX_EVERY_S = 120
SKY_EVERY_S = 30
HEARD_KEEP_S = 1200     # ring alerts kept: the pass timeline looks 20 min back
SETTLED_KM = 20.0       # consecutive fits this close: the position has settled
BACK_S, AHEAD_S = 20 * 60, 40 * 60


class SkyTracker:
    def __init__(self, sniffer_url, tle_dir, station=None, download=True):
        self.sniffer_url = sniffer_url.rstrip("/")
        self.tle_dir = tle_dir
        self.station = station          # (lat, lon, h_m) the user gave, or None
        self.download = download
        self.lock = threading.Lock()
        self.c = orbit.Constellation()
        self.tle_note = ""
        self.tle_checked = 0.0
        self.ra = orbit.RingAlerts()
        self.fix = None                 # last good fit, with "t" when made
        self.sniffer_fix = None
        self.clock = None               # frame clock behind true time (s)
        self.wall_lag = None            # wall_ms - now_ms from the sniffer
        self.feed_note = "waiting for iridium-sniffer"
        self.snapshot = {"note": "starting"}

    # ---- inputs
    def _tles(self):
        if time.time() - self.tle_checked < 3600 and self.c.sats:
            return
        self.tle_checked = time.time()
        path, note = (orbit.refresh_tles(self.tle_dir) if self.download
                      else (orbit.newest_tle_in(self.tle_dir), ""))
        alt = orbit.newest_tle_in(os.environ.get("IRIDIUM_TLE_DIR")
                                  or os.path.expanduser("~/iridium-tle"))
        if alt and (not path or os.path.getmtime(alt) > os.path.getmtime(path)):
            path = alt
        if path and path != self.c.source:
            c = orbit.Constellation()
            if c.load_file(path):
                self.c = c
        self.tle_note = note if self.c.sats else (note or "no TLEs: the Observatory "
                        "needs internet once a day for CelesTrak, or a *.tle in " + self.tle_dir)

    def _fetch(self, path, timeout=5):
        with urllib.request.urlopen(self.sniffer_url + path, timeout=timeout) as r:
            return json.loads(r.read().decode("utf-8"))

    def _poll(self):
        try:
            d = self._fetch("/api/doppler")
        except Exception as e:
            self.feed_note = "cannot reach iridium-sniffer at %s (%s)" % (self.sniffer_url, e)
            return
        if not d.get("enabled"):
            self.feed_note = d.get("note", "Doppler positioning is off in iridium-sniffer")
            return
        now_ms = d.get("now_ms", 0)
        if "wall_ms" in d:
            self.wall_lag = (d["wall_ms"] - now_ms) / 1000.0
        n_pos = 0
        for s in d.get("sats", []):
            xs, ys, zs = s.get("x"), s.get("y"), s.get("z")
            if not xs or not (len(xs) == len(ys) == len(zs) == len(s["f"]) == len(s["dt"])):
                continue
            for dt, f, x, y, z in zip(s["dt"], s["f"], xs, ys, zs):
                self.ra.add(s["sat"], (now_ms + dt) / 1000.0, f, (x, y, z))
                n_pos += 1
        self.feed_note = "" if n_pos or not d.get("sats") else (
            "iridium-sniffer is too old to report satellite positions "
            "(/api/doppler has no x/y/z): update it for the location and satellite names")
        self.ra.prune(HEARD_KEEP_S)

    def _poll_sniffer_fix(self):
        try:
            rx = self._fetch("/api/state", timeout=8).get("rx")
        except Exception:
            return
        self.sniffer_fix = {"lat": rx["lat"], "lon": rx["lon"], "hdop": rx.get("hdop")} if rx else None

    # ---- outputs
    def _locate(self):
        if not self.c.sats or not self.ra.count():
            return
        self.ra.identify(self.c)
        self.clock = self.ra.clock_offset
        fix = self.ra.locate(self.c)
        if fix:
            # An early fit on a few IRAs of satellites in one plane can sit
            # hundreds of km out; call it settled once two fits in a row agree.
            prev = self.fix
            fix["moved_km"] = (orbit.gc_km(prev["lat"], prev["lon"], fix["lat"], fix["lon"])
                               if prev else None)
            fix["settled"] = fix["moved_km"] is not None and fix["moved_km"] < SETTLED_KM
            fix["t"] = time.time()
            self.fix = fix

    def _sky(self):
        now = time.time()
        snap = {"now": now, "tle": {"n": len(self.c.sats), "source": os.path.basename(self.c.source),
                                    "note": self.tle_note},
                "feed_note": self.feed_note, "ring_alerts": self.ra.count(),
                "clock_offset_s": self.clock, "wall_lag_s": self.wall_lag,
                "sniffer_fix": self.sniffer_fix}
        if self.c.sats:
            snap["tle"]["age_days"] = (now - self.c.newest_epoch()) / 86400.0
        if self.fix:
            f = dict(self.fix)
            f["age_s"] = now - f.pop("t")
            snap["fix"] = f
            if self.sniffer_fix:
                snap["sniffer_fix"]["dist_km"] = orbit.gc_km(
                    f["lat"], f["lon"], self.sniffer_fix["lat"], self.sniffer_fix["lon"])
        if self.station:
            st, src = self.station, "configured"
        elif self.fix:
            st, src = (self.fix["lat"], self.fix["lon"], 0.0), "ring alerts"
        else:
            st, src = None, None
        names = {}
        for sid, i in self.ra.id_to_sat.items():
            names[str(sid)] = self.c.sats[i].name
        snap["names"] = names
        if st is None or not self.c.sats:
            snap["note"] = ("no TLEs yet" if not self.c.sats else
                            "waiting for ring alerts from two satellites to find where the receiver is")
            return snap
        snap["station"] = {"lat": st[0], "lon": st[1], "source": src}
        heard = {}
        for t, _, i in self.ra.observations():
            heard.setdefault(i, []).append(round(t, 1))
        rows = []
        for v in self.c.visible(now, st, 0.0):
            v2 = self.c.view(v["index"], now + 1.0, st)
            d1 = orbit.doppler_hz(orbit.RA_CENTER_HZ, v["range_rate_kms"])
            d2 = orbit.doppler_hz(orbit.RA_CENTER_HZ, v2["range_rate_kms"]) if v2 else d1
            s = self.c.sats[v["index"]]
            rows.append({"name": s.name, "short": orbit.short_name(s.name), "norad": s.norad,
                         "el": round(v["el"], 1), "az": round(v["az"]), "doppler_hz": round(d1),
                         "drift_hz_s": round(d2 - d1), "heard": len(heard.get(v["index"], []))})
        snap["visible"] = rows
        passes = []
        for i, s in enumerate(self.c.sats):
            for p in self.c.passes(i, now - BACK_S, now + AHEAD_S, st):
                # elevation along the pass every 20 s, for the bar's shading
                el, t = [], p["rise"]
                while t <= p["set"] + 20:
                    v = self.c.view(i, min(t, p["set"]), st)
                    el.append(round(max(0.0, v["el"]), 1) if v else 0.0)
                    t += 20
                passes.append({"name": s.name, "short": orbit.short_name(s.name), "norad": s.norad,
                               "rise": p["rise"], "culm": p["culm"], "set": p["set"],
                               "max_el": round(p["max_el"], 1), "el": el,
                               "heard": [h for h in heard.get(i, []) if p["rise"] - 5 <= h <= p["set"] + 5]})
        passes.sort(key=lambda p: p["rise"])
        snap["passes"] = passes
        snap["window"] = [now - BACK_S, now + AHEAD_S]
        return snap

    def run(self):
        last_fix = last_sky = last_rx = 0.0
        while True:
            try:
                self._tles()
                self._poll()
                now = time.time()
                if now - last_rx >= FIX_EVERY_S:
                    self._poll_sniffer_fix()
                    last_rx = now
                if now - last_fix >= FIX_EVERY_S or (self.fix is None and now - last_fix >= POLL_S):
                    self._locate()
                    last_fix = now
                    last_sky = 0.0
                if now - last_sky >= SKY_EVERY_S:
                    snap = self._sky()
                    with self.lock:
                        self.snapshot = snap
                    last_sky = now
            except Exception as e:      # keep tracking; say what broke
                with self.lock:
                    self.snapshot = dict(self.snapshot, note="sky tracker error: %s" % e)
            time.sleep(POLL_S)

    def start(self):
        threading.Thread(target=self.run, name="sky", daemon=True).start()

    def get(self):
        with self.lock:
            return dict(self.snapshot, age_s=time.time() - self.snapshot.get("now", time.time()))
