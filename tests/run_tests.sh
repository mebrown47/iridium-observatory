#!/usr/bin/env bash
# Copyright (c) 2026 Mike Brown
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Frame-decoder regression tests for iridium-sniffer.
#
#   Tier 1 (always; needs cc + python3):
#     - IIP CRC-24 reference vectors
#     - MSG encode->decode round-trip (incl. BCH error correction)
#     - LCW classification (ISY frame -> ft 7)
#
#   Tier 2 (optional; needs network + python3 `crcmod`):
#     - Cross-check the C MSG decoder against iridium-toolkit's iridium-parser.py
#       on identical bits. Skipped automatically if unavailable.
#
# Usage:  tests/run_tests.sh            # tier 1 (+ tier 2 if possible)
#         SKIP_ORACLE=1 tests/run_tests.sh   # tier 1 only
#
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
BUILD="$HERE/.build"
mkdir -p "$BUILD"

CC="${CC:-cc}"
PY="${PYTHON:-python3}"
PASS=0; FAIL=0
ok()   { echo "  PASS: $1"; PASS=$((PASS+1)); }
bad()  { echo "  FAIL: $1"; FAIL=$((FAIL+1)); }

echo "== building harnesses =="
$CC -O2 -o "$BUILD/test_crc"      "$HERE/test_crc.c" || { echo "build test_crc failed"; exit 2; }
$CC -O2 -I"$ROOT" -o "$BUILD/test_msg" \
    "$HERE/test_msg.c" "$ROOT/frame_decode.c" -lm || { echo "build test_msg failed"; exit 2; }
$CC -O2 -I"$ROOT" -o "$BUILD/test_classify" \
    "$HERE/test_classify.c" "$ROOT/ida_decode.c" "$ROOT/frame_decode.c" -lm \
    || { echo "build test_classify failed"; exit 2; }

# ---- Tier 1a: CRC-24 vectors ----
echo "== IIP CRC-24 vectors =="
if "$BUILD/test_crc"; then ok "CRC-24 vectors"; else bad "CRC-24 vectors"; fi

# ---- Tier 1b: MSG encode -> decode round-trip ----
echo "== MSG round-trip (encode -> C decode) =="
decode_txt() { "$BUILD/test_msg" "$1" | sed -n 's/.*TXT=\[//p' | sed 's/\]$//;s/ *$//'; }
RIC=4194303
while IFS='|' read -r ric txt; do
    [ -z "$ric" ] && continue
    bitstr="$("$PY" "$HERE/enc_msg.py" "$ric" "$txt" 2>/dev/null)"
    got="$(decode_txt "$bitstr")"
    want="$(printf '%s' "$txt" | sed 's/ *$//')"
    if [ "$got" = "$want" ]; then ok "ric=$ric [$txt]"; else bad "ric=$ric exp=[$want] got=[$got]"; fi
done <<'CASES'
42|Hi
100|ABC, def! @#$%
555|STATUS OK ALT 35000 SPD 480 HDG 270 WPT KORD
654321|The quick brown fox jumps over 0123456789
4194303|A
CASES

# ---- Tier 1c: BCH error correction (flip bits, still decode) ----
echo "== MSG BCH error correction =="
flip() { local s="$1" p="$2" c="${1:$2:1}"; [ "$c" = 0 ] && c=1 || c=0; printf '%s%s%s' "${s:0:$p}" "$c" "${s:$((p+1))}"; }
base="$("$PY" "$HERE/enc_msg.py" 12345 "BCH ERROR CORRECTION" 2>/dev/null)"
for spec in "1 flip:80" "2 spread:80 200" "2 close:80 96"; do
    name="${spec%%:*}"; pos="${spec#*:}"
    b="$base"; for p in $pos; do b="$(flip "$b" "$p")"; done
    got="$(decode_txt "$b")"
    if [ "$got" = "BCH ERROR CORRECTION" ]; then ok "$name corrected"; else bad "$name got=[$got]"; fi
done

# ---- Tier 1d: LCW classification (ISY vector from iridium-toolkit tests) ----
echo "== LCW classification (ISY -> ft 7) =="
ACCESS="001100000011000011110011"
ISY="0001000110111111000000100000001000100011000100"
SYNC="$(printf '01%.0s' $(seq 1 128))"
ft="$("$BUILD/test_classify" "${ACCESS}${ISY}${SYNC}" | sed -n 's/ft=//p')"
if [ "$ft" = 7 ]; then ok "ISY classified ft=7"; else bad "ISY ft=$ft (want 7)"; fi

# ---- Tier 2: oracle cross-check vs iridium-parser.py ----
if [ "${SKIP_ORACLE:-0}" != 1 ]; then
    echo "== oracle cross-check (vs iridium-parser.py) =="
    if ! "$PY" -c "import crcmod" 2>/dev/null; then
        echo "  SKIP: python module 'crcmod' not installed (pip install --user crcmod)"
    else
        REF="$BUILD/toolkit"; mkdir -p "$REF"
        base_url="https://raw.githubusercontent.com/muccc/iridium-toolkit/master"
        need=(iridium-parser.py bitsparser.py bch.py fec.py rs.py rs6.py reedsolo.py reedsolo6.py util.py itl.py)
        fetch_ok=1
        for f in "${need[@]}"; do
            [ -f "$REF/$f" ] && continue
            curl -fsSL -o "$REF/$f" "$base_url/$f" 2>/dev/null || fetch_ok=0
        done
        if [ "$fetch_ok" != 1 ]; then
            echo "  SKIP: could not fetch iridium-toolkit (no network?)"
        else
            for c in "42|Hi" "654321|cross check 12345 abcXYZ"; do
                ric="${c%%|*}"; txt="${c#*|}"
                bitstr="$("$PY" "$HERE/enc_msg.py" "$ric" "$txt" 2>/dev/null)"
                syms=$(( (${#bitstr} - 24) / 2 ))
                line="RAW: i-1-t1 0000000.0000 1626200000 N:10.00-80.00 I:00000000000 100% 0.10000 $syms $bitstr"
                pytxt="$(printf '%s\n' "$line" | (cd "$REF" && "$PY" iridium-parser.py 2>/dev/null) \
                         | sed -n 's/.*TXT: //p' | sed 's/ *+[0-9]* *$//;s/ *$//')"
                ctxt="$(decode_txt "$bitstr")"
                if [ -n "$pytxt" ] && [ "$pytxt" = "$ctxt" ]; then
                    ok "oracle agree ric=$ric [$ctxt]"
                else
                    bad "oracle ric=$ric py=[$pytxt] c=[$ctxt]"
                fi
            done
        fi
    fi
else
    echo "== oracle cross-check: SKIPPED (SKIP_ORACLE=1) =="
fi

echo "==================================="
echo "RESULT: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
