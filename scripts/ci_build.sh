#!/usr/bin/env bash
# Test and build the CardputerZero package inside the build container.
# CI runs this via `docker run ... cpzbm-build bash scripts/ci_build.sh`;
# dependencies/ must already be fetched (scripts/fetch_deps.sh).
#
# Env: CPZBM_VERSION_OVERRIDE  version from the release tag (default: CMakeLists)
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
VERSION="${CPZBM_VERSION_OVERRIDE:-}"
[[ -z "${VERSION}" ]] || [[ "${VERSION}" =~ ^[0-9]+\.[0-9]+\.[0-9]+ ]] || {
    echo "CPZBM_VERSION_OVERRIDE must be X.Y.Z, got '${VERSION}'" >&2
    exit 1
}
VERSION_ARG=()
[[ -z "${VERSION}" ]] || VERSION_ARG=(-DCPZBM_VERSION="${VERSION}")

echo "== host unit tests"
cmake -S . -B build/host -G Ninja -DCPZBM_DEVICE=OFF -DBUILD_TESTING=ON
cmake --build build/host
ctest --test-dir build/host --output-on-failure

echo "== arm64 device build"
cmake -S . -B build/arm64 -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-linux-gnu.cmake -DCPZBM_DEVICE=ON "${VERSION_ARG[@]}"
cmake --build build/arm64

echo "== package"
mkdir -p dist
cp build/arm64/cpz-baby-monitor dist/cpz-baby-monitor
aarch64-linux-gnu-strip dist/cpz-baby-monitor
file dist/cpz-baby-monitor | grep "ARM aarch64" >/dev/null || { echo "not an arm64 binary" >&2; exit 1; }
# The package links both codecs and crypto dynamically: their package names are
# in Depends, so check the binary really needs them (a silent static fallback
# would change the licence obligations).
for lib in libcodec2.so.1.2 libsodium.so.23; do
    aarch64-linux-gnu-readelf -d dist/cpz-baby-monitor | grep "${lib}" >/dev/null || {
        echo "missing dynamic dependency: ${lib}" >&2
        exit 1
    }
done
packaging/deb/build_deb.sh dist/cpz-baby-monitor dist "${VERSION}"
ls -l dist
( cd dist && sha256sum ./*.deb > SHA256SUMS && cat SHA256SUMS )
