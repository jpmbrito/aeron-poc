#!/bin/bash
set -e

# start_media_driver.sh — Launch the Aeron media driver in IPC mode (shared memory)
# The driver uses /dev/shm by default for IPC communication.
# Prints the media driver PID to stdout so it can be terminated after use.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

# Allow overriding the build directory via BUILD_DIR env variable
BUILD_DIR="${BUILD_DIR:-${PROJECT_DIR}/build}"

AERONMD="${BUILD_DIR}/_deps/aeron-build/binaries/aeronmd"

if [ ! -x "${AERONMD}" ]; then
    echo "ERROR: aeronmd not found at ${AERONMD}" >&2
    echo "Make sure the project is built with BUILD_AERON_DRIVER=ON:" >&2
    echo "  cmake -B build && cmake --build build" >&2
    exit 1
fi

# Determine the Aeron directory (default: /dev/shm/aeron-$USER)
AERON_DIR="${AERON_DIR:-/dev/shm/aeron-$(whoami)}"

# Kill any existing media driver and clean up stale shared memory
if pkill -f aeronmd 2>/dev/null; then
    sleep 0.5
fi
if [ -d "${AERON_DIR}" ]; then
    rm -rf "${AERON_DIR}"
fi

# Launch the media driver in the background.
# Aeron uses /dev/shm by default for shared-memory IPC — no extra flags needed.
"${AERONMD}" &
DRIVER_PID=$!

echo "${DRIVER_PID}"
