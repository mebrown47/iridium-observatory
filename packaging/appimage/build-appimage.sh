#!/usr/bin/env bash
# Copyright (c) 2026 Mike Brown
# SPDX-License-Identifier: GPL-3.0-or-later
#
# build-appimage.sh — package iridium-observatory as an x86_64 AppImage
# (prototype: RTL-SDR, Airspy and HackRF only; no GPU).
#
# Build on the OLDEST system it should run on (Ubuntu 22.04): it bundles
# that system's libraries and runs on anything with the same glibc or newer.
#
# Needs: the build tools, libhackrf-dev, libairspy-dev, librtlsdr-dev,
# libfftw3-dev, libusb-1.0-0-dev, libacars-2, and SoapySDR + SoapyRTLSDR
# (built into SOAPY_PREFIX, or installed). Run from the repo checkout:
#
#     SOAPY_PREFIX=~/appimage-build/prefix packaging/appimage/build-appimage.sh
#
# Writes iridium-observatory-<version>-x86_64.AppImage in the current folder.
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
WORK="${WORK:-$REPO/build-appimage-work}"
SOAPY_PREFIX="${SOAPY_PREFIX:-/usr}"
BUILD="$WORK/build"
APPDIR="$WORK/AppDir"
TOOLS="$WORK/tools"
VERSION="$(git -C "$REPO" describe --always --dirty --tags 2>/dev/null || echo unknown)"
export APPIMAGE_EXTRACT_AND_RUN=1      # run the tools without FUSE

mkdir -p "$WORK" "$TOOLS"

# ---- build the receiver --------------------------------------------------------
PKG_CONFIG_PATH="$SOAPY_PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}" \
cmake -S "$REPO" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release \
      -DUSE_OPENCL=OFF -DUSE_VULKAN=OFF -DSOAPYSDR_DIR="$SOAPY_PREFIX" > "$WORK/cmake.log"
for want in "HackRF: enabled" "Airspy: enabled" "SoapySDR: enabled" "libacars: enabled"; do
  grep -q "$want" "$WORK/cmake.log" || { cat "$WORK/cmake.log"; echo "missing: $want" >&2; exit 1; }
done
cmake --build "$BUILD" -j"$(nproc)" > "$WORK/build.log"

# ---- lay out the AppDir --------------------------------------------------------
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/data" "$APPDIR/usr/lib/SoapySDR/modules0.8" \
         "$APPDIR/usr/share/iridium-observatory/observatory" "$APPDIR/usr/share/doc/iridium-observatory"
cp "$BUILD/iridium-sniffer" "$APPDIR/usr/bin/"
cp "$REPO/data/waypoints.csv" "$APPDIR/usr/data/"     # found at <exe>/../data
cp "$REPO"/observatory/*.py "$APPDIR/usr/share/iridium-observatory/observatory/"
cp -r "$REPO/observatory/sgp4" "$APPDIR/usr/share/iridium-observatory/observatory/"
find "$APPDIR" -name __pycache__ -prune -exec rm -rf {} +
MODULE="$(find "$SOAPY_PREFIX" -path '*SoapySDR/modules0.8/librtlsdrSupport.so' | head -1)"
[ -n "$MODULE" ] || { echo "SoapyRTLSDR module not found under $SOAPY_PREFIX" >&2; exit 1; }
cp "$MODULE" "$APPDIR/usr/lib/SoapySDR/modules0.8/"
cp "$REPO/LICENSE" "$REPO/README.md" "$REPO/QUICKSTART.md" "$APPDIR/usr/share/doc/iridium-observatory/"
echo "$VERSION" > "$APPDIR/VERSION"

# ---- tools ---------------------------------------------------------------------
fetch() { [ -x "$TOOLS/$1" ] || { curl -fsSL -o "$TOOLS/$1" "$2"; chmod +x "$TOOLS/$1"; }; }
fetch linuxdeploy-x86_64.AppImage \
  https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
fetch appimagetool-x86_64.AppImage \
  https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage

# ---- bundle libraries: the receiver's, and the RTL-SDR module's ----------------
LD_LIBRARY_PATH="$SOAPY_PREFIX/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
"$TOOLS/linuxdeploy-x86_64.AppImage" --appdir "$APPDIR" \
  --executable "$APPDIR/usr/bin/iridium-sniffer" \
  --deploy-deps-only "$APPDIR/usr/lib/SoapySDR/modules0.8/librtlsdrSupport.so" \
  --desktop-file "$HERE/iridium-observatory.desktop" \
  --icon-file "$HERE/iridium-observatory.svg" \
  --custom-apprun "$HERE/AppRun" > "$WORK/linuxdeploy.log"

# ---- licences of what we bundle --------------------------------------------------
for p in libhackrf0 libairspy0 librtlsdr0 libfftw3-single3 libusb-1.0-0; do
  [ -f "/usr/share/doc/$p/copyright" ] && cp "/usr/share/doc/$p/copyright" "$APPDIR/usr/share/doc/iridium-observatory/copyright.$p"
done
for f in "$REPO"/observatory/sgp4/LICENSE; do cp "$f" "$APPDIR/usr/share/doc/iridium-observatory/LICENSE.python-sgp4"; done

# ---- pack ----------------------------------------------------------------------
OUT="$PWD/iridium-observatory-$VERSION-x86_64.AppImage"
ARCH=x86_64 "$TOOLS/appimagetool-x86_64.AppImage" "$APPDIR" "$OUT" > "$WORK/appimagetool.log" 2>&1 \
  || { tail -20 "$WORK/appimagetool.log"; exit 1; }
ls -la "$OUT"
