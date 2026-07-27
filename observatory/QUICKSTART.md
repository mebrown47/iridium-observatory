# Observatory — Quick Install

The Observatory is a read-only dashboard (Trends · Aviation · Doppler) that reads
the `iridium-sniffer --archive` logs. It never touches the RF engine. Stdlib
Python 3 only — **no pip, no packages to install.**

## Prerequisites

1. **A working `iridium-sniffer` checkout** on the receiver box (the same repo
   this folder lives in).
2. **The sniffer running with `--archive`** so there's data to chart, e.g.
   ```
   iridium-sniffer ... --web --archive
   ```
   This writes `archive/iridium-YYYYMMDD.jsonl`. Without it the dashboards load
   but stay empty.
3. **systemd + sudo** (for the auto-start install). Python 3.

## Install (one command)

From the repo checkout:

```
./observatory/install_observatory.sh
```

That's it. It figures out your user, paths, and python; primes the database;
and enables two services — a rollup that runs every 5 min and the dashboard.
Safe to re-run.

When it finishes it prints your dashboard URL:

```
Dashboard:  http://<this-box>:8890/
```

Open that in a browser. All times are UTC.

## If the port is firewalled

For LAN access the installer will tell you if `ufw` is blocking it:

```
sudo ufw allow 8890/tcp
```

## Common options

```
./observatory/install_observatory.sh --port 8890 --sniffer http://127.0.0.1:8888
```

| flag        | default                  | when to change it                          |
|-------------|--------------------------|--------------------------------------------|
| `--port`    | `8890`                   | port already in use                        |
| `--host`    | `0.0.0.0`                | restrict bind (e.g. `127.0.0.1`)           |
| `--sniffer` | `http://127.0.0.1:8888`  | sniffer web map is on another host/port    |
| `--archive` | `<repo>/archive`         | you point `--archive` somewhere else       |
| `--user`    | invoking user            | run the service as a different account      |

The `--sniffer` URL only matters for the **Doppler** page (it proxies live
S-curves from the running sniffer). Trends and Aviation read the archive
database, so they work even if the sniffer is stopped.

## Check it's healthy

```
curl -s http://127.0.0.1:8890/healthz          # -> ok
journalctl -u observatory-trends -f            # dashboard logs
systemctl start observatory-rollup.service     # force a rollup now
```

## Uninstall

```
./observatory/install_observatory.sh --uninstall
```

Removes the services. Your `observatory.db` is left in place — delete it by
hand if you want a clean slate.

## No systemd? Run it by hand

```
python3 observatory/rollup.py       --archive archive --db observatory/observatory.db
python3 observatory/trends_server.py --db observatory/observatory.db --port 8890
# then open http://localhost:8890/
```

Re-run the `rollup.py` line whenever you want fresh numbers (it only reads the
newly-appended bytes, so it's cheap).
