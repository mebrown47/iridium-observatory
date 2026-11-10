#!/usr/bin/env python3
"""kg_build.py on a small hand-made message set: nodes, edges, handoffs read
from single CPDLC messages, acknowledgements counted not stored, and an
incremental second build that adds nothing.

  python3 observatory/tests/test_kg.py
"""
import json
import os
import sqlite3
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import kg_build  # noqa: E402

fails = checks = 0


def check(ok, what, detail=""):
    global fails, checks
    checks += 1
    fails += not ok
    print("  %s: %s%s" % ("PASS" if ok else "FAIL", what, "  (%s)" % detail if detail else ""))


con = sqlite3.connect(":memory:")
con.executescript("""
CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT);
CREATE TABLE messages(id INTEGER PRIMARY KEY, t INTEGER, reg TEXT, flt TEXT, dir TEXT, lbl TEXT,
                      freq REAL, txt TEXT, has_cpdlc INTEGER, has_adsc INTEGER);
CREATE TABLE cpdlc_log(id INTEGER PRIMARY KEY, t INTEGER, reg TEXT, flt TEXT, dir TEXT,
                       txt TEXT, summary TEXT, dec TEXT);
""")
msgs = [(1, 100, ".N100AA", "", "DL", "_d", 0, "", 0, 0),
        (2, 110, ".N100AA", "", "DL", "_d", 0, "", 0, 0),
        (3, 120, ".N100AA", "", "DL", "H1", 0, "POS/ABC", 0, 0),
        (4, 130, ".N200BB", "", "DL", "AA", 0, "/NYCODYA.AT1.N200BB0011", 1, 0)]
con.executemany("INSERT INTO messages VALUES(?,?,?,?,?,?,?,?,?,?)", msgs)
nda = {"atc_uplink_msg": {"header": {"msg_id": 7}, "atc_uplink_msg_element_id": {
    "choice_label": "NEXT DATA AUTHORITY [icaofacilitydesignation]",
    "data": {"icao_facility_designation": "KUSA"}}}}
contact = {"atc_uplink_msg": {"header": {"msg_id": 8}, "atc_uplink_msg_element_id": {
    "choice_label": "CONTACT [icaounitname] [frequency]",
    "data": {"icao_unit_name_freq": {"icao_unit_name": {"icao_facility_id": {
        "data": {"icao_facility_name": "JACKSONVILLE"}}, "icao_facility_function": "center"}}}}}}
selfid = {"atc_uplink_msg": {"header": {"msg_id": 9}, "atc_uplink_msg_element_id": {
    "choice_label": "[icaofacilitydesignation] [tp4table]",
    "data": {"icao_facility_designation": "KZWY"}}}}
con.executemany("INSERT INTO cpdlc_log VALUES(?,?,?,?,?,?,?,?)", [
    (1, 200, ".N200BB", "", "DL", "/NYCODYA.AT1.N200BB", "", json.dumps(nda)),
    (2, 210, ".N200BB", "", "DL", "/NYCODYA.AT1.N200BB", "", json.dumps(contact)),
    (3, 220, ".N300CC", "", "DL", "USADCXA.CR1.N300CC", "", json.dumps(selfid))])

got = kg_build.build(con)
kinds = dict(con.execute("SELECT kind, COUNT(*) FROM kg_node GROUP BY kind").fetchall())
edges = dict(con.execute("SELECT kind, COUNT(*) FROM kg_edge GROUP BY kind").fetchall())
check(got["acks"] == 2 and kinds.get("msg") == 5,
      "acknowledgements are counted, content messages become nodes", "%s %s" % (got, kinds))
check(con.execute("SELECT acks FROM kg_node WHERE kind='aircraft' AND key='N100AA'").fetchone()[0] == 2,
      "acknowledgements are counted on their aircraft")
ho = con.execute("SELECT a.key, b.label FROM kg_edge e JOIN kg_node a ON a.id=e.src JOIN kg_node b ON b.id=e.dst "
                 "WHERE e.kind='hands_off_to' ORDER BY e.t").fetchall()
check(ho == [("NYCODYA", "KUSA"), ("NYCODYA", "JACKSONVILLE center")],
      "handoffs read from single messages: sender to named authority or unit", str(ho))
check(edges.get("authority") == 1 and edges.get("talks_with") == 1,
      "a self-identifying authority and a talks-with link", str(edges))
again = kg_build.build(con)
check(again["messages"] == 0 and again["cpdlc"] == 0 and again["edges"] == 0,
      "a second build adds nothing", str(again))

print("\n%d checks, %d failed" % (checks, fails))
sys.exit(1 if fails else 0)
