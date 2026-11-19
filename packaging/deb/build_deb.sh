#!/usr/bin/env bash
# Stage and build the arm64 .deb.
# Usage: build_deb.sh <binary> <outdir> [version]   (version also via $VERSION)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BIN="${1:?binary}"
OUT="${2:?outdir}"
PKG=cpz-baby-monitor
VERSION="${3:-${VERSION:-}}"
if [[ -z "${VERSION}" ]]; then
    VERSION="$(sed -n 's/^project(cpz-baby-monitor VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
fi
[[ "${VERSION}" =~ ^[0-9]+\.[0-9]+\.[0-9]+ ]] || { echo "invalid version: ${VERSION}" >&2; exit 1; }
REV=1
STAGE="$ROOT/build/deb/${PKG}"

file "$BIN" | grep -q "ARM aarch64" || { echo "not an arm64 binary: $BIN" >&2; exit 1; }
rm -rf -- "${STAGE:?}"
install -d "$STAGE/DEBIAN" "$STAGE/usr/share/$PKG/bin" "$STAGE/usr/share/APPLaunch/applications" \
    "$STAGE/usr/share/APPLaunch/share/images" "$STAGE/usr/share/doc/$PKG"
install -m 755 "$BIN" "$STAGE/usr/share/$PKG/bin/$PKG"
install -m 644 "$ROOT/packaging/deb/images/$PKG.png" "$STAGE/usr/share/APPLaunch/share/images/$PKG.png"
install -m 644 "$ROOT/LICENSE" "$STAGE/usr/share/doc/$PKG/copyright"
install -m 644 "$ROOT/THIRD_PARTY_NOTICES.md" "$STAGE/usr/share/doc/$PKG/THIRD_PARTY_NOTICES.md"
cat > "$STAGE/usr/share/APPLaunch/applications/$PKG.desktop" <<EOF
[Desktop Entry]
Name=BabyMon
Exec=/usr/share/$PKG/bin/$PKG
Terminal=false
Icon=share/images/$PKG.png
Type=Application
EOF
# The Cap LoRa package ships the udev rule that lets the gpio group switch
# the Cap's 5 V rail; codec2/sodium are linked dynamically.
cat > "$STAGE/DEBIAN/control" <<EOF
Package: $PKG
Version: $VERSION-$REV
Architecture: arm64
Maintainer: louisremi <pro@louisremi.com>
Section: APPLaunch
Priority: optional
Depends: libc6 (>= 2.38), libstdc++6, libcodec2-1.2, libsodium23, libpulse0, m5cardputerzero-cap-lora-1262
Description: LoRa baby monitor for CardputerZero
 Pairs two CardputerZero devices with Cap LoRa-1262 using a 6-digit code.
 The baby unit streams Codec2 audio over encrypted LoRa (US915 or EU868); the
 parent unit plays it and shows a live waveform.
EOF
mkdir -p "$OUT"
dpkg-deb --root-owner-group -Zxz -b "$STAGE" "$OUT/${PKG}_${VERSION}-${REV}_arm64.deb" >/dev/null
dpkg-deb -I "$OUT/${PKG}_${VERSION}-${REV}_arm64.deb" | sed -n '/Package/,$p'
dpkg-deb -c "$OUT/${PKG}_${VERSION}-${REV}_arm64.deb" | awk '{print $6}'
