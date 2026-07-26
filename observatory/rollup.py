#!/usr/bin/env python3
"""
Iridium Observatory — archive rollup (M2)

Folds the append-only JSONL archive written by iridium-sniffer's --archive
flag into hourly aggregates in a SQLite database, so the Trends dashboard
can query weeks of history cheaply.

Design:
  * Incremental. Per-file byte offsets are tracked in `ingest_state`, so
    each run only reads bytes appended since last time. Re-running is safe
    and cheap; the current (growing) day file is picked up a bit at a time.
  * Only complete lines (terminated by '\n') are consumed. A partial
    trailing line is left for the next run.
  * Aggregates are UPSERT-added, so a line counted once always lands in
    exactly one hour bucket regardless of how the reads were chunked.
  * Deleting the DB triggers a clean full re-ingest (ingest_state lives in
    the same DB).

Time units — the one invariant worth stating loudly:
  * The JSONL archive carries `t` in **milliseconds** (archive.c).
  * Every timestamp column in the DB is **epoch seconds** — `hour` buckets
    and the message-level `t` alike. Convert once, at the boundary, via
    sec_of()/hour_of(); never store a raw ev["t"].
  Schema v2 stored message-level `t` in ms while `hour` was seconds, so any
  query joining the two silently produced garbage (a ms stamp read as
  seconds lands in year 58505). Seconds is the resolution the dashboards
  render anyway, and the archive keeps the ms if it is ever wanted back;
  `messages.id` still orders messages within a second.

Usage:
    rollup.py [--archive DIR] [--db PATH] [--verbose]

Defaults match the feeder layout:
    --archive ./archive     --db ./observatory/observatory.db
"""

import argparse
import glob
import json
import os
import sqlite3
import sys
import time

SCHEMA_VERSION = 3

# Every frame label classify_frame_label()/archive.c can emit.
FRAME_TYPES = ["IRA", "IBC", "MSG", "IDA", "VOC", "IIP", "IU3", "ISY",
               "IU6", "IDA_UL_FAIL", "ITL", "RAW"]


def sec_of(t_ms):
    """Epoch seconds for the archive's ms stamp. The only ms->s boundary."""
    return int(t_ms) // 1000


def hour_of(t_ms):
    """UTC hour bucket (epoch seconds at the hour boundary) for a ms stamp."""
    return sec_of(t_ms) // 3600 * 3600


def init_db(con):
    con.executescript(
        """
        PRAGMA journal_mode=WAL;
        PRAGMA synchronous=NORMAL;

        CREATE TABLE IF NOT EXISTS meta (
            key TEXT PRIMARY KEY, value TEXT
        );
        CREATE TABLE IF NOT EXISTS ingest_state (
            file    TEXT PRIMARY KEY,
            offset  INTEGER NOT NULL,   -- bytes consumed
            size    INTEGER NOT NULL,   -- file size at last read
            updated INTEGER NOT NULL    -- epoch of last run
        );
        CREATE TABLE IF NOT EXISTS frame_hourly (
            hour INTEGER, type TEXT, dir TEXT, count INTEGER,
            PRIMARY KEY (hour, type, dir)
        );
        CREATE TABLE IF NOT EXISTS uw_hourly (
            hour INTEGER, dir TEXT, count INTEGER,
            PRIMARY KEY (hour, dir)
        );
        CREATE TABLE IF NOT EXISTS sat_hourly (
            hour INTEGER, sat_id INTEGER, count INTEGER,
            PRIMARY KEY (hour, sat_id)
        );
        CREATE TABLE IF NOT EXISTS msg_hourly (
            hour INTEGER, service TEXT, dir TEXT, count INTEGER,
            PRIMARY KEY (hour, service, dir)
        );

        -- Message-level tables (M4): one row per decoded message, so the
        -- aviation dashboard can browse per-tail history and the CPDLC log.
        -- Populated by the same incremental pass; rows are append-only and
        -- each archive event is ingested exactly once (byte-offset tracked).
        CREATE TABLE IF NOT EXISTS messages (
            id       INTEGER PRIMARY KEY AUTOINCREMENT,
            t        INTEGER,          -- epoch SECONDS (see module docstring)
            reg      TEXT, flt TEXT, dir TEXT, lbl TEXT,
            freq     INTEGER, txt TEXT,
            has_cpdlc INTEGER, has_adsc INTEGER
        );
        CREATE INDEX IF NOT EXISTS ix_messages_reg ON messages(reg);
        CREATE INDEX IF NOT EXISTS ix_messages_t   ON messages(t);

        CREATE TABLE IF NOT EXISTS cpdlc_log (
            id      INTEGER PRIMARY KEY AUTOINCREMENT,
            t       INTEGER, reg TEXT, flt TEXT, dir TEXT,
            txt     TEXT,               -- ACARS free text, if any
            summary TEXT,               -- readable gist from the decoded tree
            dec     TEXT                -- full cpdlc subtree JSON, for detail
        );
        CREATE INDEX IF NOT EXISTS ix_cpdlc_t ON cpdlc_log(t);

        -- ADS-C log. `kinds` lists the tag names present (contract requests
        -- vs. actual reports); `has_pos`/lat/lon/alt are populated only when
        -- the message carries real coordinates, which is what the track
        -- replay slice waits for. Everything seen so far is contract
        -- negotiation, so has_pos=1 is the signal worth watching.
        CREATE TABLE IF NOT EXISTS adsc_log (
            id      INTEGER PRIMARY KEY AUTOINCREMENT,
            t       INTEGER, reg TEXT, flt TEXT, dir TEXT,
            txt     TEXT,
            kinds   TEXT,               -- comma-joined ADS-C tag names
            summary TEXT,               -- readable gist
            has_pos INTEGER,            -- 1 iff lat+lon decoded
            lat     REAL, lon REAL, alt INTEGER,
            dec     TEXT                -- full adsc subtree JSON
        );
        CREATE INDEX IF NOT EXISTS ix_adsc_t   ON adsc_log(t);
        CREATE INDEX IF NOT EXISTS ix_adsc_pos ON adsc_log(has_pos);
        """
    )
    con.execute(
        "INSERT INTO meta(key,value) VALUES('schema',?) "
        "ON CONFLICT(key) DO UPDATE SET value=excluded.value",
        (str(SCHEMA_VERSION),),
    )
    con.commit()


def has_key(obj, name):
    """Recursively test whether a decoded-message tree contains a node
    whose key contains `name` (e.g. 'cpdlc', 'adsc'). libacars names its
    nodes after the message type, so this reliably tags CPDLC/ADS-C."""
    if isinstance(obj, dict):
        for k, v in obj.items():
            if name in k:
                return True
            if has_key(v, name):
                return True
    elif isinstance(obj, list):
        for v in obj:
            if has_key(v, name):
                return True
    return False


def find_node(obj, name):
    """Return the first sub-object whose key contains `name`, else None."""
    if isinstance(obj, dict):
        for k, v in obj.items():
            if name in k:
                return v
            r = find_node(v, name)
            if r is not None:
                return r
    elif isinstance(obj, list):
        for v in obj:
            r = find_node(v, name)
            if r is not None:
                return r
    return None


# Structural/bookkeeping keys that carry no readable clearance content.
_CPDLC_NOISE = {"err", "crc_ok", "mode", "label", "blk_id", "ack", "more",
                "reg", "imi", "msg_type", "timestamp", "dir", "msg_id"}


def cpdlc_summary(cpdlc_node, limit=12):
    """Best-effort readable gist of a decoded CPDLC message: collect the
    meaningful string/number leaves (route, level, beacon, free text, ...)
    from the decoded tree. Not a full renderer — the dashboard keeps the
    complete subtree for detail — but enough for a scannable log line."""
    leaves = []

    def walk(o):
        if len(leaves) >= limit:
            return
        if isinstance(o, dict):
            for k, v in o.items():
                # Skip whole timestamp/header subtrees — their hour/min/sec
                # leaves are bookkeeping, not clearance content.
                if isinstance(v, (dict, list)):
                    if "time" in k or "header" in k:
                        continue
                    walk(v)
                elif k in _CPDLC_NOISE:
                    continue
                elif isinstance(v, bool):
                    continue
                elif isinstance(v, str) and len(v) > 1:
                    leaves.append(f"{k}={v}")
                elif isinstance(v, (int, float)):
                    leaves.append(f"{k}={v}")
        elif isinstance(o, list):
            for v in o:
                walk(v)

    walk(cpdlc_node)
    return " · ".join(leaves[:limit])


# Readable names for the ADS-C tags libacars emits. Anything unmapped falls
# through as its raw tag name, so a new/unknown tag still shows up rather
# than silently rendering blank.
_ADSC_LABELS = {
    # Ground -> air: the contract negotiation.
    "periodic_contract_req": "periodic contract",
    "event_contract_req": "event contract",
    "demand_contract_req": "demand contract",
    "cancel_contract_req": "cancel contract",
    "cancel_all_contracts_req": "cancel all contracts",
    "cancel_emergency_mode_req": "cancel emergency",
    # Event-contract trigger groups.
    "report_wpt_changes": "wpt changes",
    "report_when_alt_out_of_range": "alt band",
    "report_when_lateral_dev_exceeds": "lateral dev",
    "report_when_vspd_out_of_range": "vspd band",
    "report_when_airspeed_change_exceeds": "airspeed change",
    # Periodic-contract groups: how often, and which extras to include.
    "report_interval": "interval",
    "earth_ref_data": "earth ref",
    "air_ref_data": "air ref",
    "meteo_data": "meteo",
    # Air -> ground: the reports. `basic_report` is the one carrying position.
    "basic_report": "position",
    "flight_id": "flight id",
    "predicted_route": "predicted route",
    "earth_ref": "earth ref",
    "air_ref": "air ref",
    "meteo": "meteo",
    "airframe_id": "airframe id",
    "intermediate_proj": "intermediate projection",
    "fixed_proj": "fixed projection",
    "ack": "ack",
    "nack": "nack",
    "noncompliance_notify": "noncompliance",
}


def adsc_tags(adsc_node):
    """The ADS-C `tags` list is a list of single-key dicts; return the list of
    (name, body) pairs in order."""
    out = []
    tags = adsc_node.get("tags") if isinstance(adsc_node, dict) else None
    if isinstance(tags, list):
        for tag in tags:
            if isinstance(tag, dict):
                out.extend(tag.items())
    return out


def adsc_position(node):
    """(lat, lon, alt) of the first node carrying real coordinates, else None.

    Requires `lat` AND `lon` as exact keys in the *same* dict. This matters:
    the event-contract group `report_when_lateral_dev_exceeds` has a key
    `lat_dev_treshold_nm` (libacars' spelling), so a substring match on 'lat'
    would happily report a 5-nautical-mile deviation threshold as a latitude.
    """
    found = []

    def walk(o):
        if found:
            return
        if isinstance(o, dict):
            lat, lon = o.get("lat"), o.get("lon")
            if isinstance(lat, (int, float)) and not isinstance(lat, bool) \
               and isinstance(lon, (int, float)) and not isinstance(lon, bool):
                alt = o.get("alt")
                found.append((
                    float(lat), float(lon),
                    int(alt) if isinstance(alt, (int, float))
                    and not isinstance(alt, bool) else None))
                return
            for v in o.values():
                walk(v)
        elif isinstance(o, list):
            for v in o:
                walk(v)

    walk(node)
    return found[0] if found else None


def adsc_summary(adsc_node, limit=12):
    """Readable gist of a decoded ADS-C message, e.g.
       'event contract #8 · alt band 34796–35204 ft · wpt changes'
    Event-contract groups are flattened to the top level so the trigger
    conditions read as a flat list rather than a nested blob."""
    parts = []

    def emit(name, body):
        if len(parts) >= limit:
            return
        bits = []
        if isinstance(body, dict):
            if isinstance(body.get("contract_num"), int):
                bits.append(f"#{body['contract_num']}")
            lat, lon = body.get("lat"), body.get("lon")
            if isinstance(lat, (int, float)) and isinstance(lon, (int, float)):
                bits.append(f"{lat:.4f},{lon:.4f}")
                if isinstance(body.get("alt"), (int, float)):
                    bits.append(f"{body['alt']} ft")
            f, c = body.get("floor_alt"), body.get("ceiling_alt")
            if f is not None and c is not None:
                bits.append(f"{f}–{c} ft")
            if body.get("lat_dev_treshold_nm") is not None:
                bits.append(f">{body['lat_dev_treshold_nm']} nm")
            if isinstance(body.get("interval_secs"), int):
                bits.append(f"{body['interval_secs']}s")
            # Periodic groups use a modulus: include this datum every Nth
            # report. Modulus 1 = every report, which is the common case.
            if isinstance(body.get("modulus"), int):
                bits.append("every report" if body["modulus"] == 1
                            else f"every {body['modulus']}th")
        label = _ADSC_LABELS.get(name, name)
        parts.append((label + " " + " ".join(bits)).strip())
        if isinstance(body, dict):
            for g in body.get("groups") or []:
                if isinstance(g, dict):
                    for gn, gb in g.items():
                        emit(gn, gb)

    for name, body in adsc_tags(adsc_node):
        emit(name, body)
    return " · ".join(parts[:limit])


class Agg:
    """In-memory accumulator; flushed to the DB as UPSERT-adds."""

    def __init__(self):
        self.frame = {}   # (hour,type,dir) -> count
        self.uw = {}      # (hour,dir) -> count
        self.sat = {}     # (hour,sat_id) -> count
        self.msg = {}     # (hour,service,dir) -> count
        self.messages = []  # per-message rows (M4)
        self.cpdlcs = []    # per-CPDLC rows (M4)
        self.adscs = []     # per-ADS-C rows (M4)
        self.lines = 0
        self.bad = 0

    def add(self, d, key, n=1):
        d[key] = d.get(key, 0) + n

    def event(self, ev):
        e = ev.get("e")
        if e == "fmix":
            h = hour_of(ev["t"])
            for d, dirname in (("dl", "DL"), ("ul", "UL")):
                for typ, n in ev.get(d, {}).items():
                    self.add(self.frame, (h, typ, dirname), n)
            uw = ev.get("uw", [0, 0])
            if uw[0]:
                self.add(self.uw, (h, "DL"), uw[0])
            if len(uw) > 1 and uw[1]:
                self.add(self.uw, (h, "UL"), uw[1])
            for sid, n in ev.get("sats", {}).items():
                self.add(self.sat, (h, int(sid)), n)
        elif e == "acars":
            h = hour_of(ev["t"])
            ts = sec_of(ev["t"])
            dirname = ev.get("dir", "DL")
            self.add(self.msg, (h, "acars", dirname))
            dec = ev.get("dec")
            is_cpdlc = is_adsc = False
            if dec is not None:
                if has_key(dec, "cpdlc"):
                    is_cpdlc = True
                    self.add(self.msg, (h, "cpdlc", dirname))
                if has_key(dec, "adsc"):
                    is_adsc = True
                    self.add(self.msg, (h, "adsc", dirname))
            reg = (ev.get("reg") or "").lstrip(".")
            flt = ev.get("flt") or ""
            txt = ev.get("txt") or ""
            self.messages.append((ts, reg, flt, dirname,
                                  ev.get("lbl") or "", ev.get("freq") or 0,
                                  txt, int(is_cpdlc), int(is_adsc)))
            if is_cpdlc:
                node = find_node(dec, "cpdlc")
                self.cpdlcs.append(
                    (ts, reg, flt, dirname, txt,
                     cpdlc_summary(node) if node is not None else "",
                     json.dumps(node, separators=(",", ":"))
                     if node is not None else ""))
            if is_adsc:
                node = find_node(dec, "adsc")
                pos = adsc_position(node) if node is not None else None
                kinds = ",".join(n for n, _ in adsc_tags(node)) \
                    if node is not None else ""
                self.adscs.append(
                    (ts, reg, flt, dirname, txt, kinds,
                     adsc_summary(node) if node is not None else "",
                     1 if pos else 0,
                     pos[0] if pos else None,
                     pos[1] if pos else None,
                     pos[2] if pos else None,
                     json.dumps(node, separators=(",", ":"))
                     if node is not None else ""))
        elif e == "pager":
            h = hour_of(ev["t"])
            # Pager frames carry no UL/DL; bucket as DL (all downlink).
            self.add(self.msg, (h, "pager", "DL"))
        # 'start' and unknown events: ignored.

    def flush(self, con):
        def upsert(table, cols, d):
            q = (f"INSERT INTO {table}({','.join(cols)},count) "
                 f"VALUES({','.join('?' * len(cols))},?) "
                 f"ON CONFLICT({','.join(cols)}) "
                 f"DO UPDATE SET count=count+excluded.count")
            con.executemany(q, [(*k, v) for k, v in d.items()])

        upsert("frame_hourly", ("hour", "type", "dir"), self.frame)
        upsert("uw_hourly", ("hour", "dir"), self.uw)
        upsert("sat_hourly", ("hour", "sat_id"), self.sat)
        upsert("msg_hourly", ("hour", "service", "dir"), self.msg)

        con.executemany(
            "INSERT INTO messages(t,reg,flt,dir,lbl,freq,txt,has_cpdlc,"
            "has_adsc) VALUES(?,?,?,?,?,?,?,?,?)", self.messages)
        con.executemany(
            "INSERT INTO cpdlc_log(t,reg,flt,dir,txt,summary,dec) "
            "VALUES(?,?,?,?,?,?,?)", self.cpdlcs)
        con.executemany(
            "INSERT INTO adsc_log(t,reg,flt,dir,txt,kinds,summary,has_pos,"
            "lat,lon,alt,dec) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)", self.adscs)


def ingest_file(con, path, agg, verbose):
    size = os.path.getsize(path)
    row = con.execute(
        "SELECT offset,size FROM ingest_state WHERE file=?", (path,)
    ).fetchone()
    offset = row[0] if row else 0
    if row and size < row[1]:
        # File shrank/was replaced (shouldn't happen with append-only, but
        # be safe): re-read from the top.
        offset = 0
    if offset >= size:
        return 0

    with open(path, "rb") as f:
        f.seek(offset)
        buf = f.read()

    *complete, tail = buf.split(b"\n")
    consumed = len(buf) - len(tail)
    n = 0
    for raw in complete:
        if not raw.strip():
            continue
        try:
            agg.event(json.loads(raw))
            n += 1
        except (ValueError, KeyError):
            agg.bad += 1
    agg.lines += n

    new_offset = offset + consumed
    con.execute(
        "INSERT INTO ingest_state(file,offset,size,updated) VALUES(?,?,?,?) "
        "ON CONFLICT(file) DO UPDATE SET offset=excluded.offset, "
        "size=excluded.size, updated=excluded.updated",
        (path, new_offset, size, int(time.time())),
    )
    if verbose:
        print(f"  {os.path.basename(path)}: +{n} events "
              f"({offset}->{new_offset} of {size} bytes)")
    return n


def main():
    ap = argparse.ArgumentParser(description="Iridium Observatory rollup")
    ap.add_argument("--archive", default="archive",
                    help="directory of iridium-YYYYMMDD.jsonl files")
    ap.add_argument("--db", default="observatory/observatory.db",
                    help="SQLite database to write")
    ap.add_argument("--verbose", "-v", action="store_true")
    args = ap.parse_args()

    files = sorted(glob.glob(os.path.join(args.archive, "iridium-*.jsonl")))
    if not files:
        print(f"rollup: no archive files in {args.archive}", file=sys.stderr)
        return 1

    os.makedirs(os.path.dirname(os.path.abspath(args.db)), exist_ok=True)
    con = sqlite3.connect(args.db)
    init_db(con)

    agg = Agg()
    total = 0
    for path in files:
        total += ingest_file(con, path, agg, args.verbose)
    agg.flush(con)
    con.execute(
        "INSERT INTO meta(key,value) VALUES('last_run',?) "
        "ON CONFLICT(key) DO UPDATE SET value=excluded.value",
        (str(int(time.time())),),
    )
    con.commit()
    con.close()

    print(f"rollup: {total} new events from {len(files)} file(s)"
          + (f", {agg.bad} malformed" if agg.bad else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
