#!/bin/bash
set -e

# Integration test: starts media driver, launches producer and consumer,
# verifies consumer exits cleanly with 0 failed validations.
# Use --perf to wrap both binaries with perf record -g.
# Requirements: 3.4, 4.4, 5.3

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${PROJECT_DIR}/build"

# Number of messages for the integration test (small for fast verification)
MESSAGE_COUNT="${MESSAGE_COUNT:-1000}"

# Parse script arguments
PERF_ENABLED=false
for arg in "$@"; do
    case "$arg" in
        --perf) PERF_ENABLED=true ;;
        *) echo "Unknown option: $arg" >&2; exit 1 ;;
    esac
done

# When profiling, use a larger message count for meaningful samples
if [ "$PERF_ENABLED" = true ] && [ "${MESSAGE_COUNT}" -lt 100000 ]; then
    MESSAGE_COUNT=1000000
    echo "Note: --perf enabled, using MESSAGE_COUNT=${MESSAGE_COUNT} for meaningful samples"
fi

# Paths to binaries
PRODUCER="${BUILD_DIR}/producer"
CONSUMER="${BUILD_DIR}/consumer"

# Find aeronmd binary in the build tree
if [ -f "${BUILD_DIR}/_deps/aeron-build/binaries/aeronmd" ]; then
    AERONMD="${BUILD_DIR}/_deps/aeron-build/binaries/aeronmd"
elif [ -f "${BUILD_DIR}/aeronmd" ]; then
    AERONMD="${BUILD_DIR}/aeronmd"
else
    AERONMD="$(find "${BUILD_DIR}" -name aeronmd -type f 2>/dev/null | head -1)"
fi

# Verify binaries exist
if [ ! -x "${PRODUCER}" ]; then
    echo "ERROR: Producer binary not found at ${PRODUCER}" >&2
    echo "       Run 'cmake --build build' first." >&2
    exit 1
fi

if [ ! -x "${CONSUMER}" ]; then
    echo "ERROR: Consumer binary not found at ${CONSUMER}" >&2
    echo "       Run 'cmake --build build' first." >&2
    exit 1
fi

if [ -z "${AERONMD}" ] || [ ! -f "${AERONMD}" ]; then
    echo "ERROR: Aeron media driver (aeronmd) not found in build directory" >&2
    echo "       Ensure BUILD_AERON_DRIVER is ON and run 'cmake --build build'." >&2
    exit 1
fi

if [ "$PERF_ENABLED" = true ] && ! command -v perf &>/dev/null; then
    echo "ERROR: 'perf' not found. Install linux-tools-$(uname -r) or equivalent." >&2
    exit 1
fi

# PIDs to track for cleanup
MEDIA_DRIVER_PID=""
CONSUMER_PID=""

# Cleanup function: kill background processes on exit
cleanup() {
    echo "Cleaning up..."
    if [ -n "${CONSUMER_PID}" ] && kill -0 "${CONSUMER_PID}" 2>/dev/null; then
        kill "${CONSUMER_PID}" 2>/dev/null || true
        wait "${CONSUMER_PID}" 2>/dev/null || true
    fi
    if [ -n "${MEDIA_DRIVER_PID}" ] && kill -0 "${MEDIA_DRIVER_PID}" 2>/dev/null; then
        kill "${MEDIA_DRIVER_PID}" 2>/dev/null || true
        wait "${MEDIA_DRIVER_PID}" 2>/dev/null || true
    fi
}

trap cleanup EXIT

echo "=== Aeron SBE PoC Integration Test ==="
echo "Message count: ${MESSAGE_COUNT}"
if [ "$PERF_ENABLED" = true ]; then
    echo "Profiling: ENABLED (perf record -g)"
fi
echo ""

# Clean up any stale media driver state
AERON_DIR="/dev/shm/aeron-$(whoami)"
if pkill -f aeronmd 2>/dev/null; then
    sleep 0.5
fi
if [ -d "${AERON_DIR}" ]; then
    rm -rf "${AERON_DIR}"
fi

# Step 1: Start the media driver in background
echo "[1/5] Starting Aeron media driver..."
"${AERONMD}" &
MEDIA_DRIVER_PID=$!
echo "       Media driver PID: ${MEDIA_DRIVER_PID}"

# Step 2: Wait for media driver readiness (requirement 5.3: ready within 2 seconds)
echo "[2/5] Waiting for media driver to be ready (2 seconds)..."
sleep 2

# Verify media driver is still running
if ! kill -0 "${MEDIA_DRIVER_PID}" 2>/dev/null; then
    echo "ERROR: Media driver exited unexpectedly" >&2
    exit 1
fi

# Step 3: Start consumer in background
echo "[3/5] Starting consumer (--messages ${MESSAGE_COUNT})..."
CONSUMER_OUTPUT=$(mktemp)
if [ "$PERF_ENABLED" = true ]; then
    perf record -g -o perf-consumer.data -- \
        "${CONSUMER}" --messages "${MESSAGE_COUNT}" --timeout 60 > "${CONSUMER_OUTPUT}" 2>&1 &
else
    "${CONSUMER}" --messages "${MESSAGE_COUNT}" --timeout 30 > "${CONSUMER_OUTPUT}" 2>&1 &
fi
CONSUMER_PID=$!

# Step 4: Start producer (runs in foreground)
echo "[4/5] Starting producer (--messages ${MESSAGE_COUNT})..."
if [ "$PERF_ENABLED" = true ]; then
    perf record -g -o perf-producer.data -- \
        "${PRODUCER}" --messages "${MESSAGE_COUNT}"
else
    "${PRODUCER}" --messages "${MESSAGE_COUNT}"
fi
PRODUCER_EXIT=$?

if [ ${PRODUCER_EXIT} -ne 0 ]; then
    echo "ERROR: Producer exited with code ${PRODUCER_EXIT}" >&2
    cat "${CONSUMER_OUTPUT}" 2>/dev/null
    rm -f "${CONSUMER_OUTPUT}"
    exit 1
fi

# Wait for consumer to complete
echo "       Waiting for consumer to finish..."
wait "${CONSUMER_PID}"
CONSUMER_EXIT=$?
CONSUMER_PID=""

echo ""
echo "[5/5] Checking results..."
echo ""

# Print consumer output
echo "--- Consumer Output ---"
cat "${CONSUMER_OUTPUT}"
echo "--- End Consumer Output ---"
echo ""

# Check consumer exit code
if [ ${CONSUMER_EXIT} -ne 0 ]; then
    echo "FAIL: Consumer exited with code ${CONSUMER_EXIT}" >&2
    rm -f "${CONSUMER_OUTPUT}"
    exit 1
fi

# Verify consumer reports 0 failed validations
if grep -q "Failed validations: 0" "${CONSUMER_OUTPUT}"; then
    echo "PASS: Consumer received all messages with 0 failed validations"
    rm -f "${CONSUMER_OUTPUT}"
else
    echo "FAIL: Consumer did not report 'Failed validations: 0'" >&2
    rm -f "${CONSUMER_OUTPUT}"
    exit 1
fi

# Print profiling instructions if perf was enabled
if [ "$PERF_ENABLED" = true ]; then
    echo ""
    echo "=== Perf Data Collected ==="
    echo "  Producer: perf-producer.data"
    echo "  Consumer: perf-consumer.data"
    echo ""
    echo "Analyze with:"
    echo "  perf report -i perf-producer.data"
    echo "  perf report -i perf-consumer.data"
fi
