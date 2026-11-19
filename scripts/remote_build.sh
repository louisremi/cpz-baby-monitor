#!/usr/bin/env bash
# Build on the Docker host (nasbrico) in the trixie cross-build image.
#   scripts/remote_build.sh test     host unit tests (ctest)
#   scripts/remote_build.sh device   arm64 binary -> dist/
#   scripts/remote_build.sh deb      arm64 binary + .deb -> dist/
#   scripts/remote_build.sh all      test + deb
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HOST="${CPZBM_BUILD_HOST:-192.168.1.60}"
RDIR="${CPZBM_BUILD_DIR:-/root/cpz-baby-monitor-build}"
IMAGE=cpzbm-build
what="${1:-all}"
SSH="ssh -o BatchMode=yes -o LogLevel=ERROR $HOST"

rsync -a --delete --exclude build/ --exclude dist/ --exclude .tmp/ --exclude .git/ \
    -e "ssh -o BatchMode=yes -o LogLevel=ERROR" "$ROOT/" "$HOST:$RDIR/"
$SSH "docker image inspect $IMAGE >/dev/null 2>&1 || docker build -q -t $IMAGE $RDIR/packaging/docker"

run() { $SSH "docker run --rm -v $RDIR:/src -w /src $IMAGE bash -c '$1'"; }

if [[ $what == test || $what == all ]]; then
    run "cmake -S . -B build/host -G Ninja -DCPZBM_DEVICE=OFF -DBUILD_TESTING=ON >/dev/null && cmake --build build/host && ctest --test-dir build/host --output-on-failure"
fi
if [[ $what == device || $what == deb || $what == all ]]; then
    run "cmake -S . -B build/arm64 -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-linux-gnu.cmake -DCPZBM_DEVICE=ON >/dev/null && cmake --build build/arm64 2>&1 | grep -E \"warning|error|FAILED\" | grep -v \"dependencies/\" ; test -x build/arm64/cpz-baby-monitor && mkdir -p dist && cp build/arm64/cpz-baby-monitor dist/ && aarch64-linux-gnu-strip dist/cpz-baby-monitor && file dist/cpz-baby-monitor"
fi
if [[ $what == deb || $what == all ]]; then
    run "packaging/deb/build_deb.sh dist/cpz-baby-monitor dist"
fi
mkdir -p "$ROOT/dist"
rsync -a -e "ssh -o BatchMode=yes -o LogLevel=ERROR" "$HOST:$RDIR/dist/" "$ROOT/dist/" 2>/dev/null || true
