#!/usr/bin/env bash
# Install the built .deb on one or more CardputerZero devices and restart the
# launcher so the BabyMon entry appears.   scripts/deploy.sh <ip> [ip...]
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEB="$(ls -t "$ROOT"/dist/cpz-baby-monitor_*_arm64.deb | head -1)"
SSH_OPTS=(-o BatchMode=yes -o LogLevel=ERROR)
for ip in "$@"; do
    echo "== $ip: $(basename "$DEB")"
    scp -q "${SSH_OPTS[@]}" "$DEB" "root@$ip:/tmp/cpzbm.deb"
    ssh "${SSH_OPTS[@]}" "root@$ip" '
        pkill -INT -x cpz-baby-monitor 2>/dev/null; sleep 1
        dpkg -i /tmp/cpzbm.deb >/dev/null && rm -f /tmp/cpzbm.deb
        dpkg -s cpz-baby-monitor | grep -E "^(Status|Version)"
        uid=$(id -u louisremi)
        XDG_RUNTIME_DIR=/run/user/$uid systemctl --user -M louisremi@ restart APPLaunch.service 2>/dev/null \
            || su louisremi -c "XDG_RUNTIME_DIR=/run/user/$uid systemctl --user restart APPLaunch.service"
        echo "APPLaunch restarted"'
done
