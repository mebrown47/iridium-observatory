#!/usr/bin/env python3
"""
sky_coverage.py - where in the sky this station hears Iridium (GET /api/coverage).

A thread polls the receiver's per-frame feed (GET /api/frames), names the
satellite behind each downlink burst by Doppler (orbit.identify_frame, with
the station position, clock offset and receiver offset that sky.py works
out), and counts it in the sky cell that satellite occupied: 10 degrees of
azimuth by 5 of elevation. Decoded frames and bursts whose unique word
failed are counted apart, so each cell has a decode success rate.

Counts go to their own SQLite file (coverage.db beside the Observatory
database) as hourly rows; the dashboard database stays rollup-only.

Copyright (c) 2026 Mike Brown
SPDX-License-Identifier: GPL-3.0-or-later
"""

import json
import math
import sqlite3
import threading
import time
import urllib.request

import orbit

POLL_S = 5
FLUSH_S = 60
AZ_STEP, EL_STEP = 10, 5
N_AZ, N_EL = 360 // AZ_STEP, 90 // EL_STEP
MIN_CELL = 30           # bursts a cell needs before its rate is shown
HORIZON_RATE = 0.5      # lowest elevation per azimuth with this success rate

SCHEMA = """
CREATE TABLE IF NOT EXISTS coverage_hourly (
    hour INTEGER, sat INTEGER, az INTEGER, el INTEGER, type TEXT,
    n INTEGER, level_sum REAL, level_sq REAL,
    PRIMARY KEY (hour, sat, az, el, type)
);
CREATE TABLE IF NOT EXISTS coverage_stats (
    hour INTEGER PRIMARY KEY, frames INTEGER, attributed INTEGER,
    ambiguous INTEGER, unplaced INTEGER, uplink INTEGER
);
"""


def cell_of(az, el):
    """Sky cell (az index, el index) of a direction; None below the horizon."""
    if el < -2.0:
        return None
    el = min(max(el, 0.0), 89.999)
    return int(az % 360 // AZ_STEP), int(el // EL_STEP)


class CoverageCollector:
    def __init__(self, sniffer_url, db_path, tracker):
        self.sniffer_url = sniffer_url.rstrip("/")
        self.db_path = db_path
        self.tracker = tracker
        self.seq = None
        self.lock = threading.Lock()
        self.cells = {}         # (hour, sat, az, el, type) -> [n, sum, sq]
        self.stats = {}         # hour -> [frames, attributed, ambiguous, unplaced, uplink]
        self.note = "starting"
        self.lost = 0
        # Failed bursts never locked onto a unique word, so their frequency
        # is rougher than a decoded frame's; one margin for both either
        # drops most failures (biasing success rates up) or loosens the
        # decoded frames. Each kind learns its own.
        self.margin = orbit.AdaptiveMargin()
        self.margin_fail = orbit.AdaptiveMargin()
        con = sqlite3.connect(db_path)
        con.executescript(SCHEMA)
        con.execute("PRAGMA journal_mode=WAL")
        con.close()

    # ---- collection
    def _poll(self):
        url = "%s/api/frames?since=%d" % (self.sniffer_url, self.seq or 0)
        with urllib.request.urlopen(url, timeout=10) as r:
            d = json.loads(r.read().decode("utf-8"))
        if self.seq is None:                 # start from now
            self.seq = d["head"]
            return
        self.seq = d["seq"]
        self.lost += d.get("lost", 0)
        sh = self.tracker.shared
        c, st, clock, rx = sh["c"], sh["station"], sh["clock"], sh["rx_offset"]
        if not c.sats or st is None or clock is None or rx is None:
            self.note = ("waiting for orbit data" if not c.sats else
                         "waiting for the station position, clock and frequency offsets from ring alerts")
            return
        self.note = ""
        types = d["types"]
        fail_type = types.index("FAIL") if "FAIL" in types else -1
        cand = {}
        for _, tms, f, mag, ty, dr, _sid in d["f"]:
            t = tms / 1000.0 + clock
            h = int(t // 3600 * 3600)
            stv = self.stats.setdefault(h, [0, 0, 0, 0, 0])
            stv[0] += 1
            if dr == 1:                      # uplink: sent by a handset, not a satellite
                stv[4] += 1
                continue
            b = int(t // 10)
            if b not in cand:
                cand[b] = [v["index"] for v in c.visible(b * 10 + 5, st, -4.0)]
            am = self.margin_fail if ty == fail_type else self.margin
            m = am.margin()
            r = orbit.identify_frame(c, t, f, st, rx, cand[b], margin_hz=m, max_residual_hz=m)
            am.add(r)
            if r is None:
                stv[3] += 1
                continue
            if r["ambiguous"]:
                stv[2] += 1
                continue
            cell = cell_of(r["az"], r["el"])
            if cell is None:
                stv[3] += 1
                continue
            stv[1] += 1
            key = (h, c.sats[r["sat"]].norad, cell[0], cell[1], types[ty] if 0 <= ty < len(types) else "?")
            acc = self.cells.get(key)
            if acc is None:
                acc = self.cells[key] = [0, 0.0, 0.0]
            acc[0] += 1
            acc[1] += mag
            acc[2] += mag * mag

    def _flush(self):
        with self.lock:
            cells, self.cells = self.cells, {}
            stats, self.stats = self.stats, {}
        if not cells and not stats:
            return
        con = sqlite3.connect(self.db_path, timeout=10)
        con.executemany(
            "INSERT INTO coverage_hourly VALUES(?,?,?,?,?,?,?,?) ON CONFLICT(hour,sat,az,el,type) "
            "DO UPDATE SET n=n+excluded.n, level_sum=level_sum+excluded.level_sum, "
            "level_sq=level_sq+excluded.level_sq",
            [(*k, v[0], v[1], v[2]) for k, v in cells.items()])
        con.executemany(
            "INSERT INTO coverage_stats VALUES(?,?,?,?,?,?) ON CONFLICT(hour) DO UPDATE SET "
            "frames=frames+excluded.frames, attributed=attributed+excluded.attributed, "
            "ambiguous=ambiguous+excluded.ambiguous, unplaced=unplaced+excluded.unplaced, "
            "uplink=uplink+excluded.uplink",
            [(h, *v) for h, v in stats.items()])
        con.commit()
        con.close()

    def run(self):
        last_flush = time.time()
        while True:
            try:
                with self.lock:
                    self._poll()
            except Exception as e:
                self.note = "cannot read %s/api/frames (%s): needs a receiver from 2026-11-09 or later" % (
                    self.sniffer_url, e)
            if time.time() - last_flush >= FLUSH_S:
                try:
                    self._flush()
                except Exception as e:
                    self.note = "coverage database error: %s" % e
                last_flush = time.time()
            time.sleep(POLL_S)

    def start(self):
        threading.Thread(target=self.run, name="coverage", daemon=True).start()

    # ---- the map
    def query(self, hours=24):
        """Per-cell totals over the last `hours`, the horizon line, and how
        the attribution went."""
        since = int(time.time() // 3600 * 3600 - (hours - 1) * 3600)
        con = sqlite3.connect(self.db_path, timeout=10)
        rows = con.execute(
            "SELECT az, el, SUM(CASE WHEN type='FAIL' THEN 0 ELSE n END), "
            "SUM(CASE WHEN type='FAIL' THEN n ELSE 0 END), SUM(n), SUM(level_sum), "
            "COUNT(DISTINCT sat) FROM coverage_hourly WHERE hour>=? GROUP BY az, el", (since,)).fetchall()
        st = con.execute("SELECT SUM(frames), SUM(attributed), SUM(ambiguous), SUM(unplaced), "
                         "SUM(uplink), MIN(hour) FROM coverage_stats WHERE hour>=?", (since,)).fetchone()
        first = con.execute("SELECT MIN(hour) FROM coverage_hourly").fetchone()[0]
        con.close()
        cells = []
        grid = {}
        for az, el, ok, fail, n, lsum, nsat in rows:
            rate = ok / n if n else None
            cells.append({"az": az, "el": el, "ok": ok, "fail": fail, "n": n,
                          "rate": round(rate, 4) if rate is not None else None,
                          "level": round(lsum / n, 2) if n else None, "sats": nsat})
            grid[(az, el)] = (n, rate)
        horizon = []
        for a in range(N_AZ):
            h = None
            for e in range(N_EL):
                n, rate = grid.get((a, e), (0, None))
                if n >= MIN_CELL and rate is not None and rate >= HORIZON_RATE:
                    h = e * EL_STEP
                    break
            horizon.append(h)
        frames, attributed, ambiguous, unplaced, uplink, _ = st if st else (0, 0, 0, 0, 0, None)
        sh = self.tracker.shared
        station = sh["station"]
        return {
            "hours": hours, "since": since, "first_data": first,
            "az_step": AZ_STEP, "el_step": EL_STEP, "min_cell": MIN_CELL,
            "horizon_rate": HORIZON_RATE, "cells": cells, "horizon": horizon,
            "stats": {"frames": frames or 0, "attributed": attributed or 0,
                      "ambiguous": ambiguous or 0, "unplaced": unplaced or 0, "uplink": uplink or 0},
            "rx_offset_hz": sh["rx_offset"], "clock_offset_s": sh["clock"],
            # rounded: enough to say which region, not which house
            "station": {"lat": round(station[0], 1), "lon": round(station[1], 1)} if station else None,
            "margin_hz": round(self.margin.margin()),
            "margin_fail_hz": round(self.margin_fail.margin()),
            "note": self.note, "lost": self.lost,
        }
