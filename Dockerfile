# SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#
# CI Dockerfile - copies source and builds in a single image.
# Used by GitHub Actions. For local development, use docker-compose.yml instead:
#
#   docker compose run --rm build-gcc     # Build with GCC
#   docker compose run --rm build-clang   # Build with Clang
#   docker compose run --rm test-full     # Build and test with all features
#   docker compose run --rm shell         # Interactive shell
#
FROM fedora:rawhide

RUN dnf install -y \
    gcc-c++ \
    clang \
    cmake \
    ninja-build \
    pkgconf-pkg-config \
    autoconf \
    automake \
    libtool \
    make \
    patch \
    libuuid-devel \
    git \
    && dnf clean all

# Fedora's packaged libfabric lags Loom's minimum. Build the same upstream
# release and provider fixes as the Nix flake.
COPY nix/pkgs/libfabric/*.patch /tmp/libfabric-patches/

RUN git clone --depth 1 --branch v2.7.0 https://github.com/ofiwg/libfabric.git /tmp/libfabric \
    && test "$(git -C /tmp/libfabric rev-parse HEAD)" = 9011331bb34bb90b38a7aad53857df4430c5db45 \
    && cd /tmp/libfabric \
    && for name in \
        name-server-cleanup \
        psm3-endpoint-cleanup \
        psm3-discovery-cleanup \
        sockets-probe-metadata \
        sockets-cm-metadata \
        sockets-unified-progress \
        sockets-receive-cleanup \
        srx-probe-metadata \
        discovery-copy-ownership \
        cuda-device-ordinal; do patch -p1 < "/tmp/libfabric-patches/$name.patch"; done \
    && ./autogen.sh \
    && ./configure --prefix=/usr/local --libdir=/usr/local/lib64 --disable-static \
    && make -j"$(nproc)" \
    && make install \
    && ldconfig \
    && rm -rf /tmp/libfabric

ENV PKG_CONFIG_PATH=/usr/local/lib64/pkgconfig

WORKDIR /build

COPY . /src

ARG CC=gcc
ARG CXX=g++
ARG BUILD_TYPE=Release
ARG USE_STDEXEC=OFF
ARG USE_ASIO=OFF

RUN cmake -S /src -B /build -G Ninja \
    -DCMAKE_C_COMPILER=${CC} \
    -DCMAKE_CXX_COMPILER=${CXX} \
    -DCMAKE_BUILD_TYPE=${BUILD_TYPE} \
    -DLOOM_BUILD_TESTS=ON \
    -DLOOM_USE_STDEXEC=${USE_STDEXEC} \
    -DLOOM_FETCHCONTENT_STDEXEC=${USE_STDEXEC} \
    -DLOOM_USE_ASIO=${USE_ASIO} \
    -DLOOM_FETCHCONTENT_ASIO=${USE_ASIO}

RUN cmake --build /build --parallel

CMD ["ctest", "--test-dir", "/build", "--output-on-failure"]
