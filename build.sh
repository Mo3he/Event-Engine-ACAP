#!/bin/bash
set -e
trap 'echo "Build failed at line $LINENO"; exit 1' ERR

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

TARGET=${1:-all}
case "$TARGET" in
    aarch64)  build_arch aarch64 ;;
    armv7hf)  build_arch armv7hf ;;
    all)
        build_arch aarch64
        build_arch armv7hf
        ;;
    *)
        echo "Usage: $0 [aarch64|armv7hf|all]"
        exit 1
        ;;
esac

echo "=== Done ==="
ls -lh *.eap
