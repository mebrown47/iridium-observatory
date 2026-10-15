#!/usr/bin/env bash
# Copyright (c) 2026 Mike Brown
# SPDX-License-Identifier: GPL-3.0-or-later
#
# diagnose.sh — gather what's needed to troubleshoot an iridium-observatory
# install into one text file: OS and tool versions, which SDR and ACARS
# libraries the program really loads (and duplicate copies), SDR device
# permissions and udev rules, service state, ports, the live web endpoints,
# data folders, and recent logs.
#
# Usage (from the repo checkout, as your normal user; changes nothing):
#     ./diagnose.sh                write diagnose-<host>-<UTC time>.txt here
#     ./diagnose.sh -o FILE        write FILE instead
#
# The report includes your settings file (feed addresses, station ID) and
# the receiver's estimated location. Look it over before posting it publicly.
#
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PREFIX=/opt/iridium-observatory
CONF=/etc/iridium-observatory.conf
DATA_DIR=/var/lib/iridium-observatory
OUT=""

while [ $# -gt 0 ]; do
  case "$1" in
    -o) OUT="${2:?-o needs a file name}"; shift;;
    -h|--help) sed -n '2,17p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0;;
    *) echo "unknown option: $1 (try --help)" >&2; exit 1;;
  esac
  shift
done
[ -n "$OUT" ] || OUT="$PWD/diagnose-$(hostname -s)-$(date -u +%Y%m%d-%H%M%S).txt"
: > "$OUT" || { echo "cannot write $OUT" >&2; exit 1; }

have() { command -v "$1" >/dev/null 2>&1; }
section() { printf '\n\n======== %s ========\n' "$1" >> "$OUT"; printf '  %s\n' "$1"; }
# Run a command into the report under a "$ cmd" line; never stop on failure.
run() {
  printf '\n$ %s\n' "$*" >> "$OUT"
  timeout 30 bash -c "$*" >> "$OUT" 2>&1
  local rc=$?
  [ $rc -eq 0 ] || printf '[exit %d]\n' "$rc" >> "$OUT"
}
note() { printf '%s\n' "$*" >> "$OUT"; }

# The args of a systemd unit's ExecStart, one per line (first = the program).
unit_argv() {
  systemctl show -p ExecStart "$1" 2>/dev/null \
    | sed -n 's/.*argv\[\]=\([^;]*\);.*/\1/p' | tr ' ' '\n' | sed '/^$/d'
}
unit_wd() { systemctl show -p WorkingDirectory --value "$1" 2>/dev/null; }

# Where things really are: the installer's locations, or whatever the running
# units say (a hand-written unit, as on the original feeder).
SNIFFER_BIN="$(unit_argv iridium-sniffer.service | head -1)"
[ -n "$SNIFFER_BIN" ] || SNIFFER_BIN="$PREFIX/bin/iridium-sniffer"
SNIFFER_WD="$(unit_wd iridium-sniffer.service)"
WEB_PORT=8888; OBS_PORT=8890
# shellcheck disable=SC1090
[ -r "$CONF" ] && . "$CONF" 2>/dev/null
p="$(unit_argv iridium-sniffer.service | sed -n 's/^--web=//p' | tail -1)"
[[ "$p" =~ ^[0-9]+$ ]] && WEB_PORT="$p"
p="$(unit_argv observatory-trends.service | grep -A1 -x -- --port | tail -1)"
[[ "$p" =~ ^[0-9]+$ ]] && OBS_PORT="$p"
ARCHIVE="$(unit_argv iridium-sniffer.service | sed -n 's/^--archive=//p; s/^--archive$/archive/p' | tail -1)"
[ -n "$ARCHIVE" ] && [ "${ARCHIVE#/}" = "$ARCHIVE" ] && ARCHIVE="${SNIFFER_WD:-$PWD}/$ARCHIVE"
DB="$(unit_argv observatory-trends.service | grep -A1 -x -- --db | tail -1)"
[ -n "$DB" ] && [ "${DB#/}" = "$DB" ] && DB="$(unit_wd observatory-trends.service)/$DB"

# Known SDRs by USB vendor:product (the installer's list).
SDR_IDS='1d50:6089|1d50:604b|1d50:cc15|1d50:60a1|0bda:2832|0bda:2838|2cf0:5246|2cf0:5250|1d50:6066|1df7:.*'

echo "Writing $OUT"
{
  echo "iridium-observatory diagnostics"
  echo "generated $(date -u '+%Y-%m-%d %H:%M:%S UTC') by $(id -un) on $(hostname)"
  echo "sniffer binary: $SNIFFER_BIN   web :$WEB_PORT   observatory :$OBS_PORT"
} >> "$OUT"

section "System"
run "cat /etc/os-release"
run "uname -a"
run "ls /etc/*dragon* /usr/share/dragonos* 2>/dev/null; grep -ri dragon /etc/lsb-release /etc/issue 2>/dev/null; true"
run "id"
run "nproc; free -h; df -h / ${DATA_DIR} 2>/dev/null; true"
run "for t in gcc cmake python3 git curl; do printf '%-8s ' \$t; \$t --version 2>&1 | head -1; done"
run "timedatectl 2>/dev/null | grep -Ei 'synchron|NTP|time zone' || true"

section "Install"
run "cat $PREFIX/VERSION 2>/dev/null || echo 'no $PREFIX/VERSION (not installed by install.sh?)'"
run "git -C '$REPO_ROOT' describe --always --dirty --tags; git -C '$REPO_ROOT' log --oneline -3; git -C '$REPO_ROOT' status --short | head -20"
run "cat $CONF 2>/dev/null || echo 'no $CONF'"
run "ls -la $PREFIX $PREFIX/bin $PREFIX/observatory 2>/dev/null | head -40"
if [ -f "$REPO_ROOT/build-install.log" ]; then
  run "grep -E 'enabled|not found|NOTFOUND|Found ' '$REPO_ROOT/build-install.log' | head -30"
fi
[ -f "$REPO_ROOT/build-install/tests.log" ] && run "grep -E 'RESULT|FAIL' '$REPO_ROOT/build-install/tests.log'"

section "Libraries"
note "Which copy of each library the sniffer loads; a second copy elsewhere"
note "(e.g. /usr/local from source vs /usr/lib from apt) is a common conflict."
run "ldd '$SNIFFER_BIN' 2>&1 | grep -Ei 'airspy|hackrf|rtlsdr|soapy|bladerf|uhd|sdrplay|acars|fftw|usb|not found'"
run "for l in libairspy libhackrf librtlsdr libSoapySDR libacars-2 libfftw3f libusb-1.0; do ls -la /usr/lib/x86_64-linux-gnu/\$l.so* /usr/lib/aarch64-linux-gnu/\$l.so* /usr/lib/\$l.so* /usr/local/lib/\$l.so* /usr/local/lib64/\$l.so* 2>/dev/null; done; true"
run "for m in libacars-2 libairspy libhackrf librtlsdr SoapySDR fftw3f libusb-1.0; do printf '%-12s ' \$m; pkg-config --modversion \$m 2>&1; done"
run "dpkg -l 2>/dev/null | grep -Ei 'airspy|hackrf|rtl-?sdr|soapy|bladerf|libuhd|acars|fftw3' | awk '{print \$1, \$2, \$3}'"
run "ls /usr/lib/*/SoapySDR/modules*/ /usr/local/lib/SoapySDR/modules*/ 2>/dev/null; true"

section "SDR hardware and permissions"
note "The service runs as its user with group plugdev: each SDR node must be"
note "group plugdev and group-writable (or world-writable)."
run "lsusb 2>/dev/null | grep -Ei '1d50:|0bda:283|2cf0:|1df7:|2500:|3923:' || echo 'no known SDR on USB'"
{
  printf '\n$ (SDR device nodes)\n'
  found=0
  for d in /sys/bus/usb/devices/*; do
    [ -r "$d/idVendor" ] || continue
    id="$(<"$d/idVendor"):$(<"$d/idProduct")"
    [[ "$id" =~ ^($SDR_IDS)$ ]] || continue
    found=1
    node="$(printf '/dev/bus/usb/%03d/%03d' "$(<"$d/busnum")" "$(<"$d/devnum")")"
    g="$(stat -c %G "$node" 2>/dev/null)"; m="$(stat -c %a "$node" 2>/dev/null)"
    if { [ "$g" = plugdev ] && (( 8#${m:-0} & 8#020 )); } || (( 8#${m:-0} & 8#002 )); then v=OK; else v="PROBLEM: the service can't open it"; fi
    printf '%s  %s  group %s mode %s  %s  [%s]\n' "$id" "$node" "$g" "$m" "$v" "$(cat "$d/product" 2>/dev/null)"
    command -v getfacl >/dev/null && getfacl -p "$node" 2>/dev/null | grep '^user:[^:]' | sed 's/^/    acl /'
  done
  [ $found = 1 ] || echo "no known SDR on USB"
} >> "$OUT" 2>&1
# /lib is often a link to /usr/lib (merged /usr): search each real folder once.
RULE_DIRS="$(for r in /etc/udev/rules.d /usr/lib/udev/rules.d /lib/udev/rules.d; do [ -d "$r" ] && readlink -f "$r"; done | awk '!seen[$0]++' | tr '\n' ' ')"
run "for r in $RULE_DIRS; do grep -sHn -E '1d50|0bda|2cf0|1df7' \$r/*.rules; done | grep -Ei 'airspy|hackrf|rtl|60a1|6089|2838|2832|bladerf|iridium' | head -30"
run "getent group plugdev; svc_user=\$(systemctl show -p User --value iridium-sniffer 2>/dev/null); [ -n \"\$svc_user\" ] && echo \"service user: \$svc_user (\$(id -nG \$svc_user 2>/dev/null))\"; true"
note ""
note "iridium-sniffer --list (as $(id -un)). A radio the running service already"
note "holds open can show as missing or 'busy' here; that is expected."
run "'$SNIFFER_BIN' --list 2>&1 | head -30"

section "Services"
for u in iridium-sniffer.service observatory-trends.service observatory-rollup.service observatory-rollup.timer; do
  run "systemctl status $u --no-pager -n 0 2>&1 | head -12"
done
run "systemctl show iridium-sniffer -p NRestarts -p ActiveEnterTimestamp -p User -p SupplementaryGroups -p WorkingDirectory"
run "systemctl cat iridium-sniffer.service 2>&1 | grep -vE '^\s*#|^\s*$'"
run "systemctl cat observatory-trends.service 2>&1 | grep -E 'ExecStart|User|WorkingDirectory'"
run "systemctl list-timers --all --no-pager 2>/dev/null | grep -Ei 'observatory|NEXT'"

section "Ports"
run "ss -ltnp 2>/dev/null | grep -E ':(${WEB_PORT}|${OBS_PORT})\b' || echo 'nothing listening on :${WEB_PORT} or :${OBS_PORT}'"

section "Live endpoints"
# Fetch a JSON endpoint and summarise it with a little Python; say so plainly
# when it can't be reached instead of dumping a traceback.
endpoint() {
  local url="$1" py="$2" body
  printf '\n$ curl %s\n' "$url" >> "$OUT"
  if ! body="$(curl -fsS -m 20 "$url" 2>&1)"; then
    printf 'not reachable: %s\n' "$body" >> "$OUT"; return
  fi
  printf '%s' "$body" | python3 -c "$py" >> "$OUT" 2>&1 || printf '[could not parse the reply]\n' >> "$OUT"
}
if have curl && have python3; then
  run "curl -fsS -m 10 http://127.0.0.1:${OBS_PORT}/healthz"
  endpoint "http://127.0.0.1:${WEB_PORT}/api/state" '
import sys, json
d = json.load(sys.stdin)
print("counts:", {k: v for k, v in d.items() if k.startswith("total_") or k == "uw_ambiguous"})
print("frame types:", d.get("ftypes"))
print("sniffer location estimate:", d.get("rx"))
print("acars messages:", len(d.get("acars", [])), " aircraft:", len(d.get("aircraft", [])))'
  endpoint "http://127.0.0.1:${WEB_PORT}/api/doppler" '
import sys, json
d = json.load(sys.stdin)
print("enabled:", d.get("enabled"), d.get("note", ""))
if "wall_ms" in d: print("frame clock behind wall clock: %.1f s" % ((d["wall_ms"] - d["now_ms"]) / 1000))
else: print("no wall_ms/x/y/z: this sniffer predates the location feature")
for s in d.get("sats", []): print("  sat", s["sat"], "bursts", s["n"], "positions", "yes" if s.get("x") else "no")'
  endpoint "http://127.0.0.1:${OBS_PORT}/api/sky" '
import sys, json
d = json.load(sys.stdin)
for k in ("note", "feed_note", "tle", "ring_alerts", "clock_offset_s", "wall_lag_s", "fix", "sniffer_fix", "station", "names"):
    if d.get(k) not in (None, "", {}): print("%s: %s" % (k, d[k]))
print("satellites visible:", len(d.get("visible", [])), " passes:", len(d.get("passes", [])))'
else
  note "curl or python3 missing: endpoints not checked"
fi

section "Data"
for dir in "$DATA_DIR" "${ARCHIVE:-}" "$(dirname "${DB:-/nonexistent/x}")"; do
  [ -n "$dir" ] && [ -d "$dir" ] && run "ls -la '$dir' | tail -15"
done
for dir in "$DATA_DIR/tle" "$(dirname "${DB:-/nonexistent/x}")/tle" "$HOME/iridium-tle"; do
  [ -d "$dir" ] && run "ls -la '$dir'"
done
svc_home="$(getent passwd "$(systemctl show -p User --value iridium-sniffer 2>/dev/null || id -un)" | cut -d: -f6)"
run "ls -la '${svc_home:-$HOME}/.iridium-sniffer/' 2>/dev/null || echo 'no ~/.iridium-sniffer (aircraft database not downloaded; only needed for --basestation)'"

section "Logs"
note "If these are empty, add your user to the adm or systemd-journal group,"
note "or run: sudo journalctl -u iridium-sniffer -n 120"
run "journalctl -u iridium-sniffer -n 120 --no-pager 2>&1"
run "journalctl -u observatory-trends -n 40 --no-pager 2>&1"
run "journalctl -u observatory-rollup -n 20 --no-pager 2>&1"
run "dmesg 2>/dev/null | grep -Ei 'usb.*(1d50|0bda|airspy|hackrf|rtl)|usb.*(error|disconnect|reset)' | tail -20 || echo 'dmesg not readable as this user'"

echo
echo "Done: $OUT ($(wc -l < "$OUT") lines)"
echo "It contains your settings and estimated location; look it over before posting publicly."
