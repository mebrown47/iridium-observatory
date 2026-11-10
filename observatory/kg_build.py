#!/usr/bin/env python3
"""
kg_build.py - the Observatory's knowledge graph, built from stored messages.

Nodes and time-stamped edges in two tables of the Observatory database,
added to incrementally after each rollup (watermarks in `meta`):

  nodes   aircraft (tail), ground (ARINC 622 ground address, e.g. USADCXA),
          facility (CPDLC data authority, e.g. KZWY), unit (ATC unit named in
          CONTACT, e.g. JACKSONVILLE center), msg (a content message; the
          empty "_d" acknowledgements are counted on their aircraft instead)
  edges   msg -sent_to-> aircraft          every content message
          msg -from-> ground               messages carrying a ground address
          msg -authority-> facility        CPDLC "[facility] [tp4table]": the
                                           data authority naming itself
          msg -next_authority-> facility   CPDLC NEXT DATA AUTHORITY
          msg -contact-> unit              CPDLC CONTACT [unit] [frequency]
          ground -hands_off_to-> facility|unit
                                           the sending ground address passing
                                           the aircraft on, from one message
          aircraft -talks_with-> ground    derived: weight = messages

One station hears a given aircraft's CPDLC only now and then (on the home
feeder, days apart), so handoffs are read from single messages - the
sender and the authority it names - not stitched across messages.

All edges carry the time of the message they come from. Runs inside the
rollup's write transaction; stdlib only.

Copyright (c) 2026 Mike Brown
SPDX-License-Identifier: GPL-3.0-or-later
"""

import json
import re

SCHEMA = """
CREATE TABLE IF NOT EXISTS kg_node (
    id INTEGER PRIMARY KEY, kind TEXT, key TEXT, label TEXT,
    first_t INTEGER, last_t INTEGER, n INTEGER DEFAULT 0, acks INTEGER DEFAULT 0,
    UNIQUE (kind, key)
);
CREATE TABLE IF NOT EXISTS kg_edge (
    id INTEGER PRIMARY KEY, src INTEGER, dst INTEGER, kind TEXT, t INTEGER,
    weight INTEGER DEFAULT 1, props TEXT
);
CREATE INDEX IF NOT EXISTS kg_edge_src ON kg_edge (src, kind, t);
CREATE INDEX IF NOT EXISTS kg_edge_dst ON kg_edge (dst, kind, t);
CREATE UNIQUE INDEX IF NOT EXISTS kg_edge_pair ON kg_edge (src, dst, kind)
    WHERE kind IN ('talks_with');
"""

ADDR_RE = re.compile(r"^/?([A-Z0-9]{7})\.([A-Z0-9]{2,3})\.")


class Graph:
    def __init__(self, con):
        self.con = con
        self.cache = {}

    def node(self, kind, key, label, t):
        k = (kind, key)
        nid = self.cache.get(k)
        if nid is None:
            row = self.con.execute("SELECT id FROM kg_node WHERE kind=? AND key=?", k).fetchone()
            if row:
                nid = row[0]
            else:
                nid = self.con.execute(
                    "INSERT INTO kg_node(kind,key,label,first_t,last_t,n) VALUES(?,?,?,?,?,0)",
                    (kind, key, label, t, t)).lastrowid
            self.cache[k] = nid
        self.con.execute("UPDATE kg_node SET n=n+1, first_t=MIN(first_t,?), last_t=MAX(last_t,?) "
                         "WHERE id=?", (t, t, nid))
        return nid

    def edge(self, src, dst, kind, t, props=None):
        self.con.execute("INSERT INTO kg_edge(src,dst,kind,t,props) VALUES(?,?,?,?,?)",
                         (src, dst, kind, t, json.dumps(props) if props else None))

    def weighted(self, src, dst, kind, t):
        self.con.execute(
            "INSERT INTO kg_edge(src,dst,kind,t,weight) VALUES(?,?,?,?,1) "
            "ON CONFLICT(src,dst,kind) WHERE kind IN ('talks_with') DO UPDATE SET "
            "weight=weight+1, t=MAX(t,excluded.t)", (src, dst, kind, t))


def _meta(con, key, default=0):
    row = con.execute("SELECT value FROM meta WHERE key=?", (key,)).fetchone()
    return int(row[0]) if row else default


def _set_meta(con, key, value):
    con.execute("INSERT INTO meta(key,value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value",
                (key, str(value)))


def _walk(o, out):
    """Collect (key, value) pairs from a libacars decode tree."""
    if isinstance(o, dict):
        for k, v in o.items():
            if isinstance(v, (dict, list)):
                _walk(v, out)
            else:
                out.append((k, v))
    elif isinstance(o, list):
        for v in o:
            _walk(v, out)


def build(con, limit=None):
    """Add messages stored since the last build. Returns a dict of counts."""
    con.executescript(SCHEMA)
    g = Graph(con)
    got = {"messages": 0, "acks": 0, "cpdlc": 0, "edges": 0}
    n_edges0 = con.execute("SELECT COUNT(*) FROM kg_edge").fetchone()[0]

    # ---- ACARS messages
    last = _meta(con, "kg_msg_id")
    q = "SELECT id,t,reg,lbl,txt FROM messages WHERE id>? ORDER BY id"
    rows = con.execute(q + (" LIMIT %d" % limit if limit else ""), (last,)).fetchall()
    for mid, t, reg, lbl, txt in rows:
        reg = (reg or "").lstrip(".")
        if not reg:
            continue
        ac = g.node("aircraft", reg, reg, t)
        if lbl == "_d":                      # empty acknowledgement: count it, no node
            con.execute("UPDATE kg_node SET acks=acks+1 WHERE id=?", (ac,))
            got["acks"] += 1
            continue
        m = g.node("msg", "acars:%d" % mid, lbl or "", t)
        g.edge(m, ac, "sent_to", t)
        a = ADDR_RE.match(txt or "")
        if a:
            gnd = g.node("ground", a.group(1), a.group(1), t)
            g.edge(m, gnd, "from", t, {"imi": a.group(2)})
            g.weighted(ac, gnd, "talks_with", t)
        got["messages"] += 1
    if rows:
        _set_meta(con, "kg_msg_id", rows[-1][0])

    # ---- CPDLC: handoffs, contacts, replies
    last = _meta(con, "kg_cpdlc_id")
    rows = con.execute("SELECT id,t,reg,txt,dec FROM cpdlc_log WHERE id>? ORDER BY id", (last,)).fetchall()
    for cid, t, reg, txt, dec in rows:
        reg = (reg or "").lstrip(".")
        if not reg:
            continue
        try:
            tree = json.loads(dec or "{}")
        except ValueError:
            continue
        kv = []
        _walk(tree, kv)
        vals = {}
        for k, v in kv:
            vals.setdefault(k, []).append(v)
        labels = vals.get("choice_label", [])
        ac = g.node("aircraft", reg, reg, t)
        m = g.node("msg", "cpdlc:%d" % cid, (labels[0] if labels else "CPDLC")[:60], t)
        g.edge(m, ac, "sent_to", t)
        a = ADDR_RE.match(txt or "")
        gnd = None
        if a:
            gnd = g.node("ground", a.group(1), a.group(1), t)
            g.edge(m, gnd, "from", t, {"imi": a.group(2)})
        msg_id = (vals.get("msg_id") or [None])[0]
        if msg_id is not None:
            con.execute("UPDATE kg_node SET label=label||? WHERE id=?", (" #%s" % msg_id, m))
        nda = any(l.startswith("NEXT DATA AUTHORITY") for l in labels)
        self_id = any(l.startswith("[icaofacilitydesignation]") for l in labels)
        for fac in vals.get("icao_facility_designation", []):
            if nda:
                f = g.node("facility", fac, fac, t)
                g.edge(m, f, "next_authority", t)
                if gnd is not None:
                    g.edge(gnd, f, "hands_off_to", t, {"aircraft": reg})
            elif self_id:
                f = g.node("facility", fac, fac, t)
                g.edge(m, f, "authority", t)
        names = vals.get("icao_facility_name", [])
        funcs = vals.get("icao_facility_function", [])
        if any(l.startswith("CONTACT") for l in labels):
            for i, name in enumerate(names):
                fn = funcs[i] if i < len(funcs) else ""
                u = g.node("unit", "%s/%s" % (name, fn), ("%s %s" % (name, fn)).strip(), t)
                g.edge(m, u, "contact", t)
                if gnd is not None:
                    g.edge(gnd, u, "hands_off_to", t, {"aircraft": reg})
        # No "answers" edges: a ground-sent CPDLC message's msg_ref names the
        # aircraft's message, which a station hearing only the downlink from
        # the satellite never receives. Matching it against the uplinks we
        # do hear would invent links.
        got["cpdlc"] += 1
    if rows:
        _set_meta(con, "kg_cpdlc_id", rows[-1][0])
    got["edges"] = con.execute("SELECT COUNT(*) FROM kg_edge").fetchone()[0] - n_edges0
    return got
