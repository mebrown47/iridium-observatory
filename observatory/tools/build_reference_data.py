#!/usr/bin/env python3
"""Build observatory/data/airports.csv and navaids.csv, the reference data
the Flights page resolves place names with.

  airports.csv  from OurAirports (public domain):
                https://davidmegginson.github.io/ourairports-data/airports.csv
                open airports with a 4-letter code: code,lat,lon,name,iata
  navaids.csv   from faa_navaid_database.json in
                github.com/mebrown47/iridium-aircraft-geolocation (FAA
                aeronautical data, public domain): every 2-4 letter ident
                (VOR, VORTAC, NDB...), and 5-letter fixes the engine's
                data/waypoints.csv lacks. id,lat,lon -- one row per
                candidate; identifiers repeat worldwide, and the page picks
                the candidate nearest the route.

  build_reference_data.py AIRPORTS_CSV NAVAID_JSON WAYPOINTS_CSV OUT_DIR
"""
import csv, json, os, sys

ap_in, nav_in, wp_in, out = sys.argv[1:5]
TYPES = {"small_airport", "medium_airport", "large_airport"}

rows = {}
with open(ap_in, newline="", encoding="utf-8") as f:
    for r in csv.DictReader(f):
        if r["type"] not in TYPES:
            continue
        try:
            la, lo = float(r["latitude_deg"]), float(r["longitude_deg"])
        except ValueError:
            continue
        for k in (r["icao_code"], r["gps_code"], r["ident"]):
            if k and len(k) == 4 and k.isalpha() and k.isupper():
                # a larger airport wins a shared code
                rank = ("small_airport", "medium_airport", "large_airport").index(r["type"])
                if k not in rows or rank > rows[k][0]:
                    rows[k] = (rank, round(la, 5), round(lo, 5), r["name"], r["iata_code"])
                break
with open(os.path.join(out, "airports.csv"), "w", newline="", encoding="utf-8") as f:
    w = csv.writer(f)
    w.writerow(["code", "lat", "lon", "name", "iata"])
    for k in sorted(rows):
        _, la, lo, name, iata = rows[k]
        w.writerow([k, la, lo, name, iata])

have = set()
with open(wp_in) as f:
    for line in f:
        have.add(line.split(",", 1)[0])
nav = json.load(open(nav_in))
n = 0
with open(os.path.join(out, "navaids.csv"), "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["id", "lat", "lon"])
    for ident in sorted(nav):
        if not ident.isalpha() or not (2 <= len(ident) <= 5):
            continue
        if len(ident) == 5 and ident in have:
            continue
        v = nav[ident]
        cands = v if v and isinstance(v[0], list) else [v]
        for la, lo in cands:
            if -90 <= la <= 90 and -180 <= lo <= 180 and (la, lo) != (0, 0):
                w.writerow([ident, round(la, 5), round(lo, 5)]); n += 1
print("airports %d, navaid rows %d" % (len(rows), n))
