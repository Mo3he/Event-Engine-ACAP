#!/bin/sh
set -e

rm -rf build *.eap

build_arch() {
    ARCH=$1
    echo "=== Building $ARCH ==="
    docker build --progress=plain --no-cache --build-arg ARCH="$ARCH" --tag "acap_event_engine_$ARCH" .
    CONTAINER_ID=$(docker create "acap_event_engine_$ARCH")
    docker cp "$CONTAINER_ID":/opt/app ./build
    docker rm "$CONTAINER_ID"
    mv build/*.eap .
    rm -rf build
}

build_arch aarch64
build_arch armv7hf

echo "=== Done ==="
ls -lh *.eap
