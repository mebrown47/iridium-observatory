#!/usr/bin/env bash
# Copyright (c) 2026 Mike Brown
# SPDX-License-Identifier: GPL-3.0-or-later
# preflight_binary_check.sh — will this prebuilt iridium-sniffer run on THIS machine?
# Usage: ./preflight_binary_check.sh [path-to-binary]   (default: ./iridium-sniffer)
# Exit 0 = good to run; exit 1 = something's missing (details printed).

set -u
BIN="${1:-./iridium-sniffer}"
NEED_GLIBC="2.38"   # highest GLIBC_x.y this binary imports
NEED_ARCH="x86-64"  # from 'file' output

fail=0
say()  { printf '%s\n' "$*"; }
ok()   { printf '  \033[32mOK\033[0m   %s\n' "$*"; }
bad()  { printf '  \033[31mFAIL\033[0m %s\n' "$*"; fail=1; }

[ -f "$BIN" ] || { say "No binary at '$BIN' — pass its path as arg 1."; exit 1; }
say "Checking: $BIN"
say ""

# 1) Architecture
say "[1] CPU architecture"
if file "$BIN" | grep -q "$NEED_ARCH"; then
  ok "binary is $NEED_ARCH and this host is $(uname -m)"
else
  bad "binary arch mismatch — this host is $(uname -m); rebuild required (no drop-in)"
fi
say ""

# 2) glibc version (binary needs >= NEED_GLIBC)
say "[2] glibc version (need >= $NEED_GLIBC)"
have_glibc="$(ldd --version 2>/dev/null | head -1 | grep -oE '[0-9]+\.[0-9]+' | head -1)"
if [ -n "$have_glibc" ] && \
   [ "$(printf '%s\n%s\n' "$NEED_GLIBC" "$have_glibc" | sort -V | head -1)" = "$NEED_GLIBC" ]; then
  ok "host glibc $have_glibc >= $NEED_GLIBC"
else
  bad "host glibc ${have_glibc:-unknown} < $NEED_GLIBC — rebuild on this box"
fi
say ""

# 3) Shared libraries — any 'not found' is a blocker
say "[3] Shared libraries"
missing="$(ldd "$BIN" 2>/dev/null | grep 'not found')"
if [ -z "$missing" ]; then
  ok "all shared libraries resolve"
else
  bad "missing libraries:"
  printf '%s\n' "$missing" | sed 's/^/         /'
  printf '%s\n' "$missing" | grep -qi 'libacars' && \
    say "         -> libacars powers the AT1/ADS-C tab. Build it once, or copy" \
    && say "            libacars-2.so.2* into /usr/local/lib and run 'sudo ldconfig'."
fi
say ""

# Verdict
if [ "$fail" -eq 0 ]; then
  say "RESULT: good to run. Try:  $BIN --help"
  exit 0
else
  say "RESULT: not runnable as-is — see FAIL lines above."
  exit 1
fi
