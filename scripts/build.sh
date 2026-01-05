#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#
# Local build script using podman/docker
# Usage:
#   ./scripts/build.sh              # Build with defaults (clang, Release)
#   ./scripts/build.sh gcc          # Build with GCC
#   ./scripts/build.sh clang        # Build with Clang
#   ./scripts/build.sh stdexec      # Build with stdexec
#   ./scripts/build.sh asio         # Build with Asio
#   ./scripts/build.sh full         # Build with all features
#   ./scripts/build.sh shell        # Interactive shell
#   ./scripts/build.sh clean        # Remove build volume

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

# Use podman if available, otherwise docker
if command -v podman &> /dev/null; then
    CONTAINER_CMD=podman
elif command -v docker &> /dev/null; then
    CONTAINER_CMD=docker
else
    echo "Error: neither podman nor docker found" >&2
    exit 1
fi

IMAGE_NAME="loom-dev"
VOLUME_NAME="loom-build-cache"

# Build the base image if needed
build_image() {
    echo "Building base image..."
    $CONTAINER_CMD build -t "$IMAGE_NAME" -f "$PROJECT_DIR/docker/Dockerfile.base" "$PROJECT_DIR"
}

# Ensure image exists
ensure_image() {
    if ! $CONTAINER_CMD image exists "$IMAGE_NAME" 2>/dev/null; then
        build_image
    fi
}

# Run a build with specified cmake args
run_build() {
    local cmake_args="$1"
    ensure_image

    $CONTAINER_CMD run --rm -it \
        -v "$PROJECT_DIR:/src:ro" \
        -v "$VOLUME_NAME:/build" \
        -w /build \
        "$IMAGE_NAME" \
        sh -c "cmake -S /src -B /build -G Ninja $cmake_args && cmake --build /build --parallel"
}

# Run tests
run_test() {
    local cmake_args="$1"
    ensure_image

    $CONTAINER_CMD run --rm -it \
        -v "$PROJECT_DIR:/src:ro" \
        -v "$VOLUME_NAME:/build" \
        -w /build \
        "$IMAGE_NAME" \
        sh -c "cmake -S /src -B /build -G Ninja $cmake_args && cmake --build /build --parallel && ctest --test-dir /build --output-on-failure"
}

# Interactive shell
run_shell() {
    ensure_image

    $CONTAINER_CMD run --rm -it \
        -v "$PROJECT_DIR:/src:ro" \
        -v "$VOLUME_NAME:/build" \
        -w /build \
        "$IMAGE_NAME" \
        /bin/bash
}

# Clean build cache
clean() {
    echo "Removing build cache volume..."
    $CONTAINER_CMD volume rm "$VOLUME_NAME" 2>/dev/null || true
}

# Common cmake args
BASE_ARGS="-DLOOM_BUILD_TESTS=ON"
GCC_ARGS="$BASE_ARGS -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ -DCMAKE_BUILD_TYPE=Release"
CLANG_ARGS="$BASE_ARGS -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release"
STDEXEC_ARGS="$CLANG_ARGS -DLOOM_USE_STDEXEC=ON -DLOOM_FETCHCONTENT_STDEXEC=ON"
ASIO_ARGS="$CLANG_ARGS -DLOOM_USE_ASIO=ON -DLOOM_FETCHCONTENT_ASIO=ON"
FULL_ARGS="$CLANG_ARGS -DLOOM_USE_STDEXEC=ON -DLOOM_FETCHCONTENT_STDEXEC=ON -DLOOM_USE_ASIO=ON -DLOOM_FETCHCONTENT_ASIO=ON"

case "${1:-clang}" in
    gcc)
        run_build "$GCC_ARGS"
        ;;
    clang)
        run_build "$CLANG_ARGS"
        ;;
    stdexec)
        run_build "$STDEXEC_ARGS"
        ;;
    asio)
        run_build "$ASIO_ARGS"
        ;;
    full)
        run_build "$FULL_ARGS"
        ;;
    test-gcc)
        run_test "$GCC_ARGS"
        ;;
    test-clang)
        run_test "$CLANG_ARGS"
        ;;
    test-full)
        run_test "$FULL_ARGS"
        ;;
    shell)
        run_shell
        ;;
    clean)
        clean
        ;;
    rebuild-image)
        build_image
        ;;
    *)
        echo "Usage: $0 {gcc|clang|stdexec|asio|full|test-gcc|test-clang|test-full|shell|clean|rebuild-image}"
        exit 1
        ;;
esac
