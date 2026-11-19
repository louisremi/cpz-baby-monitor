# Build image mirroring the CardputerZero OS (Debian 13 trixie):
#  - native amd64 toolchain + libs for host unit tests
#  - arm64 cross toolchain + arm64 dev libs (multiarch) for the device binary
FROM debian:trixie

ARG DEBIAN_FRONTEND=noninteractive
RUN dpkg --add-architecture arm64 \
    && apt-get update \
    && apt-get install --yes --no-install-recommends \
        build-essential crossbuild-essential-arm64 cmake ninja-build pkg-config \
        ca-certificates git dpkg-dev file binutils-aarch64-linux-gnu \
        libsodium-dev libcodec2-dev \
        libsodium-dev:arm64 libcodec2-dev:arm64 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
