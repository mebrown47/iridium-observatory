# Iridium Observatory (Rollups + Trends)

A read-only analytics layer over the append-only JSONL archive written by `iridium-sniffer --archive`. It
never touches the RF engine or the live map — it is purely a *consumer* of the archive.

```
  iridium-sniffer --archive          rollup.py (timer, every 5 min)
   └─ archive/iridium-YYYYMMDD.jsonl ───────────────▶ observatory.db (SQLite)
                                                              │
                                          trends_server.py (:8890) ── browser
```

## Components

- **`rollup.py`** — folds the JSONL archive into hourly SQLite aggregates.
  Incremental (tracks per-file byte offsets in `ingest_state`), so each run
  only reads newly-appended bytes and re-running is safe and cheap. Delete
  `observatory.db` to force a clean full re-ingest.

- **`trends_server.py`** — stdlib HTTP server; renders the aggregates as a
  self-contained dashboard (no external assets, works offline on the feeder).
  `GET /` dashboard · `GET /api/trends[?hours=N]` JSON · `GET /healthz`.

## Aggregate tables (`observatory.db`)

| table          | grain                         | source event |
|----------------|-------------------------------|--------------|
| `frame_hourly` | hour × type × dir → count     | `fmix`       |
| `uw_hourly`    | hour × dir → unknown-burst ct | `fmix` `uw`  |
| `sat_hourly`   | hour × sat_id → frame count   | `fmix` `sats`|
| `msg_hourly`   | hour × service × dir → count  | `acars`,`pager` (service ∈ acars/cpdlc/adsc/pager) |
| `ingest_state` | per-file consumed byte offset | —            |
| `meta`         | schema version, last_run      | —            |

CPDLC and ADS-C are detected by walking each `acars` event's decoded
libacars tree (`dec`) for a `cpdlc`/`adsc` node, so a single ACARS message
can count toward `acars` and `cpdlc`/`adsc` simultaneously.

## Run manually

```
python3 observatory/rollup.py --archive archive --db observatory/observatory.db
python3 observatory/trends_server.py --db observatory/observatory.db --port 8890
# open http://localhost:8890/
```

## Deploy (systemd service)

One-shot installer — templates the units to this machine's user / paths /
python, primes the database, and enables both services:

```
observatory/install_observatory.sh
# options: --user --port --host --sniffer --archive --db --uninstall
```

It is stdlib-only (no pip) and safe to re-run. Porting to another box (e.g. a
second feeder) needs nothing but the checkout + this script.

<details><summary>Manual equivalent</summary>

```
sudo cp observatory/observatory-rollup.service /etc/systemd/system/
sudo cp observatory/observatory-rollup.timer   /etc/systemd/system/
sudo cp observatory/observatory-trends.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now observatory-rollup.timer   # rollup every 5 min
sudo systemctl enable --now observatory-trends         # dashboard on :8890
```
The committed units hardcode `User=mike` and `/home/mike/iridium-sniffer`;
edit them for another account. The installer does this substitution for you.
</details>

Dashboard: `http://<feeder>:8890/`. All times UTC.

## Dashboard panels

- **Summary cards** — frames (DL/UL), ACARS / CPDLC / ADS-C / pager counts,
  active sats, unknown-burst rate, time span.

- **Frame mix** — stacked frames/hour by type (the constellation's pulse).

- **Messages decoded** — per-hour by service, CPDLC highlighted.

- **Unknown-burst rate** — unique-word failures/hour: the "what's new"
  tripwire in seed form.

- **Active satellites** / **Top satellites** — coverage over time.

## Flights page (`/flights`)

A map and list of the aircraft heard on Iridium ACARS in the last few
hours, and what their own messages say about where they are or are going.
Every mark means what it shows:

- **Solid dot**: a position the aircraft reported. That's ADS-C, or
  coordinates or a named fix in its own position report, as the receiver
  placed it. The receiver writes these to the archive as `acpos` events, and
  the rollup keeps them in `ac_pos`.
- **Ring around the station**: heard directly. An aircraft-sent message
  means the station received the aircraft itself. Checked against real VDL2
  positions, 59 of 60 such aircraft were within 300 km.
- **Dashed line**: the aircraft's flight plan, from an H1 `FPN` uplink
  (origin, destination, route, planned departure and cruise level), or
  origin to destination from a "flight plan available" notice (which may be
  for its next flight). It shows where the aircraft is going, not where it
  is. Checked against VDL2, 22 of 25 plans agreed with where the aircraft
  flew.
- **List**: also shows arrival or departure ATIS ("inbound KDFW") and
  takeoff data ("departing KATL").

Ground-sent coordinates and fix names are never treated as positions: they
are route points, weather and the like. PWI (predicted winds) is not used:
`FIX,DDDSSS` there is wind, not bearing and distance.

**Estimated positions are off** (`context.ESTIMATES_ENABLED`). The code can
walk an aircraft along its plan from a departure time, with a circle that
grows with every doubt. `tools/check_estimates.py` replays a station's
history against VDL2 positions and requires the true position inside the
circle for at least 80% of estimates. On the home feeder's data (November
2026) the result was:
- 61–73% inside, with circles of about 475 km median;
- full flight plans alone did better (29–36 of 39–40);
- departure times are the weak point: a plan often arrives during the
  previous leg, and the plan's own `DT` field didn't fix that.

Run the check again on more data before turning estimates on.

Reference data in `data/`, built by `tools/build_reference_data.py`:
- `airports.csv` comes from [OurAirports](https://ourairports.com/data/) (public domain).
- `navaids.csv` comes from the FAA aeronautical data in
  [iridium-aircraft-geolocation](https://github.com/mebrown47/iridium-aircraft-geolocation)'s
  `faa_navaid_database.json`: VORs, NDBs and fixes missing from
  `data/waypoints.csv`. Identifiers repeat worldwide; a route takes the
  candidate nearest its previous point.

## Next

Promote the unknown-burst panel into a real discovery instrument: baseline
vs. current, alert on sustained deviation. The `uw_hourly` table is the
seed; this will add per-burst context (freq, timing, length) from new archive
event types.
