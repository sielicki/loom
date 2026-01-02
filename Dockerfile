# SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
FROM fedora:rawhide

RUN dnf install -y \
    gcc-c++ \
    clang \
    cmake \
    ninja-build \
    pkgconf-pkg-config \
    libfabric-devel \
    git \
    && dnf clean all

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
