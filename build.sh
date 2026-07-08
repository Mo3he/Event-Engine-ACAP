#!/bin/bash
set -e
trap 'echo "Build failed at line $LINENO"; exit 1' ERR

# Honor an explicit RUNTIME override (RUNTIME=docker|podman); otherwise
# auto-detect, preferring podman when its daemon is reachable.
if [ -n "${RUNTIME:-}" ]; then
	CONTAINER_CMD="$RUNTIME"
elif command -v podman &>/dev/null && podman info &>/dev/null 2>&1; then
	CONTAINER_CMD=podman
elif command -v docker &>/dev/null && docker info &>/dev/null 2>&1; then
	CONTAINER_CMD=docker
else
	echo "ERROR: Neither podman nor docker is available/running."
	exit 1
fi
echo "Using: $CONTAINER_CMD"

rm -rf build *.eap

build_arch() {
	ARCH=$1
	echo "=== Building $ARCH ==="
	$CONTAINER_CMD build --progress=plain --no-cache --build-arg ARCH="$ARCH" --tag "acap_event_engine_$ARCH" .
	CONTAINER_ID=$($CONTAINER_CMD create "acap_event_engine_$ARCH")
	$CONTAINER_CMD cp "$CONTAINER_ID":/opt/app ./build
	$CONTAINER_CMD rm "$CONTAINER_ID" || true
	mv build/*.eap .
	rm -rf build
}

TARGET=${1:-all}
case "$TARGET" in
aarch64) build_arch aarch64 ;;
armv7hf) build_arch armv7hf ;;
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
