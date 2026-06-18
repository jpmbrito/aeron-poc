#!/bin/bash
set -e

# Integration test: exercises the full Aeron Archive record-replay-verify cycle.
# 1. Starts Archive Media Driver
# 2. Starts Recorder in record mode, captures recording-id
# 3. Runs Producer (through archive driver) with configurable message count
# 4. Stops Recorder with SIGINT
# 5. Replays the recording with --verify
# 6. Verifies replay count matches and 0 failed verifications
# Requirements: 5.1, 5.2, 5.3, 5.4, 5.5

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${PROJECT_DIR}/build"

# Configurable message count (default 1000)
MESSAGE_COUNT="${MESSAGE_COUNT:-1000}"

# Parse script arguments
for arg in "$@"; do
    case "$arg" in
        --messages)
            shift_next=true
            ;;
        *)
            if [ "${shift_next}" = true ]; then
                MESSAGE_COUNT="$arg"
                shift_next=false
            else
                echo "Unknown option: $arg" >&2
                echo "Usage: $0 [--messages <count>]" >&2
                exit 1
            fi
            ;;
    esac
done

# Paths to binaries
PRODUCER="${BUILD_DIR}/producer"
RECORDER="${BUILD_DIR}/recorder"

# Archive driver Aeron directory (must match what start_archive_media_driver.sh uses)
ARCHIVE_AERON_DIR="/dev/shm/aeron-archive-$(whoami)"

# Verify binaries exist
if [ ! -x "${PRODUCER}" ]; then
    echo "ERROR: Producer binary not found at ${PRODUCER}" >&2
    echo "       Run 'cmake --build build' first." >&2
    exit 1
fi

if [ ! -x "${RECORDER}" ]; then
    echo "ERROR: Recorder binary not found at ${RECORDER}" >&2
    echo "       Run 'cmake --build build' first." >&2
    exit 1
fi

# PIDs to track for cleanup
ARCHIVE_DRIVER_PID=""
RECORDER_PID=""

# Cleanup function: kill background processes on exit
cleanup() {
    if [ -n "${RECORDER_PID}" ] && kill -0 "${RECORDER_PID}" 2>/dev/null; then
        kill "${RECORDER_PID}" 2>/dev/null || true
        wait "${RECORDER_PID}" 2>/dev/null || true
    fi
    if [ -n "${ARCHIVE_DRIVER_PID}" ] && kill -0 "${ARCHIVE_DRIVER_PID}" 2>/dev/null; then
        kill "${ARCHIVE_DRIVER_PID}" 2>/dev/null || true
        wait "${ARCHIVE_DRIVER_PID}" 2>/dev/null || true
    fi
    # Clean up temp files
    rm -f "${RECORDER_OUTPUT}" "${REPLAY_OUTPUT}" 2>/dev/null || true
}

trap cleanup EXIT

echo "=== Aeron Archive Integration Test ==="
echo "Message count: ${MESSAGE_COUNT}"
echo ""

# Step 1: Start Archive Media Driver
echo "[1/6] Starting Archive Media Driver..."
ARCHIVE_DRIVER_PID=$("${SCRIPT_DIR}/start_archive_media_driver.sh")
if [ -z "${ARCHIVE_DRIVER_PID}" ]; then
    echo "ERROR: Failed to start Archive Media Driver" >&2
    exit 1
fi
echo "       Archive Media Driver PID: ${ARCHIVE_DRIVER_PID}"

# Wait for archive driver to be ready (Java driver needs more time than C driver)
sleep 4

if ! kill -0 "${ARCHIVE_DRIVER_PID}" 2>/dev/null; then
    echo "ERROR: Archive Media Driver exited unexpectedly" >&2
    exit 1
fi

# Step 2: Start Recorder in record mode (background), capture recording-id
echo "[2/6] Starting Recorder in record mode..."
RECORDER_OUTPUT=$(mktemp)
"${RECORDER}" --record > "${RECORDER_OUTPUT}" 2>&1 &
RECORDER_PID=$!

# Wait for the recorder to print the recording-id (up to 15 seconds)
RECORDING_ID=""
WAIT_DEADLINE=$((SECONDS + 15))
while [ ${SECONDS} -lt ${WAIT_DEADLINE} ]; do
    if ! kill -0 "${RECORDER_PID}" 2>/dev/null; then
        echo "ERROR: Recorder exited unexpectedly during record mode" >&2
        echo "--- Recorder Output ---" >&2
        cat "${RECORDER_OUTPUT}" >&2
        exit 1
    fi
    if grep -q "^recording-id:" "${RECORDER_OUTPUT}" 2>/dev/null; then
        RECORDING_ID="ready"
        break
    fi
    sleep 0.2
done

if [ -z "${RECORDING_ID}" ]; then
    echo "ERROR: Timed out waiting for recording-id from Recorder" >&2
    echo "--- Recorder Output ---" >&2
    cat "${RECORDER_OUTPUT}" >&2
    exit 1
fi
echo "       Recorder is ready for recording."

# Step 3: Run Producer with messages through the archive driver's Aeron directory
echo "[3/6] Running Producer (--messages ${MESSAGE_COUNT})..."
AERON_DIR="${ARCHIVE_AERON_DIR}" "${PRODUCER}" --messages "${MESSAGE_COUNT}"
PRODUCER_EXIT=$?

if [ ${PRODUCER_EXIT} -ne 0 ]; then
    echo "ERROR: Producer exited with code ${PRODUCER_EXIT}" >&2
    exit 1
fi
echo "       Producer completed successfully."

# Step 4: Stop Recorder with SIGINT
echo "[4/6] Stopping Recorder (SIGINT)..."
kill -INT "${RECORDER_PID}" 2>/dev/null || true
wait "${RECORDER_PID}" 2>/dev/null || true
RECORDER_EXIT=$?
RECORDER_PID=""

if [ ${RECORDER_EXIT} -ne 0 ]; then
    echo "ERROR: Recorder exited with code ${RECORDER_EXIT} after SIGINT" >&2
    echo "--- Recorder Output ---" >&2
    cat "${RECORDER_OUTPUT}" >&2
    exit 1
fi
echo "       Recorder stopped cleanly."

# Extract the actual numeric recording ID from recorder output
RECORDING_ID=$(grep "^recording-id: [0-9]" "${RECORDER_OUTPUT}" | tail -1 | awk '{print $2}')
if [ -z "${RECORDING_ID}" ]; then
    # No recording ID found — might be 0 by default for first recording
    RECORDING_ID="0"
    echo "       No recording ID printed by recorder, using default: ${RECORDING_ID}"
else
    echo "       Recording ID: ${RECORDING_ID}"
fi

# Give archive a moment to flush segments to disk
sleep 1

# Step 5: Start Recorder in replay mode with --verify
echo "[5/6] Replaying recording ${RECORDING_ID} with --verify..."
REPLAY_OUTPUT=$(mktemp)
"${RECORDER}" --replay --recording-id "${RECORDING_ID}" --verify > "${REPLAY_OUTPUT}" 2>&1
REPLAY_EXIT=$?

if [ ${REPLAY_EXIT} -ne 0 ]; then
    echo "ERROR: Replay exited with code ${REPLAY_EXIT}" >&2
    echo "--- Replay Output ---" >&2
    cat "${REPLAY_OUTPUT}" >&2
    exit 1
fi

# Step 6: Verify results
echo "[6/6] Checking results..."
echo ""
echo "--- Replay Output ---"
cat "${REPLAY_OUTPUT}"
echo "--- End Replay Output ---"
echo ""

# Extract replay count
REPLAY_COUNT=$(grep "^messages-replayed:" "${REPLAY_OUTPUT}" | awk '{print $2}')
FAILED_COUNT=$(grep "^failed:" "${REPLAY_OUTPUT}" | awk '{print $2}')

if [ -z "${REPLAY_COUNT}" ]; then
    echo "FAIL: Could not parse messages-replayed from replay output" >&2
    exit 1
fi

if [ -z "${FAILED_COUNT}" ]; then
    echo "FAIL: Could not parse failed count from replay output" >&2
    exit 1
fi

# Verify replay count matches producer count
if [ "${REPLAY_COUNT}" -ne "${MESSAGE_COUNT}" ]; then
    echo "FAIL: Expected ${MESSAGE_COUNT} messages replayed, got ${REPLAY_COUNT}" >&2
    exit 1
fi

# Verify 0 failed verifications
if [ "${FAILED_COUNT}" -ne 0 ]; then
    echo "FAIL: Expected 0 failed verifications, got ${FAILED_COUNT}" >&2
    exit 1
fi

echo "PASS: All ${MESSAGE_COUNT} messages recorded, replayed, and verified successfully."
exit 0
