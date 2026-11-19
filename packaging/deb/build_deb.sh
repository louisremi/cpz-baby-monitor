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
    "$STAGE/usr/share/APPLaunch/share/images" "$STAGE/usr/share/doc/$PKG" "$STAGE/usr/lib/udev/rules.d"
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
# The status LEDs are root-owned and far too bright in a dark room: the app
# switches the activity LED off and leaves the power LED faint. This rule lets
# the desktop user (group gpio) write the two attributes; the postinst applies
# it to the LEDs that already exist, so no reboot is needed.
cat > "$STAGE/usr/lib/udev/rules.d/99-$PKG-leds.rules" <<'EOF'
# cpz-baby-monitor: allow the desktop user to dim the status LEDs at night.
SUBSYSTEM=="leds", KERNEL=="ACT", RUN+="/bin/chgrp gpio /sys$devpath/brightness /sys$devpath/trigger", RUN+="/bin/chmod g+w /sys$devpath/brightness /sys$devpath/trigger"
SUBSYSTEM=="leds", KERNEL=="default-on", RUN+="/bin/chgrp gpio /sys$devpath/brightness /sys$devpath/trigger", RUN+="/bin/chmod g+w /sys$devpath/brightness /sys$devpath/trigger"
EOF
cat > "$STAGE/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
if [ "$1" = "configure" ]; then
    udevadm control --reload-rules >/dev/null 2>&1 || true
    udevadm trigger --subsystem-match=leds --action=change >/dev/null 2>&1 || true
fi
exit 0
EOF
chmod 755 "$STAGE/DEBIAN/postinst"
cat > "$STAGE/DEBIAN/prerm" <<'EOF'
#!/bin/sh
set -e
# Leave the LEDs as the system expects them, not in night mode.
for led in ACT default-on; do
    [ -w /sys/class/leds/$led/brightness ] || continue
    echo 255 > /sys/class/leds/$led/brightness 2>/dev/null || true
done
exit 0
EOF
chmod 755 "$STAGE/DEBIAN/prerm"
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
