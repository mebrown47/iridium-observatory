#!/usr/bin/env python3
"""The Flights page's context rules (observatory/context.py): each rule on
real message shapes, what must NOT become a place (an AFN facility code, a
PWI wind group, ground-sent coordinates), the estimate maths on a straight
route, the planned-departure day roll, and build_flights on a small
database with a reported position.

  python3 observatory/tests/test_context.py
"""
import os
import sqlite3
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import context as cx  # noqa: E402
import rollup  # noqa: E402

fails = checks = 0


def check(ok, what, detail=""):
    global fails, checks
    checks += 1
    fails += not ok
    print("  %s: %s%s" % ("PASS" if ok else "FAIL", what, "  (%s)" % detail if detail else ""))


ref = cx.Reference(aircraft_csv=False)


def kinds(lbl, txt, d="DL"):
    return [i["kind"] for i in cx.infer(lbl, txt, d, ref)]


# ---- flight plans
fpn = ("FPN/RN10003/RI:DA:KPDK:AA:KTEB:F:EAONE,N33524W083480..SPA,N35020W081556.."
       "LYH,N37152W079141:A:JAIKE4.JAIKE:CA:450:DT:2300F6A5")
p = cx.infer("H1", fpn, "DL", ref)
check(len(p) == 1 and p[0]["kind"] == "plan", "FPN uplink gives a plan")
if p:
    p = p[0]
    check(p["origin"] == "KPDK" and p["dest"] == "KTEB", "plan origin and destination", "%s-%s" % (p["origin"], p["dest"]))
    check(p.get("dep_hhmm") == "2300", "planned departure from :DT:", p.get("dep_hhmm"))
    check(p.get("cruise_fl") == 450, "cruise level from :CA:", p.get("cruise_fl"))
    check(len(p["route"]) >= 5, "route points resolved (coordinates and fixes)", len(p["route"]))
    check("SPA" in p.get("via", []), "a 3-letter navaid (SPA) resolves", p.get("via"))
p2 = cx.infer("H1", "FPN/RP:DA:KCQF:AA:KMSY..SOBLE,N30182W0884542160", "DL", ref)
check(p2 and p2[0]["kind"] == "plan" and p2[0]["dest"] == "KMSY", "route run on after AA still parses")
check(kinds("H1", "FPN/RP:DA:ZZZZ:AA:KMSY") == [], "unknown airport: no plan")

# ---- plan notices
note = ("DFS|N691QS|19-OCT-26 06:08Z\nFROM: GDFS\nTO: N691QS\nFLIGHT PLAN #E3805,\n"
        "KBNA-KJWN IS NOW\nAVAILABLE FOR RECALL")
n = cx.infer("21", note, "DL", ref)
check(n and n[0]["kind"] == "plan_notice" and (n[0]["origin"], n[0]["dest"]) == ("KBNA", "KJWN"),
      "flight plan notice gives origin and destination")
check(kinds("41", "NOT FOUND IN FDF DATABASE. MOST RECENT FLIGHT PLANS STORED ARE\nF4650 KHRT-KMCF\n"
               "F4643 KMCF-KVQQ\nFLIGHT PLAN AVAILABLE") == [], "a list of stored plans is not a plan")

# ---- ATIS and takeoff data
a = cx.infer("A9", "/HDQGLXS.TI2/KDFW ARR ATIS Q\nDFW ARR INFO Q 1253Z.", "DL", ref)
check(a and a[0]["kind"] == "atis" and a[0]["role"] == "dest" and a[0]["airport"] == "KDFW", "ARR ATIS: destination")
a = cx.infer("A9", "/HDQGLXS.TI2/KATL DEP ATIS B", "DL", ref)
check(a and a[0]["role"] == "origin", "DEP ATIS: origin")
to = cx.infer("35", " 142 BAG\n \nKATL 08R         SPECIAL\nT/O WGTS\nTOGW 126.2", "DL", ref)
check(to and to[0]["kind"] == "takeoff" and to[0]["airport"] == "KATL", "takeoff data: departing KATL")

# ---- what must not become a place
check(kinds("A0", "/USADCXA.AFN/FMHN736K,..N736K,A9E236,103609/FAK0,KUSA/FARADS,1") == [],
      "KUSA in an AFN logon is a facility code, not an airport")
check(kinds("H1", "PWI/WD430,GONDR,149009,430M62.REECO,093003") == [],
      "a PWI wind group is not a route")
check(kinds("H1", "POSN46247W101478,KSEA,190504,350,HASTE,203912", "DL") == [],
      "ground-sent coordinates are not a position")
check(kinds("20", "anything", "UL") == ["heard"], "an aircraft-sent message: heard directly")

# ---- estimates
atl, jfk = ref.airport("KATL"), ref.airport("KJFK")
route = [atl, jfk]
L = cx.route_length(route)
t0 = 1_000_000_000
e = cx.estimate(route, t0, 15, t0 - 3600)
check(e["status"] == "not_departed", "before departure: not departed", e["status"])
e1 = cx.estimate(route, t0, 15, t0 + 30 * 60)
e2 = cx.estimate(route, t0, 15, t0 + 60 * 60)
check(e1["status"] == e2["status"] == "en_route", "en route after departure")
d1, d2 = cx.km(atl, (e1["lat"], e1["lon"])), cx.km(atl, (e2["lat"], e2["lon"]))
check(0 < d1 < d2 < L, "estimate moves along the route", "%.0f then %.0f of %.0f km" % (d1, d2, L))
check(e2["radius_km"] > e1["radius_km"], "radius grows with time flown", "%s -> %s" % (e1["radius_km"], e2["radius_km"]))
check(cx.estimate(route, t0, 15, t0 + 6 * 3600)["status"] == "arrived", "long after departure: arrived")
check(cx.estimate([atl, atl], t0, 15, t0 + 600) is None, "no estimate for a route too short to matter")
d_late = {"t": 86400 * 100 + 23 * 3600 + 50 * 60, "dep_hhmm": "0010"}
check(cx.planned_departure(d_late) == 86400 * 101 + 10 * 60, "planned departure rolls to the next day")

# ---- build_flights on a small database
tmp = tempfile.mkdtemp()
db = os.path.join(tmp, "obs.db")
con = sqlite3.connect(db)
rollup.init_db(con)
now = 2_000_000_000
# the receiver's "acpos" archive event, through the rollup into ac_pos
agg = rollup.Agg()
agg.event({"e": "acpos", "t": (now - 300) * 1000, "reg": ".N918LB", "lat": 31.4, "lon": -86.4, "src": "text"})
agg.flush(con)
row = con.execute("SELECT t, reg, lat, lon, src FROM ac_pos").fetchone()
check(row == (now - 300, "N918LB", 31.4, -86.4, "text"), "rollup stores acpos events", repr(row))
con.execute("INSERT INTO messages(t,reg,flt,dir,lbl,txt) VALUES(?,?,?,?,?,?)", (now - 600, "N745QS", "", "DL", "H1", fpn))
con.execute("INSERT INTO messages(t,reg,flt,dir,lbl,txt) VALUES(?,?,?,?,?,?)", (now - 300, ".N918LB", "AB12", "UL", "20", "x"))
con.execute("INSERT INTO messages(t,reg,flt,dir,lbl,txt) VALUES(?,?,?,?,?,?)", (now - 90000, "N1OLD", "", "DL", "H1", fpn))
con.commit()
con.close()
f = cx.build_flights(db, ref, hours=6, now=now, station=[31.4, -86.1])
regs = {a["reg"]: a for a in f["aircraft"]}
check(set(regs) == {"N745QS", "N918LB"}, "aircraft in the window only", sorted(regs))
check("plan" in regs.get("N745QS", {}).get("context", {}), "plan in the aircraft's context")
nb = regs.get("N918LB", {}).get("context", {})
check("reported" in nb and "heard" in nb, "reported position and heard-directly together", sorted(nb))
check(not f["estimates_enabled"] and regs["N745QS"]["estimate"] is None, "estimates off by default")
fe = cx.build_flights(db, ref, hours=6, now=now, estimates=True)
check(any(a["estimate"] for a in fe["aircraft"]), "estimates when enabled")

print("\n%d checks, %d failed" % (checks, fails))
sys.exit(1 if fails else 0)
