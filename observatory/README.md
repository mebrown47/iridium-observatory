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

## Next

Promote the unknown-burst panel into a real discovery instrument: baseline
vs. current, alert on sustained deviation. The `uw_hourly` table is the
seed; this will add per-burst context (freq, timing, length) from new archive
event types.
