"""
Aircraft context for the Flights page: what the stored ACARS messages say
about where each aircraft is, or is going -- and how sure that is.

Every item is tied to the message it came from (its id, time and rule):

  reported  a position the aircraft reported (ADS-C, or its own text), as the
            receiver placed it ("acpos" archive events -> ac_pos). Real.
  heard     an aircraft-sent message: the station heard the aircraft itself,
            so it was within radio range (HEARD_KM; 59 of 60 aircraft checked
            against VDL2 were inside 300 km). A ring, never a point.
  plan      an H1 FPN flight-plan uplink: origin, destination and route.
            Checked against VDL2: 22 of 25 agreed.
  plan_notice  "FLIGHT PLAN ... XXXX-YYYY ... AVAILABLE": origin and
            destination only, and sometimes for the next leg.
  atis      "XXXX ARR ATIS" (destination) or "XXXX DEP ATIS" (origin).
  takeoff   takeoff data for one airport: at that airport, departing soon.

Ground-sent coordinates and fix names are never positions: they are route
points, weather and the like. PWI (predicted winds) is not used: its fixes
lie ahead of the aircraft, and FIX,DDDSSS is wind, not bearing/distance.

An estimate walks an aircraft along its plan's route from a departure time,
with a radius that grows with every source of doubt. The Flights page shows
it only as a circle, and only while ESTIMATES_ENABLED: the offline check
(tools/check_estimates.py) must find the true VDL2 position inside the
circle for at least 80% of estimates first.
"""
import csv
import math
import os
import re
import sqlite3
import time

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "data")
HEARD_KM = 300.0
NOT_AIRPORTS = {"KUSA"}          # FANS facility code in AFN logons (label A0)

# Set from tools/check_estimates.py's result; see that file and the README.
ESTIMATES_ENABLED = False

R_KM = 6371.0
KT_KMH = 1.852


# ---------------------------------------------------------------- geometry
def km(a, b):
    la1, lo1, la2, lo2 = map(math.radians, (a[0], a[1], b[0], b[1]))
    h = (math.sin((la2 - la1) / 2) ** 2
         + math.cos(la1) * math.cos(la2) * math.sin((lo2 - lo1) / 2) ** 2)
    return 2 * R_KM * math.asin(min(1.0, math.sqrt(h)))


def bearing(a, b):
    la1, lo1, la2, lo2 = map(math.radians, (a[0], a[1], b[0], b[1]))
    return math.atan2(math.sin(lo2 - lo1) * math.cos(la2),
                      math.cos(la1) * math.sin(la2)
                      - math.sin(la1) * math.cos(la2) * math.cos(lo2 - lo1))


def project(a, brg, dist_km):
    """The point dist_km from a along initial bearing brg (radians)."""
    la1, lo1 = math.radians(a[0]), math.radians(a[1])
    d = dist_km / R_KM
    la2 = math.asin(math.sin(la1) * math.cos(d)
                    + math.cos(la1) * math.sin(d) * math.cos(brg))
    lo2 = lo1 + math.atan2(math.sin(brg) * math.sin(d) * math.cos(la1),
                           math.cos(d) - math.sin(la1) * math.sin(la2))
    return (math.degrees(la2), (math.degrees(lo2) + 540) % 360 - 180)


def route_length(pts):
    return sum(km(pts[i], pts[i + 1]) for i in range(len(pts) - 1))


def along(pts, dist_km):
    """The point dist_km along the polyline pts (clamped to its ends)."""
    if dist_km <= 0:
        return pts[0]
    for i in range(len(pts) - 1):
        seg = km(pts[i], pts[i + 1])
        if dist_km <= seg:
            return project(pts[i], bearing(pts[i], pts[i + 1]), dist_km)
        dist_km -= seg
    return pts[-1]


# ---------------------------------------------------------------- reference
class Reference:
    """Airports, fixes and navaids, and aircraft types if available."""

    def __init__(self, data_dir=DATA, waypoints=None, aircraft_csv=None):
        self.airports = {}
        with open(os.path.join(data_dir, "airports.csv"), newline="", encoding="utf-8") as f:
            for r in csv.DictReader(f):
                self.airports[r["code"]] = (float(r["lat"]), float(r["lon"]), r["name"])
        self.fixes = {}
        paths = [os.path.join(data_dir, "navaids.csv")]
        if waypoints is None:
            waypoints = os.path.join(HERE, "..", "data", "waypoints.csv")
        if waypoints and os.path.exists(waypoints):
            paths.append(waypoints)
        for p in paths:
            with open(p) as f:
                for line in f:
                    q = line.strip().split(",")
                    if len(q) < 3 or q[0] == "id":
                        continue
                    try:
                        la, lo = float(q[1]), float(q[2])
                    except ValueError:
                        continue
                    if -90 <= la <= 90 and -180 <= lo <= 180 and (la, lo) != (0.0, 0.0):
                        self.fixes.setdefault(q[0], []).append((la, lo))
        self.types = {}
        if aircraft_csv is None:
            aircraft_csv = os.path.expanduser("~/.iridium-sniffer/aircraft.csv")
        if aircraft_csv and os.path.exists(aircraft_csv):
            with open(aircraft_csv, encoding="utf-8", errors="replace") as f:
                for line in f:
                    p = line.split(";")
                    if len(p) > 2 and p[1] and p[2]:
                        self.types[norm_reg(p[1])] = p[2].strip().upper()

    def airport(self, code):
        if code in NOT_AIRPORTS:
            return None
        a = self.airports.get(code)
        return (a[0], a[1]) if a else None

    def airport_name(self, code):
        a = self.airports.get(code)
        return a[2] if a else ""

    def fix(self, ident, near):
        """The candidate nearest `near`; None if unknown or implausibly far
        (a same-named fix on another continent)."""
        c = self.fixes.get(ident)
        if not c:
            return None
        best = min(c, key=lambda q: km(q, near))
        return best if km(best, near) < 2500 else None


def norm_reg(r):
    return re.sub(r"[^A-Z0-9]", "", (r or "").upper())


# ---------------------------------------------------------------- rules
AP = r"([A-Z]{4})"
RE_FPN_FIELD = re.compile(r":([A-Z]{1,3}):")
RE_PAIR = re.compile(r"\b" + AP + r" ?- ?" + AP + r"\b")
RE_ATIS = re.compile(r"\b" + AP + r" (ARR|DEP) ATIS\b")
RE_TO = re.compile(r"(?m)^\s*" + AP + r"\s+(\d{2}[LRC]?)\b")
RE_COORD = re.compile(r"^([NS])(\d{2})(\d{2})(\d)([EW])(\d{3})(\d{2})(\d)$")


def route_coord(tok):
    """N27530W085155 -> 27 deg 53.0', -85 deg 15.5' (an FPN route point)."""
    m = RE_COORD.match(tok)
    if not m:
        return None
    la = int(m[2]) + (int(m[3]) + int(m[4]) / 10) / 60
    lo = int(m[6]) + (int(m[7]) + int(m[8]) / 10) / 60
    if la > 90 or lo > 180:
        return None
    return (-la if m[1] == "S" else la, -lo if m[5] == "W" else lo)


def fpn_fields(t):
    """An FPN uplink's :TAG:value fields: DA origin, AA destination (its
    value may run on into the route after '..'), D departure procedure,
    F route, A arrival procedure, CA cruise level, DT planned departure
    HHMM (UTC, then the message's 4-character check value), AL alternate."""
    i = t.find("FPN/")
    if i < 0:
        return None
    parts = RE_FPN_FIELD.split(t[i:])
    f = {}
    for k in range(1, len(parts) - 1, 2):
        f.setdefault(parts[k], parts[k + 1])
    return f


def infer(lbl, txt, direction, ref):
    """Context items from one message (no time or registration yet)."""
    if direction == "UL":
        return [{"kind": "heard"}]
    t = txt or ""
    f = fpn_fields(t) if "FPN/" in t else None
    if f and "DA" in f and "AA" in f:
        oc, dc = f["DA"][:4], f["AA"][:4]
        o, d = ref.airport(oc), ref.airport(dc)
        if not (o and d):
            return []
        route_txt = f["AA"][4:] + "." + f.get("F", "")
        pts, near, names = [o], o, []
        for tok in re.split(r"[.,]", route_txt):
            q = route_coord(tok)
            if q is None and re.fullmatch(r"[A-Z]{2,5}", tok) and tok not in (oc, dc):
                q = ref.fix(tok, near)
                if q:
                    names.append(tok)
            if q and km(q, near) > 1:
                pts.append(q)
                near = q
        pts.append(d)
        it = {"kind": "plan", "origin": oc, "dest": dc, "route": pts, "via": names[:12]}
        m = re.match(r"(\d{2})(\d{2})", f.get("DT", ""))
        if m and int(m[1]) < 24 and int(m[2]) < 60:
            it["dep_hhmm"] = m[1] + m[2]
        m = re.match(r"(\d{2,3})", f.get("CA", ""))
        if m:
            it["cruise_fl"] = int(m[1])
        return [it]
    if "FLIGHT PLAN" in t and "AVAILABLE" in t and "MOST RECENT" not in t:
        pairs = {(a, b) for a, b in RE_PAIR.findall(t)
                 if a != b and ref.airport(a) and ref.airport(b)}
        if len(pairs) == 1:
            a, b = pairs.pop()
            return [{"kind": "plan_notice", "origin": a, "dest": b,
                     "route": [ref.airport(a), ref.airport(b)]}]
        return []
    out = [{"kind": "atis", "airport": c, "role": "dest" if w == "ARR" else "origin"}
           for c, w in RE_ATIS.findall(t) if ref.airport(c)]
    if out:
        return out[:1]
    if "T/O WGTS" in t or "TOGW" in t:
        cands = {a for a, _ in RE_TO.findall(t) if ref.airport(a)}
        if len(cands) == 1:
            return [{"kind": "takeoff", "airport": cands.pop()}]
    return []


# ---------------------------------------------------------------- estimates
# Cruise ground speed (kt) by ICAO type designator; airliners otherwise.
TURBOPROP = ("C208", "PC12", "PC24", "BE20", "BE30", "B350", "BE9L", "AT4", "AT7",
             "DH8", "SF34", "E120", "JS41", "C441", "P180", "TBM", "KODI")
BIZJET = ("C25", "C50", "C51", "C52", "C55", "C56", "C68", "C70", "C75", "CL30",
          "CL35", "CL60", "E50P", "E55P", "E545", "E550", "F2TH", "F900", "FA7X",
          "FA8X", "GLF", "GL5T", "GL6T", "GL7T", "GLEX", "H25", "LJ", "PRM1",
          "HDJT", "G150", "G280", "SF50", "C650", "C750")


def cruise_kt(icao_type):
    t = icao_type or ""
    if t.startswith(TURBOPROP):
        return 280.0
    if t.startswith(BIZJET):
        return 420.0
    return 450.0


CLIMB_MIN = 15.0       # first minutes at CLIMB_KT
CLIMB_KT = 260.0
TAXI_MIN = 10.0        # takeoff data -> wheels up


def estimate(route, t_dep, dep_doubt_min, now, icao_type=None):
    """Where a plan puts the aircraft at `now`, and how sure: a dict with
    lat, lon, radius_km and status (en_route | not_departed | arrived), or
    None for a route too short to matter.

    The radius adds every doubt we have: the departure time (dep_doubt_min
    either way, at cruise speed), 10% of the distance flown for speed and
    winds, and 20 km for the route's own resolution."""
    total = route_length(route)
    if total < 50:
        return None
    v = cruise_kt(icao_type) * KT_KMH / 60.0          # km per minute
    vc = CLIMB_KT * KT_KMH / 60.0
    m = (now - t_dep) / 60.0                           # minutes since departure
    if m < -dep_doubt_min:
        return {"lat": route[0][0], "lon": route[0][1], "radius_km": 20.0,
                "status": "not_departed", "flown_km": 0.0, "total_km": round(total)}
    mm = max(0.0, m)
    flown = vc * min(mm, CLIMB_MIN) + v * max(0.0, mm - CLIMB_MIN)
    radius = 20.0 + v * dep_doubt_min + 0.10 * flown
    if flown >= total:
        return {"lat": route[-1][0], "lon": route[-1][1], "radius_km": 20.0,
                "status": "arrived", "flown_km": round(total), "total_km": round(total)}
    p = along(route, flown)
    return {"lat": p[0], "lon": p[1], "radius_km": round(radius),
            "status": "en_route", "flown_km": round(flown), "total_km": round(total)}


def planned_departure(plan):
    """The plan's own DT field as a time: HH:MM UTC on whichever day puts it
    nearest the message (a plan for 00:10 sent at 23:50 is for tomorrow)."""
    hh = plan.get("dep_hhmm")
    if not hh:
        return None
    day = int(plan["t"] // 86400) * 86400
    cands = [day + d * 86400 + int(hh[:2]) * 3600 + int(hh[2:]) * 60 for d in (-1, 0, 1)]
    best = min(cands, key=lambda c: abs(c - plan["t"]))
    return best if abs(best - plan["t"]) <= 12 * 3600 else None


def departure(plan, items):
    """Departure time and its doubt (minutes) for a plan: from takeoff data
    at the plan's origin if we have it, then the plan's own planned
    departure time, else the plan message itself (uplinked before
    departure, often well before)."""
    best = None
    for it in items:
        if (it["kind"] == "takeoff" and it["airport"] == plan["origin"]
                and abs(it["t"] - plan["t"]) < 4 * 3600):
            if best is None or abs(it["t"] - plan["t"]) < abs(best["t"] - plan["t"]):
                best = it
    if best:
        return best["t"] + TAXI_MIN * 60, 10.0
    p = planned_departure(plan)
    if p is not None:
        return p, 15.0
    return plan["t"] + 20 * 60, 30.0


# ---------------------------------------------------------------- the API
def build_flights(db_path, ref, hours=6, now=None, station=None,
                  estimates=None):
    """Per aircraft heard in the last `hours`: its newest item of each kind,
    an estimate if its plan allows one, and the station position."""
    if estimates is None:
        estimates = ESTIMATES_ENABLED
    now = time.time() if now is None else now
    t0 = now - hours * 3600
    con = sqlite3.connect("file:%s?mode=ro" % db_path, uri=True)
    try:
        rows = con.execute(
            "SELECT id, t, reg, flt, dir, lbl, txt FROM messages "
            "WHERE t >= ? AND t <= ? AND reg != '' ORDER BY t", (t0, now)).fetchall()
        try:
            pos = con.execute(
                "SELECT t, reg, lat, lon, alt, src FROM ac_pos "
                "WHERE t >= ? AND t <= ? ORDER BY t", (t0, now)).fetchall()
        except sqlite3.OperationalError:          # rollup older than ac_pos
            pos = []
    finally:
        con.close()

    ac = {}

    def entry(key, raw):
        """One aircraft per normalized registration (key, for matching); it
        is shown as its messages spelled it -- C-FPCA, not CFPCA -- and the
        page's history and message links need that stored form too."""
        shown = (raw or "").strip().lstrip(".") or key
        e = ac.get(key)
        if e is None:
            e = ac[key] = {"reg": shown, "flight": "", "last": 0, "msgs": 0,
                           "items": [], "type": ref.types.get(key, "")}
        elif "-" in shown and "-" not in e["reg"]:
            e["reg"] = shown
        return e

    for mid, t, reg, flt, d, lbl, txt in rows:
        r = norm_reg(reg)
        if not r:
            continue
        e = entry(r, reg)
        e["msgs"] += 1
        e["last"] = max(e["last"], t)
        if flt:
            e["flight"] = flt
        for it in infer(lbl, txt, d, ref):
            it.update(t=t, id=mid, lbl=lbl)
            e["items"].append(it)
    for t, reg, la, lo, alt, src in pos:
        r = norm_reg(reg)
        if r and la is not None and lo is not None:
            e = entry(r, reg)
            e["last"] = max(e["last"], t)
            e["items"].append({"kind": "reported", "t": t, "lat": la, "lon": lo,
                               "alt": alt, "src": src})

    out = []
    for e in ac.values():
        newest = {}
        for it in e["items"]:
            k = it["kind"]
            if k not in newest or it["t"] >= newest[k]["t"]:
                newest[k] = it
        plan = newest.get("plan")
        est = None
        # only full FPN plans: "plan available" notices are often for the
        # next leg (4 of 15 inside their circle against VDL2)
        if plan and estimates:
            t_dep, doubt = departure(plan, e["items"])
            est = estimate(plan["route"], t_dep, doubt, now, e["type"])
            if est:
                est["from"] = plan["kind"]
                est["departure"] = int(t_dep)
                est["departure_doubt_min"] = doubt
        out.append({"reg": e["reg"], "flight": e["flight"], "type": e["type"],
                    "last": e["last"], "msgs": e["msgs"],
                    "context": {k: _public(v, ref) for k, v in newest.items()},
                    "estimate": est})
    out.sort(key=lambda x: -x["last"])
    return {"now": int(now), "hours": hours, "station": station,
            "heard_km": HEARD_KM, "estimates_enabled": bool(estimates),
            "aircraft": out}


def _public(it, ref):
    """An item as the page wants it: rounded, with airport names."""
    o = {k: v for k, v in it.items() if k not in ("route",)}
    if "route" in it:
        o["route"] = [[round(p[0], 4), round(p[1], 4)] for p in it["route"]]
    for k in ("origin", "dest", "airport"):
        if k in it:
            o[k + "_name"] = ref.airport_name(it[k])
            p = ref.airport(it[k])
            if p:
                o[k + "_pos"] = [round(p[0], 4), round(p[1], 4)]
    return o
