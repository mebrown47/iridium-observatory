#!/usr/bin/env python3
# Copyright (c) 2026 Mike Brown
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Iridium Observatory — Trends dashboard (M2)

A small read-only HTTP server that renders the hourly aggregates built by
rollup.py as a live "Grafana for Iridium" trends page. Stdlib only, no
external assets — it runs happily on a headless feeder with no internet.

  GET /              -> the dashboard (self-contained HTML/CSS/JS)
  GET /api/trends    -> JSON aggregates (optional ?hours=N window)
  GET /healthz       -> ok

Usage:
    trends_server.py [--db PATH] [--port 8890] [--host 0.0.0.0]

The dashboard is a *consumer* of the archive: it never touches the RF
engine or the live map. Run rollup.py on a timer to keep it current.
"""

import argparse
import json
import os
import sqlite3
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

DB_PATH = "observatory/observatory.db"
SNIFFER_URL = "http://127.0.0.1:8888"

# Iridium channel plan (mirrors gsmtap.h). Used to fold absolute burst
# frequencies into a Doppler shift about the satellite's channel centre.
IR_BASE_FREQ = 1616000000.0
IR_CHANNEL_WIDTH = 41666.667

# Kept in sync with rollup.FRAME_TYPES; drives legend order + colors.
FRAME_TYPES = ["IRA", "IBC", "MSG", "IDA", "VOC", "IIP", "IU3", "ISY",
               "IU6", "IDA_UL_FAIL", "ITL", "RAW"]


def connect_ro(path):
    if not os.path.exists(path):
        return None
    return sqlite3.connect(f"file:{path}?mode=ro", uri=True, timeout=5)


def build_trends(path, hours=None):
    con = connect_ro(path)
    if con is None:
        return {"error": "no database yet — run rollup.py first"}
    con.row_factory = sqlite3.Row
    try:
        row = con.execute(
            "SELECT MIN(hour) a, MAX(hour) b FROM frame_hourly"
        ).fetchone()
        hmin, hmax = (row["a"], row["b"]) if row else (None, None)
        if hmin is None:
            # No frame data yet; maybe only messages. Fall back to msg span.
            row = con.execute(
                "SELECT MIN(hour) a, MAX(hour) b FROM msg_hourly").fetchone()
            hmin, hmax = (row["a"], row["b"]) if row else (None, None)
        if hmin is None:
            return {"error": "database is empty — run rollup.py"}

        floor = hmin
        if hours:
            floor = max(hmin, hmax - (int(hours) - 1) * 3600)

        def rows(sql, *a):
            return con.execute(sql, a).fetchall()

        # Frame mix per hour.
        frame_hours = {}
        for r in rows("SELECT hour,type,dir,count FROM frame_hourly "
                      "WHERE hour>=? ORDER BY hour", floor):
            h = frame_hours.setdefault(
                r["hour"], {"hour": r["hour"], "types": {}, "dl": 0, "ul": 0})
            h["types"][r["type"]] = h["types"].get(r["type"], 0) + r["count"]
            h["dl" if r["dir"] == "DL" else "ul"] += r["count"]
        for r in rows("SELECT hour,dir,count FROM uw_hourly WHERE hour>=?",
                      floor):
            h = frame_hours.get(r["hour"])
            if h is not None:
                h["uw_" + r["dir"].lower()] = r["count"]
        frame_list = [frame_hours[k] for k in sorted(frame_hours)]
        for h in frame_list:
            h.setdefault("uw_dl", 0)
            h.setdefault("uw_ul", 0)

        # Messages per hour by service.
        svc_hours = {}
        for r in rows("SELECT hour,service,SUM(count) c FROM msg_hourly "
                      "WHERE hour>=? GROUP BY hour,service ORDER BY hour",
                      floor):
            h = svc_hours.setdefault(r["hour"], {"hour": r["hour"]})
            h[r["service"]] = r["c"]
        svc_list = [svc_hours[k] for k in sorted(svc_hours)]
        for h in svc_list:
            for s in ("acars", "cpdlc", "adsc", "pager"):
                h.setdefault(s, 0)

        # Active sats per hour + total frames per hour.
        active = []
        for r in rows("SELECT hour,COUNT(DISTINCT sat_id) n,SUM(count) c "
                      "FROM sat_hourly WHERE hour>=? GROUP BY hour ORDER BY "
                      "hour", floor):
            active.append({"hour": r["hour"], "n": r["n"], "frames": r["c"]})

        # Top satellites over the window.
        top_sats = [{"sat": r["sat_id"], "count": r["c"]} for r in rows(
            "SELECT sat_id,SUM(count) c FROM sat_hourly WHERE hour>=? "
            "GROUP BY sat_id ORDER BY c DESC LIMIT 24", floor)]

        # Totals over the window.
        tf = rows("SELECT dir,SUM(count) c FROM frame_hourly WHERE hour>=? "
                  "GROUP BY dir", floor)
        frames_dir = {r["dir"]: r["c"] for r in tf}
        tm = rows("SELECT service,SUM(count) c FROM msg_hourly WHERE hour>=? "
                  "GROUP BY service", floor)
        msg_tot = {r["service"]: r["c"] for r in tm}
        uw = rows("SELECT dir,SUM(count) c FROM uw_hourly WHERE hour>=? "
                  "GROUP BY dir", floor)
        uw_tot = {r["dir"]: r["c"] for r in uw}
        nsat = rows("SELECT COUNT(DISTINCT sat_id) n FROM sat_hourly "
                    "WHERE hour>=?", floor)[0]["n"]

        meta = {r[0]: r[1] for r in con.execute(
            "SELECT key,value FROM meta").fetchall()}

        return {
            "meta": {
                "generated": int(time.time()),
                "db_mtime": int(os.path.getmtime(path)),
                "last_run": int(meta.get("last_run", 0)),
                "schema": int(meta.get("schema", 0)),
            },
            "span": {"start": floor, "end": hmax,
                     "hours": (hmax - floor) // 3600 + 1},
            "totals": {
                "frames": sum(frames_dir.values()),
                "frames_dl": frames_dir.get("DL", 0),
                "frames_ul": frames_dir.get("UL", 0),
                "messages": {s: msg_tot.get(s, 0)
                             for s in ("acars", "cpdlc", "adsc", "pager")},
                "sats": nsat,
                "uw_dl": uw_tot.get("DL", 0),
                "uw_ul": uw_tot.get("UL", 0),
            },
            "frame_types": FRAME_TYPES,
            "frame_hours": frame_list,
            "service_hours": svc_list,
            "active_sats_hours": active,
            "top_sats": top_sats,
        }
    finally:
        con.close()


def build_aviation(path, tail=None, limit=200):
    con = connect_ro(path)
    if con is None:
        return {"error": "no database yet — run rollup.py first"}
    con.row_factory = sqlite3.Row
    try:
        # Message-level tables arrive with the M4 schema; if this DB predates
        # it, guide the user to re-ingest.
        have = {r["name"] for r in con.execute(
            "SELECT name FROM sqlite_master WHERE type='table'")}
        missing = {"messages", "cpdlc_log", "adsc_log"} - have
        if missing:
            return {"error": "database predates this schema (missing "
                             + ", ".join(sorted(missing)) +
                             ") — delete observatory.db and re-run rollup.py "
                             "to backfill from the archive"}

        def rows(sql, *a):
            return con.execute(sql, a).fetchall()

        span = rows("SELECT MIN(t) a, MAX(t) b FROM messages")[0]
        stats = {
            "messages": rows("SELECT COUNT(*) c FROM messages")[0]["c"],
            "tails": rows("SELECT COUNT(DISTINCT reg) c FROM messages "
                          "WHERE reg!=''")[0]["c"],
            "cpdlc": rows("SELECT COUNT(*) c FROM cpdlc_log")[0]["c"],
            "adsc": rows("SELECT COUNT(*) c FROM messages "
                         "WHERE has_adsc=1")[0]["c"],
            # Position reports are the track-replay gate: every ADS-C message
            # seen so far is contract negotiation, which carries no coords.
            "adsc_pos": rows("SELECT COUNT(*) c FROM adsc_log "
                             "WHERE has_pos=1")[0]["c"],
            "span_start": span["a"], "span_end": span["b"],
        }

        tails = [dict(r) for r in rows(
            "SELECT reg, COUNT(*) n, MAX(t) last_t, MAX(flt) flt, "
            "SUM(has_cpdlc) cpdlc, SUM(has_adsc) adsc FROM messages "
            "WHERE reg!='' GROUP BY reg ORDER BY n DESC LIMIT 120")]

        where = "WHERE reg=?" if tail else ""
        args = (tail,) if tail else ()
        msgs = [dict(r) for r in rows(
            f"SELECT t,reg,flt,dir,lbl,txt,has_cpdlc,has_adsc FROM messages "
            f"{where} ORDER BY t DESC LIMIT ?", *args, int(limit))]
        cpdlc = [dict(r) for r in rows(
            f"SELECT t,reg,flt,dir,txt,summary,dec FROM cpdlc_log "
            f"{where} ORDER BY t DESC LIMIT ?", *args, 100)]
        # Position reports first (they're the rare prize), then newest.
        adsc = [dict(r) for r in rows(
            f"SELECT t,reg,flt,dir,txt,kinds,summary,has_pos,lat,lon,alt,dec "
            f"FROM adsc_log {where} ORDER BY has_pos DESC, t DESC LIMIT ?",
            *args, 100)]

        return {"stats": stats, "tails": tails, "messages": msgs,
                "cpdlc": cpdlc, "adsc": adsc, "tail": tail or ""}
    finally:
        con.close()


def assign_channel(freq):
    """Nearest Iridium channel centre to a measured frequency. Mirrors
    assign_channel_freq() in doppler_pos.c."""
    return IR_BASE_FREQ + round(
        (freq - IR_BASE_FREQ) / IR_CHANNEL_WIDTH) * IR_CHANNEL_WIDTH


def build_doppler(sniffer_url, timeout=4):
    """Fetch the sniffer's live Doppler feed and shape it into per-satellite
    S-curves. Read-only proxy: keeps the dashboard single-origin (no CORS)
    and means the engine only ever publishes raw measurements, with all
    interpretation out here.

    Deliberately NOT folded to a channel centre. The obvious move is to
    subtract the nearest Iridium channel and plot a shift about zero, but
    real passes measured on this receiver swing ~55 kHz — wider than the
    41.67 kHz channel spacing itself — so a burst near either end of a pass
    snaps to the neighbouring channel and any "distance from centre" is
    fiction. Observed satellites also sit on channels far apart (247 vs 77),
    so there is no single centre to fold onto. We plot what was actually
    measured: absolute frequency. The S-curve shape — the whole point — is
    unaffected, and the true unshifted frequency is simply not something
    these measurements alone can honestly pin down.
    """
    url = sniffer_url.rstrip("/") + "/api/doppler"
    try:
        with urllib.request.urlopen(url, timeout=timeout) as r:
            raw = json.loads(r.read().decode("utf-8"))
    except urllib.error.URLError as e:
        return {"error": f"cannot reach iridium-sniffer at {url} "
                         f"({getattr(e, 'reason', e)}) — is it running with "
                         f"--web --position?"}
    except (ValueError, OSError) as e:
        return {"error": f"bad response from {url}: {e}"}

    if not raw.get("enabled"):
        return {"error": raw.get("note", "Doppler positioning is disabled"),
                "sats": [], "now_ms": 0}

    sats = []
    for s in raw.get("sats", []):
        f, dt = s.get("f") or [], s.get("dt") or []
        if not f or len(f) != len(dt):
            continue
        span_s = (max(dt) - min(dt)) / 1000.0 if len(dt) > 1 else 0.0
        swing = max(f) - min(f)
        drift = ((f[-1] - f[0]) / span_s) if span_s > 0 else 0.0
        sats.append({
            "sat": s.get("sat"), "n": s.get("n", len(f)),
            "dt": dt, "f": f,
            "f_min": min(f), "f_max": max(f),
            # Nearest channel to the middle of the pass — reported as a
            # locator only, never used as the plot origin (see above).
            "near_chan": int(round(
                (assign_channel((max(f) + min(f)) / 2.0) - IR_BASE_FREQ)
                / IR_CHANNEL_WIDTH)),
            "span_s": span_s,
            "swing_hz": swing,
            "drift_hz_s": drift,
            # Swing wider than the channel spacing is itself notable: it's
            # why no channel-centre line is drawn.
            "wide": swing > IR_CHANNEL_WIDTH,
        })
    # Steepest curve first: that's the satellite passing nearest overhead.
    sats.sort(key=lambda s: -abs(s["drift_hz_s"]))
    return {"now_ms": raw.get("now_ms", 0), "sats": sats,
            "sniffer": sniffer_url}


class Handler(BaseHTTPRequestHandler):
    db_path = DB_PATH
    sniffer_url = SNIFFER_URL

    def log_message(self, *a):
        pass  # quiet; systemd/journald captures what matters

    def _send(self, code, body, ctype):
        if isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def do_GET(self):
        u = urlparse(self.path)
        if u.path == "/":
            self._send(200, PAGE, "text/html; charset=utf-8")
        elif u.path == "/aviation":
            self._send(200, AVIATION_PAGE, "text/html; charset=utf-8")
        elif u.path == "/api/trends":
            q = parse_qs(u.query)
            hours = q.get("hours", [None])[0]
            try:
                data = build_trends(self.db_path, hours)
                self._send(200, json.dumps(data), "application/json")
            except Exception as e:  # noqa: keep the server alive
                self._send(500, json.dumps({"error": str(e)}),
                           "application/json")
        elif u.path == "/api/aviation":
            q = parse_qs(u.query)
            tail = q.get("tail", [None])[0] or None
            limit = q.get("limit", ["200"])[0]
            try:
                limit = max(1, min(1000, int(limit)))
            except ValueError:
                limit = 200
            try:
                data = build_aviation(self.db_path, tail, limit)
                self._send(200, json.dumps(data), "application/json")
            except Exception as e:  # noqa: keep the server alive
                self._send(500, json.dumps({"error": str(e)}),
                           "application/json")
        elif u.path == "/doppler":
            self._send(200, DOPPLER_PAGE, "text/html; charset=utf-8")
        elif u.path == "/api/doppler":
            try:
                data = build_doppler(self.sniffer_url)
                self._send(200, json.dumps(data), "application/json")
            except Exception as e:  # noqa: keep the server alive
                self._send(500, json.dumps({"error": str(e)}),
                           "application/json")
        elif u.path == "/healthz":
            self._send(200, "ok", "text/plain")
        else:
            self._send(404, "not found", "text/plain")

    do_HEAD = do_GET


PAGE = r"""<!doctype html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Iridium Observatory — Trends</title>
<style>
  :root{
    --bg:#0e1116; --panel:#161b22; --panel2:#1c232c; --line:#2b333d;
    --fg:#e6edf3; --dim:#8b98a5; --accent:#4aa8ff; --good:#3fb950;
    --warn:#d29922; --hot:#f778ba; --grid:#242c36;
  }
  *{box-sizing:border-box}
  body{margin:0;background:var(--bg);color:var(--fg);
    font:14px/1.4 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif}
  header{display:flex;align-items:baseline;gap:14px;flex-wrap:wrap;
    padding:16px 22px;border-bottom:1px solid var(--line);background:var(--panel)}
  header h1{font-size:18px;margin:0;font-weight:650;letter-spacing:.2px}
  header .sub{color:var(--dim);font-size:12.5px}
  header .spacer{flex:1}
  .nav{display:flex;gap:4px}
  .nav a{color:var(--dim);text-decoration:none;font-size:13px;padding:4px 11px;
    border-radius:6px;border:1px solid transparent}
  .nav a:hover{color:var(--fg)}
  .nav a.on{color:var(--fg);background:var(--panel2);border-color:var(--line)}
  header .status{color:var(--dim);font-size:12px}
  header .dot{display:inline-block;width:8px;height:8px;border-radius:50%;
    background:var(--good);margin-right:5px;vertical-align:middle}
  .controls{display:flex;gap:6px}
  .controls button{background:var(--panel2);color:var(--dim);border:1px solid
    var(--line);padding:4px 10px;border-radius:6px;cursor:pointer;font-size:12px}
  .controls button.on{color:var(--fg);border-color:var(--accent);
    background:#182634}
  main{padding:18px 22px;max-width:1400px;margin:0 auto}
  .cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));
    gap:12px;margin-bottom:18px}
  .card{background:var(--panel);border:1px solid var(--line);border-radius:10px;
    padding:14px 16px}
  .card .k{color:var(--dim);font-size:11.5px;text-transform:uppercase;
    letter-spacing:.6px}
  .card .v{font-size:26px;font-weight:600;margin-top:4px}
  .card .v small{font-size:13px;color:var(--dim);font-weight:400}
  .card.hot .v{color:var(--hot)}
  .grid{display:grid;grid-template-columns:2fr 1fr;gap:14px}
  @media(max-width:900px){.grid{grid-template-columns:1fr}}
  .panel{background:var(--panel);border:1px solid var(--line);border-radius:10px;
    padding:14px 16px 6px;margin-bottom:14px}
  .panel h2{font-size:13px;margin:0 0 10px;font-weight:600;color:var(--fg);
    display:flex;justify-content:space-between;align-items:center}
  .panel h2 .hint{color:var(--dim);font-weight:400;font-size:11.5px}
  .legend{display:flex;flex-wrap:wrap;gap:10px;margin:6px 0 2px;font-size:11.5px;
    color:var(--dim)}
  .legend span{display:inline-flex;align-items:center;gap:5px}
  .legend i{width:10px;height:10px;border-radius:2px;display:inline-block}
  svg{display:block;width:100%;overflow:visible}
  .ax{fill:var(--dim);font-size:10.5px}
  .gl{stroke:var(--grid);stroke-width:1}
  .empty{color:var(--dim);padding:30px;text-align:center;font-size:13px}
  .bar:hover{opacity:.82}
  a{color:var(--accent)}
</style></head>
<body>
<header>
  <h1>Iridium Observatory</h1>
  <nav class="nav"><a href="/" class="on">Trends</a><a href="/aviation">Aviation</a>
    <a href="/doppler">Doppler</a></nav>
  <span class="sub">constellation activity over time</span>
  <span class="spacer"></span>
  <div class="controls" id="win">
    <button data-h="6">6h</button>
    <button data-h="24" class="on">24h</button>
    <button data-h="72">3d</button>
    <button data-h="168">7d</button>
    <button data-h="0">all</button>
  </div>
  <span class="status" id="status"><span class="dot"></span>loading…</span>
</header>
<main>
  <div class="cards" id="cards"></div>
  <div class="panel">
    <h2>Frame mix <span class="hint">frames/hour by type — the pulse of the constellation</span></h2>
    <div class="legend" id="fmix-legend"></div>
    <div id="fmix"></div>
  </div>
  <div class="grid">
    <div>
      <div class="panel">
        <h2>Messages decoded <span class="hint">per hour by service</span></h2>
        <div class="legend" id="svc-legend"></div>
        <div id="svc"></div>
      </div>
      <div class="panel">
        <h2>Unknown-burst rate <span class="hint">unique-word failures/hour — the "what's new" tripwire</span></h2>
        <div id="uw"></div>
      </div>
    </div>
    <div>
      <div class="panel">
        <h2>Active satellites <span class="hint">distinct sats/hour</span></h2>
        <div id="sats"></div>
      </div>
      <div class="panel">
        <h2>Top satellites <span class="hint">by frames in window</span></h2>
        <div id="topsats"></div>
      </div>
    </div>
  </div>
  <p style="color:var(--dim);font-size:11.5px;margin:6px 2px 30px">
    Times UTC. Aggregated hourly by rollup.py from the sniffer JSONL archive.
    <span id="freshness"></span>
  </p>
</main>
<script>
const NS="http://www.w3.org/2000/svg";
const TYPE_COLORS={IRA:"#4aa8ff",IBC:"#3fb950",MSG:"#f778ba",IDA:"#d29922",
  VOC:"#a371f7",IIP:"#79c0ff",IU3:"#56d364",ISY:"#ff7b72",IU6:"#e3b341",
  IDA_UL_FAIL:"#f85149",ITL:"#bc8cff",RAW:"#546170"};
const SVC_COLORS={acars:"#4aa8ff",cpdlc:"#f778ba",adsc:"#d29922",pager:"#3fb950"};
let win=24;

function el(tag,attrs,txt){const e=document.createElementNS(NS,tag);
  for(const k in attrs)e.setAttribute(k,attrs[k]);
  if(txt!=null)e.textContent=txt;return e;}
function fmt(n){if(n>=1e6)return (n/1e6).toFixed(n<1e7?1:0)+"M";
  if(n>=1e3)return (n/1e3).toFixed(n<1e4?1:0)+"k";return ""+n;}
function hourLabel(h){const d=new Date(h*1000);
  return String(d.getUTCHours()).padStart(2,"0")+":00";}
function dayLabel(h){const d=new Date(h*1000);
  return (d.getUTCMonth()+1)+"/"+d.getUTCDate();}
function ago(sec){const s=Math.max(0,Math.floor(Date.now()/1000)-sec);
  if(s<90)return s+"s ago";if(s<5400)return Math.round(s/60)+"m ago";
  return Math.round(s/3600)+"h ago";}

function svg(host,h){host.innerHTML="";const s=el("svg",
  {viewBox:"0 0 1000 "+h,preserveAspectRatio:"none",height:h});
  host.appendChild(s);return s;}
function emptyMsg(host,t){host.innerHTML='<div class="empty">'+t+'</div>';}

// Stacked bar chart. series = ordered keys, rows = [{x,label2,vals:{k:v}}]
function stackedBars(host,rows,keys,colors){
  if(!rows.length){emptyMsg(host,"no data in window");return;}
  const W=1000,H=200,padL=44,padB=22,padT=8;
  const s=svg(host,H);const iw=W-padL-6,ih=H-padB-padT;
  let max=0;rows.forEach(r=>{let t=0;keys.forEach(k=>t+=(r.vals[k]||0));
    if(t>max)max=t;});
  max=max||1;
  [0,.25,.5,.75,1].forEach(f=>{const y=padT+ih-ih*f;
    s.appendChild(el("line",{x1:padL,y1:y,x2:W,y2:y,class:"gl"}));
    s.appendChild(el("text",{x:padL-6,y:y+3,"text-anchor":"end",class:"ax"},
      fmt(Math.round(max*f))));});
  const bw=iw/rows.length, bar=Math.max(1,bw*0.82);
  rows.forEach((r,i)=>{const x=padL+i*bw+(bw-bar)/2;let acc=0;
    keys.forEach(k=>{const v=r.vals[k]||0;if(!v)return;
      const hh=ih*v/max,y=padT+ih-acc-hh;acc+=hh;
      const rc=el("rect",{x:x,y:y,width:bar,height:hh,fill:colors[k]||"#888",
        class:"bar"});rc.appendChild(el("title",null,
        r.label+" · "+k+": "+v));s.appendChild(rc);});
    if(rows.length<=26||i%Math.ceil(rows.length/16)===0)
      s.appendChild(el("text",{x:x+bar/2,y:H-6,"text-anchor":"middle",
        class:"ax"},r.tick));});
}
// Line/area chart. rows=[{tick,label,v}]
function lineChart(host,rows,color,fill){
  if(!rows.length){emptyMsg(host,"no data in window");return;}
  const W=1000,H=170,padL=44,padB=22,padT=8;
  const s=svg(host,H);const iw=W-padL-6,ih=H-padB-padT;
  let max=0;rows.forEach(r=>{if(r.v>max)max=r.v;});max=max||1;
  [0,.5,1].forEach(f=>{const y=padT+ih-ih*f;
    s.appendChild(el("line",{x1:padL,y1:y,x2:W,y2:y,class:"gl"}));
    s.appendChild(el("text",{x:padL-6,y:y+3,"text-anchor":"end",class:"ax"},
      fmt(Math.round(max*f))));});
  const n=rows.length,step=n>1?iw/(n-1):0;
  const X=i=>padL+(n>1?i*step:iw/2);
  const Y=v=>padT+ih-ih*v/max;
  let d="",da="";rows.forEach((r,i)=>{const x=X(i),y=Y(r.v);
    d+=(i?"L":"M")+x.toFixed(1)+" "+y.toFixed(1)+" ";
    da+=(i?"L":"M")+x.toFixed(1)+" "+y.toFixed(1)+" ";});
  da+="L"+X(n-1).toFixed(1)+" "+(padT+ih)+" L"+X(0).toFixed(1)+" "+(padT+ih)+" Z";
  if(fill)s.appendChild(el("path",{d:da,fill:color,"fill-opacity":.14}));
  s.appendChild(el("path",{d:d,fill:"none",stroke:color,"stroke-width":2}));
  rows.forEach((r,i)=>{const c=el("circle",{cx:X(i),cy:Y(r.v),r:2.4,
    fill:color});c.appendChild(el("title",null,r.label+": "+r.v));
    s.appendChild(c);
    if(n<=26||i%Math.ceil(n/16)===0)
      s.appendChild(el("text",{x:X(i),y:H-6,"text-anchor":"middle",
        class:"ax"},r.tick));});
}
// Horizontal bars for top sats.
function hbars(host,rows){
  if(!rows.length){emptyMsg(host,"no data");return;}
  const rowH=20,padL=42,padR=48,W=1000,H=rows.length*rowH+8;
  const s=svg(host,H);const max=rows[0].count||1;
  rows.forEach((r,i)=>{const y=i*rowH+4,bw=(W-padL-padR)*r.count/max;
    s.appendChild(el("text",{x:padL-6,y:y+rowH/2+3,"text-anchor":"end",
      class:"ax"},"S"+r.sat));
    const rc=el("rect",{x:padL,y:y+2,width:Math.max(1,bw),height:rowH-6,
      rx:2,fill:"#2f6feb",class:"bar"});
    rc.appendChild(el("title",null,"sat "+r.sat+": "+r.count+" frames"));
    s.appendChild(rc);
    s.appendChild(el("text",{x:padL+bw+6,y:y+rowH/2+3,class:"ax"},
      fmt(r.count)));});
}

function card(k,v,sub,hot){return '<div class="card'+(hot?' hot':'')+
  '"><div class="k">'+k+'</div><div class="v">'+v+
  (sub?' <small>'+sub+'</small>':'')+'</div></div>';}

function tickFor(rows){ // choose hour vs day labels based on span
  const span=rows.length?rows[rows.length-1].hour-rows[0].hour:0;
  const byDay=span>36*3600;
  rows.forEach(r=>r._tick=byDay?dayLabel(r.hour):hourLabel(r.hour));
  return byDay;
}

async function load(){
  const st=document.getElementById("status");
  try{
    const r=await fetch("/api/trends"+(win?("?hours="+win):""));
    const d=await r.json();
    if(d.error){document.getElementById("cards").innerHTML="";
      emptyMsg(document.getElementById("fmix"),d.error);
      st.innerHTML='<span class="dot" style="background:var(--warn)"></span>'
        +d.error;return;}
    render(d);
    st.innerHTML='<span class="dot"></span>updated '+
      new Date().toLocaleTimeString();
  }catch(e){st.innerHTML='<span class="dot" style="background:var(--warn)">'
    +'</span>fetch failed';}
}

function render(d){
  const t=d.totals,m=t.messages;
  const uwRate=t.frames?(100*(t.uw_dl+t.uw_ul)/(t.frames+t.uw_dl+t.uw_ul)):0;
  document.getElementById("cards").innerHTML=
    card("Frames",fmt(t.frames),t.frames_ul?("DL "+fmt(t.frames_dl)+
      " · UL "+fmt(t.frames_ul)):"downlink")+
    card("ACARS msgs",fmt(m.acars),null)+
    card("CPDLC",fmt(m.cpdlc),"clearances",true)+
    card("ADS-C",fmt(m.adsc),"reports")+
    card("Pager",fmt(m.pager),null)+
    card("Active sats",fmt(t.sats),"in window")+
    card("Unknown rate",uwRate.toFixed(1)+"%","of bursts")+
    card("Span",d.span.hours+"h",dayLabel(d.span.start)+"–"+
      dayLabel(d.span.end));

  // frame mix
  const byDay=tickFor(d.frame_hours);
  const present=d.frame_types.filter(tp=>
    d.frame_hours.some(h=>h.types[tp]));
  const leg=document.getElementById("fmix-legend");leg.innerHTML="";
  present.forEach(tp=>{const sp=document.createElement("span");
    sp.innerHTML='<i style="background:'+(TYPE_COLORS[tp]||"#888")+'"></i>'+tp;
    leg.appendChild(sp);});
  stackedBars(document.getElementById("fmix"),
    d.frame_hours.map(h=>({label:(byDay?dayLabel(h.hour)+" ":"")+
      hourLabel(h.hour),tick:h._tick,vals:h.types})),present,TYPE_COLORS);

  // services
  const svcKeys=["acars","cpdlc","adsc","pager"];
  tickFor(d.service_hours);
  const sl=document.getElementById("svc-legend");sl.innerHTML="";
  svcKeys.forEach(k=>{const sp=document.createElement("span");
    sp.innerHTML='<i style="background:'+SVC_COLORS[k]+'"></i>'+k.toUpperCase();
    sl.appendChild(sp);});
  stackedBars(document.getElementById("svc"),
    d.service_hours.map(h=>({label:hourLabel(h.hour),tick:h._tick,
      vals:{acars:h.acars,cpdlc:h.cpdlc,adsc:h.adsc,pager:h.pager}})),
    svcKeys,SVC_COLORS);

  // unknown-burst rate
  tickFor(d.frame_hours);
  lineChart(document.getElementById("uw"),
    d.frame_hours.map(h=>({tick:h._tick,label:hourLabel(h.hour),
      v:(h.uw_dl||0)+(h.uw_ul||0)})),"#f85149",true);

  // active sats
  tickFor(d.active_sats_hours);
  lineChart(document.getElementById("sats"),
    d.active_sats_hours.map(h=>({tick:h._tick,label:hourLabel(h.hour),
      v:h.n})),"#3fb950",true);

  // top sats
  hbars(document.getElementById("topsats"),d.top_sats);

  const f=document.getElementById("freshness");
  if(d.meta.last_run)f.textContent=" Last rollup "+ago(d.meta.last_run)+".";
}

document.querySelectorAll("#win button").forEach(b=>{
  b.onclick=()=>{document.querySelectorAll("#win button").forEach(x=>
    x.classList.remove("on"));b.classList.add("on");
    win=+b.dataset.h;load();};});
load();
setInterval(load,30000);
</script>
</body></html>
"""


AVIATION_PAGE = r"""<!doctype html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Iridium Observatory — Aviation</title>
<style>
  :root{
    --bg:#0e1116; --panel:#161b22; --panel2:#1c232c; --line:#2b333d;
    --fg:#e6edf3; --dim:#8b98a5; --accent:#4aa8ff; --good:#3fb950;
    --warn:#d29922; --hot:#f778ba;
  }
  *{box-sizing:border-box}
  body{margin:0;background:var(--bg);color:var(--fg);
    font:14px/1.4 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif}
  header{display:flex;align-items:baseline;gap:14px;flex-wrap:wrap;
    padding:16px 22px;border-bottom:1px solid var(--line);background:var(--panel)}
  header h1{font-size:18px;margin:0;font-weight:650;letter-spacing:.2px}
  header .sub{color:var(--dim);font-size:12.5px}
  header .spacer{flex:1}
  header .status{color:var(--dim);font-size:12px}
  header .dot{display:inline-block;width:8px;height:8px;border-radius:50%;
    background:var(--good);margin-right:5px;vertical-align:middle}
  .nav{display:flex;gap:4px}
  .nav a{color:var(--dim);text-decoration:none;font-size:13px;padding:4px 11px;
    border-radius:6px;border:1px solid transparent}
  .nav a:hover{color:var(--fg)}
  .nav a.on{color:var(--fg);background:var(--panel2);border-color:var(--line)}
  main{padding:18px 22px;max-width:1400px;margin:0 auto}
  .cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));
    gap:12px;margin-bottom:18px}
  .card{background:var(--panel);border:1px solid var(--line);border-radius:10px;
    padding:14px 16px}
  .card .k{color:var(--dim);font-size:11.5px;text-transform:uppercase;
    letter-spacing:.6px}
  .card .v{font-size:26px;font-weight:600;margin-top:4px}
  .card .v small{font-size:13px;color:var(--dim);font-weight:400}
  .card.hot .v{color:var(--hot)}
  .grid{display:grid;grid-template-columns:300px 1fr;gap:14px;align-items:start}
  @media(max-width:820px){.grid{grid-template-columns:1fr}}
  .panel{background:var(--panel);border:1px solid var(--line);border-radius:10px;
    padding:14px 16px;margin-bottom:14px}
  .panel h2{font-size:13px;margin:0 0 10px;font-weight:600;
    display:flex;justify-content:space-between;align-items:center;gap:8px}
  .panel h2 .hint{color:var(--dim);font-weight:400;font-size:11.5px}
  .tails{max-height:70vh;overflow:auto}
  .tail{display:flex;justify-content:space-between;align-items:center;gap:8px;
    padding:7px 9px;border-radius:7px;cursor:pointer;font-size:13px}
  .tail:hover{background:var(--panel2)}
  .tail.on{background:#182634;border:1px solid var(--accent)}
  .tail .reg{font-weight:600;font-variant-numeric:tabular-nums}
  .tail .meta{color:var(--dim);font-size:11.5px}
  .tail .badge{font-size:10px;padding:1px 6px;border-radius:20px;margin-left:4px}
  .badge.cp{background:#3d1f33;color:var(--hot)}
  .badge.ad{background:#3a3020;color:var(--warn)}
  .n{color:var(--dim);font-variant-numeric:tabular-nums}
  input.search{width:100%;background:var(--panel2);border:1px solid var(--line);
    color:var(--fg);border-radius:7px;padding:7px 10px;font-size:13px;
    margin-bottom:8px}
  table{width:100%;border-collapse:collapse;font-size:12.5px}
  th{text-align:left;color:var(--dim);font-weight:500;font-size:11px;
    text-transform:uppercase;letter-spacing:.5px;padding:6px 8px;
    border-bottom:1px solid var(--line);position:sticky;top:0;
    background:var(--panel)}
  td{padding:6px 8px;border-bottom:1px solid #1b2129;vertical-align:top}
  tr:hover td{background:var(--panel2)}
  .mono{font-family:ui-monospace,SFMono-Regular,Menlo,monospace}
  .dir{font-size:10px;padding:1px 5px;border-radius:4px}
  .dir.DL{background:#12261a;color:var(--good)}
  .dir.UL{background:#241a2e;color:#a371f7}
  .tbl-wrap{max-height:60vh;overflow:auto}
  .cpd{border-left:3px solid var(--hot);padding:9px 12px;margin:8px 0;
    background:#161019;border-radius:0 8px 8px 0}
  .cpd .top{display:flex;gap:10px;align-items:baseline;flex-wrap:wrap}
  .cpd .reg{font-weight:600}
  .cpd .flt{color:var(--accent)}
  .cpd .ts{color:var(--dim);font-size:11.5px;margin-left:auto}
  .cpd .sum{margin-top:5px;font-family:ui-monospace,SFMono-Regular,Menlo,
    monospace;font-size:12px;color:#e6d7ef}
  .cpd .txt{margin-top:4px;color:var(--dim);font-size:12px}
  .cpd details{margin-top:6px}
  .cpd summary{color:var(--dim);font-size:11.5px;cursor:pointer}
  .cpd pre{white-space:pre-wrap;word-break:break-word;font-size:11px;
    color:var(--dim);background:#0d0a10;padding:8px;border-radius:6px;
    margin:6px 0 0;max-height:240px;overflow:auto}
  .empty{color:var(--dim);padding:26px;text-align:center;font-size:13px}
  /* ADS-C entries reuse the .cpd card shape in amber; a position report
     switches to green — that colour shift is the track-replay tripwire. */
  .ad{border-left:3px solid var(--warn);padding:9px 12px;margin:8px 0;
    background:#17130c;border-radius:0 8px 8px 0}
  .ad.pos{border-left-color:var(--good);background:#0d1a10}
  .ad .top{display:flex;gap:10px;align-items:baseline;flex-wrap:wrap}
  .ad .reg{font-weight:600}
  .ad .flt{color:var(--accent)}
  .ad .ts{color:var(--dim);font-size:11.5px;margin-left:auto}
  .ad .sum{margin-top:5px;font-family:ui-monospace,SFMono-Regular,Menlo,
    monospace;font-size:12px;color:#efe2cd}
  .ad.pos .sum{color:#c6f6d5}
  .ad .txt{margin-top:4px;color:var(--dim);font-size:12px}
  .ad details{margin-top:6px}
  .ad summary{color:var(--dim);font-size:11.5px;cursor:pointer}
  .ad pre{white-space:pre-wrap;word-break:break-word;font-size:11px;
    color:var(--dim);background:#0b0d0a;padding:8px;border-radius:6px;
    margin:6px 0 0;max-height:240px;overflow:auto}
  .kind{font-size:10px;padding:1px 6px;border-radius:20px;
    background:#3a3020;color:var(--warn)}
  .kind.rep{background:#12261a;color:var(--good)}
  .banner{border:1px solid var(--line);border-radius:9px;padding:11px 14px;
    margin-bottom:12px;font-size:12.5px;color:var(--dim);background:var(--panel2)}
  .banner.hit{border-color:var(--good);background:#0d1a10;color:#c6f6d5}
  .banner b{color:var(--fg)}
  .banner.hit b{color:var(--good)}
  .tabs{display:flex;gap:4px;margin-bottom:10px}
  .tabs button{background:var(--panel2);color:var(--dim);border:1px solid
    var(--line);padding:5px 12px;border-radius:7px;cursor:pointer;font-size:12.5px}
  .tabs button.on{color:var(--fg);border-color:var(--accent);background:#182634}
  a{color:var(--accent)}
</style></head>
<body>
<header>
  <h1>Iridium Observatory</h1>
  <nav class="nav"><a href="/">Trends</a><a href="/aviation" class="on">Aviation</a>
    <a href="/doppler">Doppler</a></nav>
  <span class="sub">decoded aircraft traffic — per-tail history &amp; CPDLC log</span>
  <span class="spacer"></span>
  <span class="status" id="status"><span class="dot"></span>loading…</span>
</header>
<main>
  <div class="cards" id="cards"></div>
  <div class="tabs" id="tabs">
    <button data-v="tails" class="on">Per-tail history</button>
    <button data-v="cpdlc">CPDLC catch log</button>
    <button data-v="adsc">ADS-C contracts</button>
  </div>

  <div id="view-tails">
    <div class="grid">
      <div class="panel">
        <h2>Aircraft <span class="hint" id="tailcount"></span></h2>
        <input class="search" id="tailsearch" placeholder="filter tails…">
        <div class="tails" id="tails"></div>
      </div>
      <div class="panel">
        <h2 id="msgtitle">Messages <span class="hint">select a tail, or newest across all</span></h2>
        <div class="tbl-wrap">
          <table><thead><tr><th>Time (UTC)</th><th>Tail</th><th>Flight</th>
            <th>Dir</th><th>Lbl</th><th>Text / type</th></tr></thead>
            <tbody id="msgrows"></tbody></table>
        </div>
      </div>
    </div>
  </div>

  <div id="view-cpdlc" style="display:none">
    <div class="panel">
      <h2>CPDLC catch log <span class="hint">every controller–pilot data-link
        message decoded off L-band</span></h2>
      <div id="cpdlclist"></div>
    </div>
  </div>

  <div id="view-adsc" style="display:none">
    <div class="panel">
      <h2>ADS-C log <span class="hint">contract negotiation &amp; position
        reports decoded off L-band</span></h2>
      <div id="adscbanner"></div>
      <div id="adsclist"></div>
    </div>
  </div>

  <p style="color:var(--dim);font-size:11.5px;margin:6px 2px 30px">
    Times UTC. From the sniffer JSONL archive via rollup.py.
    <span id="freshness"></span>
  </p>
</main>
<script>
let TAIL=null, VIEW="tails", DATA=null;

function fmt(n){if(n>=1e6)return (n/1e6).toFixed(1)+"M";
  if(n>=1e3)return (n/1e3).toFixed(1)+"k";return ""+n;}
// Every DB timestamp is epoch SECONDS (schema v3) — same unit as hourLabel's
// bucket. Multiply here, at the render boundary, and nowhere else.
function ts(sec){const d=new Date(sec*1000);
  const p=x=>String(x).padStart(2,"0");
  return p(d.getUTCMonth()+1)+"/"+p(d.getUTCDate())+" "+
    p(d.getUTCHours())+":"+p(d.getUTCMinutes())+":"+p(d.getUTCSeconds());}
function esc(s){return (s==null?"":""+s).replace(/[&<>]/g,
  c=>({"&":"&amp;","<":"&lt;",">":"&gt;"}[c]));}
function card(k,v,sub,hot){return '<div class="card'+(hot?' hot':'')+
  '"><div class="k">'+k+'</div><div class="v">'+v+
  (sub?' <small>'+sub+'</small>':'')+'</div></div>';}

async function load(){
  const st=document.getElementById("status");
  try{
    const q=TAIL?("?tail="+encodeURIComponent(TAIL)):"";
    const r=await fetch("/api/aviation"+q);
    DATA=await r.json();
    if(DATA.error){document.getElementById("cards").innerHTML="";
      document.getElementById("tails").innerHTML=
        '<div class="empty">'+esc(DATA.error)+'</div>';
      st.innerHTML='<span class="dot" style="background:var(--warn)"></span>'
        +esc(DATA.error);return;}
    render();
    st.innerHTML='<span class="dot"></span>updated '+
      new Date().toLocaleTimeString();
  }catch(e){st.innerHTML='<span class="dot" style="background:var(--warn)">'
    +'</span>fetch failed';}
}

function render(){
  const s=DATA.stats;
  document.getElementById("cards").innerHTML=
    card("Messages",fmt(s.messages),null)+
    card("Aircraft",fmt(s.tails),"tails seen")+
    card("CPDLC",fmt(s.cpdlc),"clearances",true)+
    card("ADS-C",fmt(s.adsc),(s.adsc_pos?s.adsc_pos+" w/ position"
      :"contracts only"))+
    card("Span",s.span_start?ts(s.span_start).split(" ")[0]+"–"+
      ts(s.span_end).split(" ")[0]:"—","UTC");
  renderTails();
  renderMsgs();
  renderCpdlc();
  renderAdsc();
  document.getElementById("tailcount").textContent=
    DATA.tails.length+" shown";
}

function renderTails(){
  const host=document.getElementById("tails");
  const filt=(document.getElementById("tailsearch").value||"").toUpperCase();
  const list=DATA.tails.filter(t=>!filt||t.reg.toUpperCase().includes(filt));
  if(!list.length){host.innerHTML='<div class="empty">no tails</div>';return;}
  host.innerHTML="";
  const all=document.createElement("div");
  all.className="tail"+(TAIL?"":" on");
  all.innerHTML='<span class="reg">All aircraft</span>'+
    '<span class="meta">newest across fleet</span>';
  all.onclick=()=>{TAIL=null;load();};
  host.appendChild(all);
  list.forEach(t=>{const el=document.createElement("div");
    el.className="tail"+(TAIL===t.reg?" on":"");
    el.innerHTML='<span class="reg">'+esc(t.reg)+
      (t.cpdlc?'<span class="badge cp">CPDLC</span>':'')+
      (t.adsc?'<span class="badge ad">ADS-C</span>':'')+'</span>'+
      '<span class="meta"><span class="n">'+t.n+'</span> · '+
      ts(t.last_t).split(" ")[1]+'</span>';
    el.onclick=()=>{TAIL=t.reg;load();};
    host.appendChild(el);});
}

function renderMsgs(){
  const host=document.getElementById("msgrows");
  document.getElementById("msgtitle").innerHTML="Messages "+
    (TAIL?'<span class="hint">'+esc(TAIL)+'</span>':
      '<span class="hint">newest across all tails</span>');
  if(!DATA.messages.length){host.innerHTML=
    '<tr><td colspan="6" class="empty">no messages</td></tr>';return;}
  host.innerHTML=DATA.messages.map(m=>{
    let body=esc(m.txt);
    if(m.has_cpdlc)body='<span style="color:var(--hot)">◆ CPDLC</span> '+body;
    else if(m.has_adsc)body='<span style="color:var(--warn)">◆ ADS-C</span> '+body;
    else if(!m.txt)body='<span class="n">—</span>';
    return '<tr><td class="mono n">'+ts(m.t)+'</td><td class="mono">'+
      esc(m.reg)+'</td><td>'+esc(m.flt)+'</td><td><span class="dir '+
      esc(m.dir)+'">'+esc(m.dir)+'</span></td><td class="mono">'+esc(m.lbl)+
      '</td><td>'+body+'</td></tr>';}).join("");
}

function renderCpdlc(){
  const host=document.getElementById("cpdlclist");
  if(!DATA.cpdlc.length){host.innerHTML='<div class="empty">'+
    'No CPDLC captured yet in this window — clearances are bursty. '+
    'The log fills as aircraft exchange controller messages.</div>';return;}
  host.innerHTML=DATA.cpdlc.map(c=>{
    let dj="";try{dj=JSON.stringify(JSON.parse(c.dec),null,2);}
    catch(e){dj=c.dec||"";}
    return '<div class="cpd"><div class="top">'+
      '<span class="reg">'+esc(c.reg||"—")+'</span>'+
      (c.flt?'<span class="flt">'+esc(c.flt)+'</span>':'')+
      '<span class="dir '+esc(c.dir)+'">'+esc(c.dir)+'</span>'+
      '<span class="ts">'+ts(c.t)+' UTC</span></div>'+
      (c.summary?'<div class="sum">'+esc(c.summary)+'</div>':'')+
      (c.txt?'<div class="txt">'+esc(c.txt)+'</div>':'')+
      (dj?'<details><summary>full decode</summary><pre>'+esc(dj)+
        '</pre></details>':'')+'</div>';}).join("");
}

function renderAdsc(){
  const host=document.getElementById("adsclist");
  const ban=document.getElementById("adscbanner");
  const list=DATA.adsc||[];
  const nPos=DATA.stats.adsc_pos||0;

  // The banner states plainly where the track-replay gate stands, so the
  // dashboard announces its own unblocking instead of needing a code read.
  ban.className="banner"+(nPos?" hit":"");
  ban.innerHTML=nPos
    ? '<b>Position reports arriving.</b> '+nPos+' ADS-C message'+
      (nPos===1?"":"s")+' carrying lat/lon decoded — enough to plot a track. '+
      'The track-replay slice is unblocked.'
    : '<b>Contracts only so far.</b> '+list.length+' ADS-C message'+
      (list.length===1?"":"s")+' captured, all contract negotiation — the '+
      'ground station arming reports. None carry coordinates yet. A position '+
      'report (a <code>basic_report</code> tag with lat/lon) turns this '+
      'banner green and unblocks track replay.';

  if(!list.length){host.innerHTML='<div class="empty">'+
    'No ADS-C captured yet — it is rarer than CPDLC on this link.</div>';
    return;}
  host.innerHTML=list.map(a=>{
    let dj="";try{dj=JSON.stringify(JSON.parse(a.dec),null,2);}
    catch(e){dj=a.dec||"";}
    const kinds=(a.kinds||"").split(",").filter(Boolean).map(k=>
      '<span class="kind'+(a.has_pos?' rep':'')+'">'+esc(k)+'</span>').join(" ");
    const pos=a.has_pos
      ? '<div class="sum">◆ '+a.lat.toFixed(4)+', '+a.lon.toFixed(4)+
        (a.alt!=null?' @ '+fmt(a.alt)+' ft':'')+'</div>'
      : '';
    return '<div class="ad'+(a.has_pos?' pos':'')+'"><div class="top">'+
      '<span class="reg">'+esc(a.reg||"—")+'</span>'+
      (a.flt?'<span class="flt">'+esc(a.flt)+'</span>':'')+
      '<span class="dir '+esc(a.dir)+'">'+esc(a.dir)+'</span>'+kinds+
      '<span class="ts">'+ts(a.t)+' UTC</span></div>'+pos+
      (a.summary?'<div class="sum">'+esc(a.summary)+'</div>':'')+
      (a.txt?'<div class="txt">'+esc(a.txt)+'</div>':'')+
      (dj?'<details><summary>full decode</summary><pre>'+esc(dj)+
        '</pre></details>':'')+'</div>';}).join("");
}

document.getElementById("tailsearch").oninput=renderTails;
const VIEWS=["tails","cpdlc","adsc"];
document.querySelectorAll("#tabs button").forEach(b=>{
  b.onclick=()=>{document.querySelectorAll("#tabs button").forEach(x=>
    x.classList.remove("on"));b.classList.add("on");VIEW=b.dataset.v;
    VIEWS.forEach(v=>{document.getElementById("view-"+v).style.display=
      VIEW===v?"":"none";});};});
load();
setInterval(load,30000);
</script>
</body></html>
"""


DOPPLER_PAGE = r"""<!doctype html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Iridium Observatory — Doppler</title>
<style>
  :root{
    --bg:#0e1116; --panel:#161b22; --panel2:#1c232c; --line:#2b333d;
    --fg:#e6edf3; --dim:#8b98a5; --accent:#4aa8ff; --good:#3fb950;
    --warn:#d29922; --hot:#f778ba; --grid:#242c36;
  }
  *{box-sizing:border-box}
  body{margin:0;background:var(--bg);color:var(--fg);
    font:14px/1.4 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif}
  header{display:flex;align-items:baseline;gap:14px;flex-wrap:wrap;
    padding:16px 22px;border-bottom:1px solid var(--line);background:var(--panel)}
  header h1{font-size:18px;margin:0;font-weight:650;letter-spacing:.2px}
  header .sub{color:var(--dim);font-size:12.5px}
  header .spacer{flex:1}
  header .status{color:var(--dim);font-size:12px}
  header .dot{display:inline-block;width:8px;height:8px;border-radius:50%;
    background:var(--good);margin-right:5px;vertical-align:middle}
  .nav{display:flex;gap:4px}
  .nav a{color:var(--dim);text-decoration:none;font-size:13px;padding:4px 11px;
    border-radius:6px;border:1px solid transparent}
  .nav a:hover{color:var(--fg)}
  .nav a.on{color:var(--fg);background:var(--panel2);border-color:var(--line)}
  main{padding:18px 22px;max-width:1400px;margin:0 auto}
  .cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));
    gap:12px;margin-bottom:16px}
  .card{background:var(--panel);border:1px solid var(--line);border-radius:10px;
    padding:14px 16px}
  .card .k{color:var(--dim);font-size:11.5px;text-transform:uppercase;
    letter-spacing:.6px}
  .card .v{font-size:26px;font-weight:600;margin-top:4px}
  .card .v small{font-size:13px;color:var(--dim);font-weight:400}
  .note{background:var(--panel);border:1px solid var(--line);border-radius:10px;
    padding:12px 16px;margin-bottom:16px;color:var(--dim);font-size:12.5px;
    line-height:1.55}
  .note b{color:var(--fg)}
  .note code{color:var(--accent);font-family:ui-monospace,Menlo,monospace}
  /* One satellite per facet: a single series each, so the heading carries
     identity and no legend or categorical palette is needed. */
  .facets{display:grid;grid-template-columns:repeat(auto-fit,minmax(330px,1fr));
    gap:14px}
  .facet{background:var(--panel);border:1px solid var(--line);
    border-radius:10px;padding:12px 14px 8px}
  .facet h3{margin:0;font-size:14px;font-weight:600;display:flex;
    align-items:baseline;gap:8px}
  .facet h3 .chan{color:var(--dim);font-size:11.5px;font-weight:400}
  .facet .meta{color:var(--dim);font-size:11.5px;margin:3px 0 6px;
    font-variant-numeric:tabular-nums}
  .facet .meta .amb{color:var(--warn)}
  .facet .verdict{font-size:11.5px;margin-bottom:4px}
  svg{display:block;width:100%;overflow:visible}
  .ax{fill:var(--dim);font-size:10px}
  .gl{stroke:var(--grid);stroke-width:1}
  .zero{stroke:var(--dim);stroke-width:1;stroke-dasharray:3 3;opacity:.65}
  .empty{color:var(--dim);padding:34px;text-align:center;font-size:13px;
    background:var(--panel);border:1px solid var(--line);border-radius:10px}
  a{color:var(--accent)}
</style></head>
<body>
<header>
  <h1>Iridium Observatory</h1>
  <nav class="nav"><a href="/">Trends</a><a href="/aviation">Aviation</a>
    <a href="/doppler" class="on">Doppler</a></nav>
  <span class="sub">live satellite passes, drawn in radio frequency</span>
  <span class="spacer"></span>
  <span class="status" id="status"><span class="dot"></span>loading…</span>
</header>
<main>
  <div class="cards" id="cards"></div>
  <div class="note">
    <b>What am I looking at?</b> Each panel is one satellite currently
    overhead. Iridium satellites move at ~16,000 mph, so their signal arrives
    Doppler-shifted — <b>higher</b> while approaching, sliding down through
    the true transmit frequency at closest approach, then <b>lower</b> as
    they recede. That falling curve is a picture of a satellite flying over,
    drawn with nothing but frequency. A <b>steep</b> curve means a high,
    near-overhead pass; a <b>flat</b> one means it hugged the horizon.
    Live from the receiver's IRA bursts via <code>/api/doppler</code> — every
    point is a real burst off the antenna, not a simulation.
    <br><br>
    <b>Why no zero line?</b> You'd expect the true, unshifted frequency
    marked as a centre line. We don't draw one: a real pass here swings
    ~55 kHz, <i>wider than Iridium's 41.67 kHz channel spacing</i>, so
    snapping bursts to a nearest channel misassigns them at both ends of the
    pass. These measurements alone can't honestly pin the centre, so the axis
    shows what was actually measured. Each panel is scaled to its own
    satellite's frequency range — compare <i>shapes</i>, not heights.
  </div>
  <div id="facets" class="facets"></div>
  <p style="color:var(--dim);font-size:11.5px;margin:12px 2px 30px">
    Absolute measured burst frequency, each panel on its own scale.
    Updates every 5s. <span id="src"></span>
  </p>
</main>
<script>
const NS="http://www.w3.org/2000/svg";
let LAST=null;
function el(tag,attrs,txt){const e=document.createElementNS(NS,tag);
  for(const k in attrs)if(attrs[k]!=null)e.setAttribute(k,attrs[k]);
  if(txt!=null)e.textContent=txt;return e;}
function esc(s){return (s==null?"":""+s).replace(/[&<>]/g,
  c=>({"&":"&amp;","<":"&lt;",">":"&gt;"}[c]));}
function card(k,v,sub){return '<div class="card"><div class="k">'+k+
  '</div><div class="v">'+v+(sub?' <small>'+sub+'</small>':'')+'</div></div>';}

/* Single-series S-curve. x = seconds ago (0 = now, right edge),
   y = absolute measured frequency, labelled in kHz above the facet's own
   floor so the ticks stay readable (a 1626.xxxx MHz axis is unreadable at
   kHz resolution). Each facet gets its own y-scale: satellites sit on
   different channels, so a shared scale would flatten every curve to a
   line. The heading carries the absolute MHz. */
function sCurve(host,s){
  // Size the viewBox to the facet's real pixel width so 1 unit = 1 px.
  // A fixed wide viewBox squeezed into a narrow facet needs
  // preserveAspectRatio="none", which scales x and y unequally and shears
  // every tick label into mush.
  const H=210,padL=54,padR=12,padB=26,padT=10;
  const W=Math.max(260,Math.round(host.clientWidth||420));
  host.innerHTML="";
  const svg=el("svg",{viewBox:"0 0 "+W+" "+H,width:W,height:H});
  host.appendChild(svg);
  const iw=W-padL-padR, ih=H-padB-padT;

  const xs=s.dt.map(v=>v/1000);            // seconds, negative = past
  let x0=Math.min(...xs), x1=Math.max(...xs);
  if(x1-x0<1){x0-=1;x1+=1;}
  let f0=s.f_min, f1=s.f_max;
  if(f1-f0<200){f0-=100;f1+=100;}          // near-flat pass: don't magnify noise
  const pad=(f1-f0)*0.12; f0-=pad; f1+=pad;
  const base=f0;                            // facet floor, in Hz

  const X=v=>padL+iw*(v-x0)/(x1-x0);
  const Y=v=>padT+ih*(1-(v-f0)/(f1-f0));

  for(let i=0;i<=4;i++){
    const v=f0+(f1-f0)*i/4, y=Y(v);
    svg.appendChild(el("line",{x1:padL,y1:y,x2:W-padR,y2:y,class:"gl"}));
    svg.appendChild(el("text",{x:padL-7,y:y+3,"text-anchor":"end",
      class:"ax"},"+"+((v-base)/1000).toFixed(1)));
  }
  svg.appendChild(el("text",{x:12,y:padT+ih/2,class:"ax",
    transform:"rotate(-90 12 "+(padT+ih/2)+")","text-anchor":"middle"},
    "kHz above "+(base/1e6).toFixed(4)+" MHz"));
  for(let i=0;i<=4;i++){
    const v=x0+(x1-x0)*i/4;
    svg.appendChild(el("text",{x:X(v),y:H-8,"text-anchor":"middle",
      class:"ax"},Math.round(v)+"s"));
  }
  let d="";
  xs.forEach((x,i)=>{d+=(i?"L":"M")+X(x).toFixed(1)+" "+Y(s.f[i]).toFixed(1)+" ";});
  svg.appendChild(el("path",{d:d,fill:"none",stroke:"var(--accent)",
    "stroke-width":2,"stroke-linejoin":"round","stroke-linecap":"round"}));
  xs.forEach((x,i)=>{
    const c=el("circle",{cx:X(x),cy:Y(s.f[i]),r:2.6,fill:"var(--accent)"});
    c.appendChild(el("title",null,
      Math.round(x)+"s ago · "+(s.f[i]/1e6).toFixed(5)+" MHz"));
    svg.appendChild(c);
  });
}

function verdict(s){
  // Read the pass geometry off the slope, as the explainer describes:
  // steep = the satellite swept nearly overhead, flat = it hugged the horizon.
  // Only judge once there's enough of an arc to judge: any short window
  // caught near closest approach looks steep no matter how high the pass
  // actually got, so a handful of bursts over a few seconds says nothing.
  if(s.n<8||s.span_s<60)
    return '<span style="color:var(--dim)">collecting — too little of the pass '+
      'to call yet</span>';
  const drift=Math.abs(s.drift_hz_s);
  if(drift>=40) return '<span style="color:var(--good)">steep — high, near-overhead pass</span>';
  if(drift>=10) return '<span style="color:var(--accent)">moderate slope — decent pass</span>';
  return '<span style="color:var(--dim)">shallow — low on the horizon</span>';
}

async function load(){
  const st=document.getElementById("status");
  try{
    const r=await fetch("/api/doppler");
    const d=await r.json();
    const facets=document.getElementById("facets");
    if(d.error){
      document.getElementById("cards").innerHTML="";
      facets.className="";
      facets.innerHTML='<div class="empty">'+esc(d.error)+'</div>';
      st.innerHTML='<span class="dot" style="background:var(--warn)"></span>'+
        'no feed';
      return;
    }
    facets.className="facets";
    LAST=d;
    render(d);
    st.innerHTML='<span class="dot"></span>updated '+
      new Date().toLocaleTimeString();
  }catch(e){
    st.innerHTML='<span class="dot" style="background:var(--warn)"></span>'+
      'fetch failed';
  }
}

function render(d){
  const sats=d.sats||[];
  const pts=sats.reduce((a,s)=>a+s.n,0);
  const best=sats.length?sats[0]:null;
  document.getElementById("cards").innerHTML=
    card("Overhead",sats.length,"satellites")+
    card("Bursts",pts,"in buffers")+
    card("Steepest",best?Math.abs(best.drift_hz_s).toFixed(1):"—","Hz/s"+
      (best?" · sat "+best.sat:""))+
    card("Widest swing",best?Math.max(...sats.map(s=>s.swing_hz/1000))
      .toFixed(1):"—","kHz");
  document.getElementById("src").textContent=
    d.sniffer?("Source: "+d.sniffer+"/api/doppler"):"";

  const host=document.getElementById("facets");
  if(!sats.length){
    host.className="";
    host.innerHTML='<div class="empty">No satellites overhead right now — '+
      'wait for the next pass. Curves appear as IRA bursts arrive.</div>';
    return;
  }
  host.className="facets";
  host.innerHTML="";
  // Two passes: insert every facet, THEN draw. Charts size themselves to
  // their facet's real width, and an auto-fit grid collapses empty tracks —
  // so a facet measured while it's still the only child reports the full
  // container width and is then squeezed when its siblings arrive.
  const pending=[];
  sats.forEach(s=>{
    const f=document.createElement("div");
    f.className="facet";
    f.innerHTML='<h3>Sat '+s.sat+'<span class="chan">'+
      (s.f_min/1e6).toFixed(4)+'–'+(s.f_max/1e6).toFixed(4)+' MHz · near ch'+
      s.near_chan+'</span></h3>'+
      '<div class="meta">'+s.n+' bursts · '+Math.round(s.span_s)+'s · swing '+
      (s.swing_hz/1000).toFixed(2)+' kHz · drift '+
      s.drift_hz_s.toFixed(1)+' Hz/s'+
      (s.wide?' · <span class="amb">swing &gt; channel spacing</span>':'')+
      '</div><div class="verdict">'+verdict(s)+'</div>';
    const chart=document.createElement("div");
    f.appendChild(chart);
    host.appendChild(f);
    pending.push([chart,s]);
  });
  pending.forEach(([chart,s])=>sCurve(chart,s));
}
// Charts are sized in real pixels, so a resize needs a redraw.
let rz=null;
window.addEventListener("resize",()=>{clearTimeout(rz);
  rz=setTimeout(()=>{if(LAST)render(LAST);},150);});
load();
setInterval(load,5000);
</script>
</body></html>
"""


def main():
    ap = argparse.ArgumentParser(description="Iridium Observatory trends server")
    ap.add_argument("--db", default=DB_PATH)
    ap.add_argument("--port", type=int, default=8890)
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--sniffer", default=SNIFFER_URL,
                    help="base URL of the running iridium-sniffer web map "
                         "(read-only; supplies /api/doppler)")
    args = ap.parse_args()

    Handler.db_path = args.db
    Handler.sniffer_url = args.sniffer
    srv = ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"observatory trends: http://{args.host}:{args.port}/  db={args.db}")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
