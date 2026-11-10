#!/usr/bin/env python3
"""sky_coverage.py: sky cells, per-cell success rate and the horizon line,
from rows written the way the collector writes them.

  python3 observatory/tests/test_coverage.py
"""
import os
import sqlite3
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import sky_coverage as sc  # noqa: E402

fails = checks = 0


def check(ok, what, detail=""):
    global fails, checks
    checks += 1
    fails += not ok
    print("  %s: %s%s" % ("PASS" if ok else "FAIL", what, "  (%s)" % detail if detail else ""))


check(sc.cell_of(0, 0) == (0, 0) and sc.cell_of(359.9, 89.99) == (35, 17) and sc.cell_of(95, 47) == (9, 9),
      "directions fall in the right 10x5 degree cell")
check(sc.cell_of(10, -1.0) == (1, 0) and sc.cell_of(10, -3) is None, "just below the horizon clamps; below -2 deg is dropped")


class Tracker:
    shared = {"c": None, "station": (31.4, -86.1, 0.0), "clock": 0.1, "rx_offset": 1200.0}


d = tempfile.mkdtemp()
col = sc.CoverageCollector("http://127.0.0.1:1", os.path.join(d, "c.db"), Tracker())
h = int(time.time() // 3600 * 3600)
rows = []
# a station blocked to the north-east (az 30-60) below 25 deg, clear elsewhere above 5 deg
for a in range(sc.N_AZ):
    for e in range(sc.N_EL):
        el_deg, az_deg = e * sc.EL_STEP, a * sc.AZ_STEP
        blocked = 30 <= az_deg < 60 and el_deg < 25
        low = el_deg < 5
        ok, fail = (10, 90) if (blocked or low) else (90, 10)
        rows.append((h, 40000 + a, a, e, "IDA", ok, ok * 20.0, ok * 400.0))
        rows.append((h, 40000 + a, a, e, "FAIL", fail, fail * 15.0, fail * 225.0))
con = sqlite3.connect(os.path.join(d, "c.db"))
con.executemany("INSERT INTO coverage_hourly VALUES(?,?,?,?,?,?,?,?)", rows)
con.commit()
con.close()
q = col.query(24)
cell = next(c for c in q["cells"] if c["az"] == 9 and c["el"] == 9)
check(cell["n"] == 100 and abs(cell["rate"] - 0.9) < 1e-9 and cell["ok"] == 90 and cell["fail"] == 10,
      "a cell's success rate counts failed bursts against decoded ones", str(cell))
hz = q["horizon"]
check(hz[0] == 5 and hz[3] == 25 and hz[5] == 25 and hz[6] == 5 and hz[35] == 5,
      "horizon line rises where the sky is blocked", "az 0: %s, 30: %s, 50: %s, 60: %s" % (hz[0], hz[3], hz[5], hz[6]))
check(q["station"] == {"lat": 31.4, "lon": -86.1}, "station position is rounded in the API")

print("\n%d checks, %d failed" % (checks, fails))
sys.exit(1 if fails else 0)
