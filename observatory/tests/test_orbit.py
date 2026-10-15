#!/usr/bin/env python3
"""orbit.py against skyfield (the fixtures are iridium-analyzer's: made by its
tests/orbit_oracle.py and passes_oracle.py from iridium_tle_20260927.tle),
with the same tolerances as its test_orbit.cpp, plus ring-alert location
from synthetic IRAs whose frame clock runs behind true time.

  python3 observatory/tests/test_orbit.py
"""
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import orbit  # noqa: E402

FIX = os.path.join(HERE, "fixtures")
fails = checks = 0


def check(ok, what, detail=""):
    global fails, checks
    checks += 1
    fails += not ok
    print("  %s: %s%s" % ("PASS" if ok else "FAIL", what, "  (%s)" % detail if detail else ""))


c = orbit.Constellation()
n = c.load_file(os.path.join(FIX, "iridium_tle_20260927.tle"))
check(n >= 66, "TLEs load", "%d satellites" % n)
by_norad = {s.norad: i for i, s in enumerate(c.sats)}

# look angles, range rate, subpoint vs skyfield
st = (46.83, -100.89, 520.0)
rows = 0
w = dict(el=0, az=0, rng=0, rr=0, sub=0, alt=0)
for line in open(os.path.join(FIX, "orbit_expected.txt")):
    if line.startswith("#"):
        continue
    norad, t, el, az, rng, rr, sla, slo, alt = line.split()
    norad, t = int(norad), float(t)
    v = c.view(by_norad[norad], t, st)
    rows += 1
    rem = lambda x: math.remainder(x, 360.0)
    w["el"] = max(w["el"], abs(v["el"] - float(el)))
    w["az"] = max(w["az"], abs(rem(v["az"] - float(az))) * math.cos(float(el) * orbit.DEG))
    w["rng"] = max(w["rng"], abs(v["range_km"] - float(rng)))
    w["rr"] = max(w["rr"], abs(v["range_rate_kms"] - float(rr)))
    w["sub"] = max(w["sub"], abs(v["sub_lat"] - float(sla)), abs(rem(v["sub_lon"] - float(slo))))
    w["alt"] = max(w["alt"], abs(v["alt_km"] - float(alt)))
check(rows >= 20 and w["el"] < 0.01 and w["az"] < 0.01 and w["rng"] < 0.2 and w["rr"] < 0.002
      and w["sub"] < 0.01 and w["alt"] < 0.05, "matches skyfield (look angles, range rate, subpoint)",
      "%d rows: el %.4f az %.4f deg, range %.3f km, rr %.2f m/s, sub %.4f deg, alt %.3f km"
      % (rows, w["el"], w["az"], w["rng"], w["rr"] * 1e3, w["sub"], w["alt"]))

check(abs(orbit.doppler_hz(1626.27e6, -7.0) - 37972.6) < 0.5, "Doppler sign and size")

# passes vs skyfield's find_events
gs = (46.8594, -101.0093, 520.0)
want = found = 0
we = wc = wm = 0.0
for line in open(os.path.join(FIX, "orbit_passes.txt")):
    if line.startswith("#"):
        continue
    norad, r, cu, se, me = line.split()
    r, cu, se, me = float(r), float(cu), float(se), float(me)
    want += 1
    for p in c.passes(by_norad[int(norad)], 1790500000.0, 1790500000.0 + 6 * 3600, gs):
        if abs(p["culm"] - cu) < 60:
            found += 1
            we = max(we, abs(p["rise"] - r), abs(p["set"] - se))
            wc = max(wc, abs(p["culm"] - cu))
            wm = max(wm, abs(p["max_el"] - me))
check(want >= 8 and found == want and we < 1.0 and wc < 2.0 and wm < 0.01,
      "passes match skyfield (incl. two grazing ones)",
      "%d of %d; rise/set %.2f s, culmination %.2f s, max el %.4f deg" % (found, want, we, wc, wm))

# ring alerts: the analyzer's synthetic test (Chalmette, LA; -1234 Hz
# receiver offset), now with the sniffer's view of them - stamped by a
# frame clock 37.5 s behind true time, satellite position in 4 km steps,
# and the sniffer's own satellite numbers
rx = (29.94, -89.96, 5.0)
K_OFF, T0, CLOCK_LAG = -1234.0, 1790500000.0, 37.5
ra = orbit.RingAlerts()
t = T0
while t < T0 + 300:
    for v in c.visible(t, rx, 10.0)[:3]:
        f = orbit.RA_CENTER_HZ * (1.0 - v["range_rate_kms"] / orbit.C_KMS) + K_OFF
        p = tuple(round(x / 4.0) * 4.0 for x in v["ecef"])
        ra.add(200 + v["index"], t - CLOCK_LAG, f, p)
    t += 4.32
named = ra.identify(c)
truth_ok = all(i == sid - 200 for sid, i in ra.id_to_sat.items())
check(named >= 2 and truth_ok, "ring alerts name their satellites",
      "%d named, all correct: %s" % (named, truth_ok))
check(ra.clock_offset is not None and abs(ra.clock_offset - CLOCK_LAG) < 0.5,
      "frame-clock offset recovered from IRA positions",
      "%+.2f s (true %+.1f), spread %.2f s" % (ra.clock_offset or 0, CLOCK_LAG, ra.offset_spread or 0))
fix = ra.locate(c)
km = orbit.gc_km(fix["lat"], fix["lon"], rx[0], rx[1]) if fix else 1e9
check(fix is not None and km < 3.0 and fix["rms_hz"] < 20 and abs(fix["rx_offset_hz"] - K_OFF) < 5,
      "locate finds the receiver",
      "%.3f %.3f, %.1f km off, rms %.1f Hz, rx offset %+.0f Hz"
      % (fix["lat"], fix["lon"], km, fix["rms_hz"], fix["rx_offset_hz"]) if fix else "no fix")

print("\n%d checks, %d failed" % (checks, fails))
sys.exit(1 if fails else 0)
