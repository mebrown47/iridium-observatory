#!/usr/bin/env python3
"""The gate for the Flights page's estimates (context.ESTIMATES_ENABLED).

Replays a station's stored Iridium messages through context.py, and wherever
a VDL2 receiver saw the same airframe (vdl2_pos: real positions from XID and
the aircraft's own reports), asks the estimate made from the latest flight
plan at that moment where the aircraft should be. An estimate passes if the
true position lies inside its circle.

Estimates may be shown only if at least 80% of checkable ones pass. Also
reported: how big the circles were, since a huge circle passes trivially and
says nothing.

  check_estimates.py IRIDIUM_DB VDL2_DB AIRCRAFT_CSV [--days N] [--json OUT]
"""
import argparse
import collections
import json
import os
import re
import sqlite3
import statistics
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import context as cx  # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument("iridium_db")
ap.add_argument("vdl2_db")
ap.add_argument("aircraft_csv")
ap.add_argument("--days", type=float, default=30)
ap.add_argument("--json")
ap.add_argument("--notices", action="store_true", help="also estimate from plan notices")
a = ap.parse_args()
KINDS = ("plan", "plan_notice") if a.notices else ("plan",)

ref = cx.Reference(aircraft_csv=a.aircraft_csv)
hex2reg = {}
with open(a.aircraft_csv, encoding="utf-8", errors="replace") as f:
    for line in f:
        p = line.split(";")
        if len(p) > 1 and p[1]:
            hex2reg[p[0].upper()] = cx.norm_reg(p[1])

vdl = sqlite3.connect("file:%s?mode=ro" % a.vdl2_db, uri=True)
tracks = collections.defaultdict(list)
for t, icao, la, lo in vdl.execute("SELECT t, icao, lat, lon FROM vdl2_pos WHERE lat IS NOT NULL"):
    r = hex2reg.get((icao or "").upper())
    if r:
        tracks[r].append((t, (la, lo)))
vmin = vdl.execute("SELECT min(t) FROM vdl2_pos").fetchone()[0]

iri = sqlite3.connect("file:%s?mode=ro" % a.iridium_db, uri=True)
tmax = iri.execute("SELECT max(t) FROM messages").fetchone()[0]
t0 = max(vmin, tmax - a.days * 86400)
items = collections.defaultdict(list)
for mid, t, reg, d, lbl, txt in iri.execute(
        "SELECT id, t, reg, dir, lbl, txt FROM messages WHERE t >= ? AND reg != ''", (t0 - 6 * 3600,)):
    r = cx.norm_reg(reg)
    for it in cx.infer(lbl, txt, d, ref):
        it.update(t=t, id=mid)
        items[r].append(it)

res = collections.Counter()
radii, misses, per_kind = [], [], collections.defaultdict(collections.Counter)
for reg, its in items.items():
    plans = sorted((i for i in its if i["kind"] in KINDS), key=lambda i: i["t"])
    if not plans or reg not in tracks:
        continue
    typ = ref.types.get(reg)
    for tt, p in tracks[reg]:
        # the newest plan at that moment, no older than 8 h
        cur = [pl for pl in plans if pl["t"] <= tt and tt - pl["t"] <= 8 * 3600]
        if not cur:
            continue
        pl = cur[-1]
        t_dep, doubt = cx.departure(pl, its)
        est = cx.estimate(pl["route"], t_dep, doubt, tt, typ)
        if est is None:
            continue
        o, d = pl["route"][0], pl["route"][-1]
        if est["status"] == "en_route":
            dist = cx.km(p, (est["lat"], est["lon"]))
            ok = dist <= est["radius_km"]
            res["en_route_checked"] += 1
            res["en_route_inside"] += ok
            radii.append(est["radius_km"])
            per_kind[pl["kind"]]["checked"] += 1
            per_kind[pl["kind"]]["inside"] += ok
            if not ok and len(misses) < 15:
                misses.append({"reg": reg, "kind": pl["kind"], "o": pl.get("origin"),
                               "d": pl.get("dest"), "mins_after_plan": round((tt - pl["t"]) / 60),
                               "off_km": round(dist), "radius_km": est["radius_km"],
                               "from_route_km": round(min(cx.km(p, q) for q in pl["route"]))})
        else:
            # "not departed" should find it near the origin, "arrived" near
            # the destination; anything else is a miss for the page's text
            near = cx.km(p, o if est["status"] == "not_departed" else d)
            res[est["status"] + "_checked"] += 1
            res[est["status"] + "_ok"] += near <= 50

n = res["en_route_checked"]
summary = {
    "window_days": a.days, "airframes_with_vdl2": len(tracks),
    "counts": dict(res),
    "per_kind": {k: dict(v) for k, v in per_kind.items()},
    "en_route_inside_pct": round(100.0 * res["en_route_inside"] / n, 1) if n else None,
    "radius_km": {"median": statistics.median(radii) if radii else None,
                  "p90": sorted(radii)[int(len(radii) * 0.9)] if radii else None},
    "gate_80pct": bool(n and res["en_route_inside"] / n >= 0.8),
    "misses": misses,
}
print(json.dumps(summary, indent=1))
if a.json:
    json.dump(summary, open(a.json, "w"), indent=1)
