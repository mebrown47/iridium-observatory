#!/usr/bin/env python3
"""The Search tab's queries (trends_server.build_search / search_csv) on a
small hand-made database: each filter, heartbeats hidden by default, LIKE
wildcards taken literally, paging, the date range including its last day,
CPDLC summaries joined in, and the CSV export.

  python3 observatory/tests/test_search.py
"""
import calendar
import os
import sqlite3
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import rollup  # noqa: E402
import trends_server as ts  # noqa: E402

fails = checks = 0


def check(ok, what, detail=""):
    global fails, checks
    checks += 1
    fails += not ok
    print("  %s: %s%s" % ("PASS" if ok else "FAIL", what, "  (%s)" % detail if detail else ""))


def day(s, hh=12):
    return calendar.timegm((int(s[:4]), int(s[5:7]), int(s[8:10]), hh, 0, 0))


tmp = tempfile.mkdtemp()
db = os.path.join(tmp, "observatory.db")
con = sqlite3.connect(db)
rollup.init_db(con)
msgs = [  # t, reg, flt, dir, lbl, freq, txt, has_cpdlc, has_adsc
    (day("2026-11-10"), "N100AA", "AA0100", "DL", "_d", 0, "", 0, 0),
    (day("2026-11-10", 13), "N100AA", "AA0100", "DL", "H1", 0, "LOADSHEET FINAL", 0, 0),
    (day("2026-11-11"), "N200BB", "", "UL", "Q0", 0, "", 0, 0),
    (day("2026-11-11", 23), "N200BB", "", "DL", "AA", 0, "/AT1.N200BB", 1, 0),
    (day("2026-11-12"), "N300CC", "", "DL", "H1", 0, "FUEL 100% OK", 0, 0),
    (day("2026-11-12", 1), "N300CC", "", "DL", "H1", 0, "FUEL 1000 OK", 0, 0),
]
con.executemany("INSERT INTO messages(t,reg,flt,dir,lbl,freq,txt,has_cpdlc,has_adsc) "
                "VALUES(?,?,?,?,?,?,?,?,?)", msgs)
con.execute("INSERT INTO cpdlc_log(t,reg,flt,dir,txt,summary,dec) VALUES(?,?,?,?,?,?,?)",
            (day("2026-11-11", 23), "N200BB", "", "DL", "", "CLIMB TO FL350", "{}"))
con.commit()
con.close()


def S(**kw):
    return ts.build_search(db, {k: [str(v)] for k, v in kw.items()})


r = S()
check(r["total"] == 5, "heartbeats hidden by default", r["total"])
check(S(hb=1)["total"] == 6, "hb=1 includes them")
check([x["t"] for x in r["rows"]] == sorted((x["t"] for x in r["rows"]), reverse=True), "newest first")
check(S(sort="oldest")["rows"][0]["reg"] == "N100AA", "sort=oldest")
check(S(ac="n200")["total"] == 2, "aircraft: partial, case-insensitive")
check(S(ac="aa0100")["total"] == 1, "aircraft also matches the flight number")
check(S(ac=".N300CC")["total"] == 2, "a leading dot is ignored")
check(S(q="loadsheet")["total"] == 1, "text contains, case-insensitive")
check(S(q="100%")["total"] == 1, "% is literal, not a wildcard")
check(S(q="1_00")["total"] == 0, "_ is literal, not a wildcard")
check(S(dir="UL")["total"] == 1 and S(dir="ul")["total"] == 1, "direction")
check(S(dir="XX")["total"] == 5 and S(dir="XX")["filters"]["dir"] == "", "a bad direction is ignored")
check(S(lbl="H1")["total"] == 3, "label")
c = S(kind="cpdlc")
check(c["total"] == 1 and c["rows"][0]["summary"] == "CLIMB TO FL350", "CPDLC filter + summary joined")
check(S(**{"from": "2026-11-11", "to": "2026-11-11"})["total"] == 2, "date range includes the To day (23:00)")
check(S(**{"from": "garbage"})["total"] == 5, "a bad date is ignored")
p1, p2 = S(limit=2), S(limit=2, offset=2)
check(len(p1["rows"]) == 2 and len(p2["rows"]) == 2 and p1["rows"][0] != p2["rows"][0], "paging")
check(S(limit=99999)["limit"] == ts.SEARCH_LIMIT_MAX, "limit capped")
check(dict(r["labels"])["H1"] == 3 and dict(r["aircraft"])["N300CC"] == 2, "label and aircraft breakdowns")
check([d for d, _ in r["days"]] == ["2026-11-10", "2026-11-11", "2026-11-12"], "matches per day")
csv = ts.search_csv(db, {"dir": ["UL"]}).splitlines()
check(csv[0].startswith("time_utc,reg") and len(csv) == 2 and "N200BB" in csv[1], "CSV export")
check("error" in ts.build_search(os.path.join(tmp, "missing.db"), {}), "missing database reported")

print("%d checks, %d failed" % (checks, fails))
sys.exit(1 if fails else 0)
