#!/bin/bash
set -e

# start_archive_media_driver.sh — Launch the Java Aeron Archive Media Driver.
# The Archive is a Java component that provides recording and replay services.
# Uses a DISTINCT Aeron directory (/dev/shm/aeron-archive-$USER) so it doesn't
# conflict with the regular C media driver.
# Prints the archive media driver PID to stdout so it can be terminated after use.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

# Allow overriding via environment variables
BUILD_DIR="${BUILD_DIR:-${PROJECT_DIR}/build}"
ARCHIVE_DIR="${ARCHIVE_DIR:-/tmp/aeron-archive}"
AERON_DIR="${AERON_DIR:-/dev/shm/aeron-archive-$(whoami)}"

# Aeron version (must match the C++ client version used in CMakeLists.txt)
AERON_VERSION="1.42.1"
AERON_ALL_JAR="${BUILD_DIR}/aeron-all-${AERON_VERSION}.jar"
AERON_ALL_URL="https://repo1.maven.org/maven2/io/aeron/aeron-all/${AERON_VERSION}/aeron-all-${AERON_VERSION}.jar"
AERON_ARCHIVE_JAR="${BUILD_DIR}/aeron-archive-${AERON_VERSION}.jar"
AERON_ARCHIVE_URL="https://repo1.maven.org/maven2/io/aeron/aeron-archive/${AERON_VERSION}/aeron-archive-${AERON_VERSION}.jar"

# Check Java is available
if ! command -v java &>/dev/null; then
    echo "ERROR: Java runtime not found. The Aeron Archive requires a JDK/JRE." >&2
    echo "       Install with: sudo apt install default-jdk" >&2
    exit 1
fi

# Download the aeron-all JAR if not present
if [ ! -f "${AERON_ALL_JAR}" ]; then
    echo "Downloading aeron-all-${AERON_VERSION}.jar..." >&2
    mkdir -p "${BUILD_DIR}"
    curl -fSL -o "${AERON_ALL_JAR}" "${AERON_ALL_URL}"
fi

# Download the aeron-archive JAR if not present
if [ ! -f "${AERON_ARCHIVE_JAR}" ]; then
    echo "Downloading aeron-archive-${AERON_VERSION}.jar..." >&2
    mkdir -p "${BUILD_DIR}"
    curl -fSL -o "${AERON_ARCHIVE_JAR}" "${AERON_ARCHIVE_URL}"
fi

# Kill any previously running archive media driver and clean stale shared memory
if pkill -f "ArchiveMediaDriver" 2>/dev/null; then
    sleep 0.5
fi
if [ -d "${AERON_DIR}" ]; then
    rm -rf "${AERON_DIR}"
fi
if [ -d "${ARCHIVE_DIR}" ]; then
    rm -rf "${ARCHIVE_DIR}"
fi

# Create archive storage directory
mkdir -p "${ARCHIVE_DIR}"

# Launch the Java Archive Media Driver in the background.
# This process embeds both the media driver and the archive service.
# The --add-opens flags are required for Java 17+ due to module system restrictions
# that Agrona needs to access internal NIO selector fields.
java -cp "${AERON_ALL_JAR}:${AERON_ARCHIVE_JAR}" \
    --add-opens java.base/sun.nio.ch=ALL-UNNAMED \
    --add-opens java.base/java.nio=ALL-UNNAMED \
    --add-opens java.base/java.lang=ALL-UNNAMED \
    -Daeron.dir="${AERON_DIR}" \
    -Daeron.archive.dir="${ARCHIVE_DIR}" \
    -Daeron.archive.control.channel.enabled=true \
    -Daeron.archive.control.channel="aeron:udp?endpoint=localhost:8010" \
    -Daeron.archive.control.stream.id=100 \
    -Daeron.archive.control.response.channel="aeron:udp?endpoint=localhost:0" \
    -Daeron.archive.control.response.stream.id=200 \
    -Daeron.archive.local.control.channel="aeron:ipc" \
    -Daeron.archive.local.control.stream.id=101 \
    -Daeron.archive.recording.events.channel="aeron:udp?endpoint=localhost:8011" \
    -Daeron.archive.recording.events.stream.id=102 \
    -Daeron.archive.replication.channel="aeron:udp?endpoint=localhost:0" \
    -Daeron.threading.mode=SHARED \
    -Daeron.archive.threading.mode=SHARED \
    io.aeron.archive.ArchivingMediaDriver > /dev/null 2>&1 &
DRIVER_PID=$!

# Brief wait to check if process started successfully (Java needs more time)
sleep 3
if ! kill -0 "${DRIVER_PID}" 2>/dev/null; then
    echo "ERROR: Archive Media Driver failed to start. Run manually to see error:" >&2
    echo "  java -cp ${AERON_ALL_JAR}:${AERON_ARCHIVE_JAR} -Daeron.dir=${AERON_DIR} -Daeron.archive.dir=${ARCHIVE_DIR} -Daeron.threading.mode=SHARED -Daeron.archive.threading.mode=SHARED io.aeron.archive.ArchivingMediaDriver" >&2
    exit 1
fi

echo "${DRIVER_PID}"
