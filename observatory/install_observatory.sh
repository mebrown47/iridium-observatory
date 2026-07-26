#!/usr/bin/env bash
#
# install_observatory.sh — one-shot installer for the Iridium Observatory
# analytics layer (rollup timer + Trends/Aviation/Doppler dashboards on :8890).
#
# Makes the two-process design deploy like a single artifact: it templates the
# systemd units to THIS machine's user / paths / python, installs and enables
# them, and runs a first rollup so the dashboard has data on first load. The
# Observatory is stdlib-only Python 3 — no pip, no external packages.
#
# Usage (run from the repo, or anywhere):
#     observatory/install_observatory.sh [options]
#
# Options:
#     --user USER        service account to run as   (default: invoking user)
#     --port PORT        dashboard port               (default: 8890)
#     --host HOST        bind address                 (default: 0.0.0.0)
#     --sniffer URL      running sniffer web map      (default: http://127.0.0.1:8888)
#     --archive DIR      --archive JSONL directory    (default: <repo>/archive)
#     --db PATH          SQLite database path         (default: <repo>/observatory/observatory.db)
#     --uninstall        stop, disable and remove the units, then exit
#     -h, --help         show this help
#
# Re-running is safe (idempotent). Needs systemd and sudo for the unit install.
#
set -euo pipefail

# ---- pretty output ----------------------------------------------------------
if [ -t 1 ]; then B=$'\033[1m'; G=$'\033[32m'; Y=$'\033[33m'; R=$'\033[31m'; Z=$'\033[0m'
else B=""; G=""; Y=""; R=""; Z=""; fi
say()  { printf '%s\n' "$*"; }
info() { printf '%s==>%s %s\n' "$B" "$Z" "$*"; }
ok()   { printf '  %sok%s   %s\n' "$G" "$Z" "$*"; }
warn() { printf '  %swarn%s %s\n' "$Y" "$Z" "$*"; }
die()  { printf '  %serror%s %s\n' "$R" "$Z" "$*" >&2; exit 1; }

# ---- resolve locations ------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"     # holds observatory/ and archive/
OBS_DIR="$SCRIPT_DIR"

# ---- defaults ---------------------------------------------------------------
RUN_USER="${SUDO_USER:-$(id -un)}"
PORT=8890
HOST=0.0.0.0
SNIFFER="http://127.0.0.1:8888"
ARCHIVE_DIR="$REPO_ROOT/archive"
DB_PATH="$OBS_DIR/observatory.db"
UNINSTALL=0

UNITS=(observatory-rollup.service observatory-rollup.timer observatory-trends.service)

# ---- parse args -------------------------------------------------------------
while [ $# -gt 0 ]; do
  case "$1" in
    --user)     RUN_USER="$2"; shift 2;;
    --port)     PORT="$2"; shift 2;;
    --host)     HOST="$2"; shift 2;;
    --sniffer)  SNIFFER="$2"; shift 2;;
    --archive)  ARCHIVE_DIR="$2"; shift 2;;
    --db)       DB_PATH="$2"; shift 2;;
    --uninstall) UNINSTALL=1; shift;;
    -h|--help)  sed -n '2,30p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0;;
    *) die "unknown option: $1 (try --help)";;
  esac
done

# ---- privilege helpers ------------------------------------------------------
# Run a command as root (directly if already root, else via sudo).
as_root() { if [ "$(id -u)" -eq 0 ]; then "$@"; else sudo "$@"; fi; }
# Run a command as the service user (for correct DB ownership).
as_user() {
  if [ "$(id -un)" = "$RUN_USER" ]; then "$@"
  elif [ "$(id -u)" -eq 0 ]; then sudo -u "$RUN_USER" "$@"
  else sudo -u "$RUN_USER" "$@"; fi
}

command -v systemctl >/dev/null 2>&1 || die "systemd (systemctl) is required"

# ---- uninstall path ---------------------------------------------------------
if [ "$UNINSTALL" -eq 1 ]; then
  info "Removing Observatory units"
  as_root systemctl disable --now observatory-trends.service 2>/dev/null || true
  as_root systemctl disable --now observatory-rollup.timer   2>/dev/null || true
  for u in "${UNITS[@]}"; do as_root rm -f "/etc/systemd/system/$u"; done
  as_root systemctl daemon-reload
  ok "units removed (observatory.db left in place; delete it by hand if you want a clean slate)"
  exit 0
fi

# ---- preflight --------------------------------------------------------------
info "Preflight"
PYTHON="$(command -v python3 || true)"
[ -n "$PYTHON" ] || die "python3 not found — install it (stdlib only, no pip needed)"
ok "python3: $PYTHON ($("$PYTHON" -V 2>&1))"
[ -f "$OBS_DIR/rollup.py" ]        || die "missing $OBS_DIR/rollup.py — run this from the repo checkout"
[ -f "$OBS_DIR/trends_server.py" ] || die "missing $OBS_DIR/trends_server.py"
"$PYTHON" -m py_compile "$OBS_DIR/rollup.py" "$OBS_DIR/trends_server.py" \
  || die "observatory python failed to compile"
ok "observatory sources present and compile"
id "$RUN_USER" >/dev/null 2>&1 || die "service user '$RUN_USER' does not exist"
ok "service user: $RUN_USER"

have_archive() { find "$ARCHIVE_DIR" -maxdepth 1 -name 'iridium-*.jsonl' -print -quit 2>/dev/null | grep -q .; }
if have_archive; then
  ok "archive found: $ARCHIVE_DIR"
else
  warn "no iridium-*.jsonl in $ARCHIVE_DIR yet."
  warn "The dashboards need iridium-sniffer running with --archive to have data."
  warn "Install will proceed; Trends/Aviation fill in once the archive is written."
fi

# ---- initial rollup so the DB exists before the service starts --------------
if have_archive; then
  info "Priming the database (first rollup)"
  as_user "$PYTHON" "$OBS_DIR/rollup.py" --archive "$ARCHIVE_DIR" --db "$DB_PATH" \
    && ok "initial rollup complete: $DB_PATH" \
    || warn "initial rollup returned non-zero — the timer will retry every 5 min"
fi

# ---- render + install units -------------------------------------------------
info "Installing systemd units (templated to this machine)"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT

cat > "$TMP/observatory-rollup.service" <<EOF
# Generated by install_observatory.sh — folds the sniffer JSONL archive into
# hourly SQLite aggregates. Oneshot, driven by the .timer.
[Unit]
Description=Iridium Observatory rollup (JSONL archive -> SQLite aggregates)
After=iridium-sniffer.service

[Service]
Type=oneshot
User=$RUN_USER
WorkingDirectory=$REPO_ROOT
ExecStart=$PYTHON $OBS_DIR/rollup.py --archive $ARCHIVE_DIR --db $DB_PATH
Nice=10
EOF

cat > "$TMP/observatory-rollup.timer" <<EOF
# Generated by install_observatory.sh — runs the rollup every 5 minutes.
[Unit]
Description=Run Iridium Observatory rollup every 5 minutes

[Timer]
OnBootSec=2min
OnUnitActiveSec=5min
AccuracySec=30s
Persistent=true

[Install]
WantedBy=timers.target
EOF

cat > "$TMP/observatory-trends.service" <<EOF
# Generated by install_observatory.sh — read-only dashboards on port $PORT.
# Degrades to a clear message if the sniffer is down; never writes to it.
[Unit]
Description=Iridium Observatory dashboards (read-only, port $PORT)
After=network-online.target iridium-sniffer.service
Wants=network-online.target

[Service]
Type=simple
User=$RUN_USER
WorkingDirectory=$REPO_ROOT
ExecStart=$PYTHON $OBS_DIR/trends_server.py --db $DB_PATH --port $PORT --host $HOST --sniffer $SNIFFER
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF

for u in "${UNITS[@]}"; do
  as_root install -m 0644 "$TMP/$u" "/etc/systemd/system/$u"
done
ok "units written to /etc/systemd/system/"

as_root systemctl daemon-reload
as_root systemctl enable --now observatory-rollup.timer
as_root systemctl enable --now observatory-trends.service
ok "rollup timer + trends service enabled and started"

# ---- report -----------------------------------------------------------------
IP="$(hostname -I 2>/dev/null | awk '{print $1}')"; IP="${IP:-localhost}"
info "Done"
say "  Dashboard:  ${B}http://$IP:$PORT/${Z}   (Trends · Aviation · Doppler)"
say "  Health:     curl -s http://127.0.0.1:$PORT/healthz"
say "  Logs:       journalctl -u observatory-trends -f"
say "  Rollup now: sudo systemctl start observatory-rollup.service"
if command -v ufw >/dev/null 2>&1 && as_root ufw status 2>/dev/null | grep -q '^Status: active'; then
  warn "ufw is active — allow the port for LAN access:  sudo ufw allow $PORT/tcp"
fi
if ! have_archive; then
  warn "Reminder: start iridium-sniffer with --archive so the dashboards get data."
fi
