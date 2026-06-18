#!/bin/bash
set -e

# UDP test: two separate Docker containers, each with its own media driver,
# communicating over real UDP on localhost:40123.
# Uses network_mode: host so both containers share the host network.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

export MESSAGE_COUNT="${MESSAGE_COUNT:-10000}"

cd "${PROJECT_DIR}"

echo "=== Aeron UDP Docker Test (separate containers) ==="
echo "Messages: ${MESSAGE_COUNT}"
echo "Transport: UDP localhost:40123"
echo ""

# Clean up
docker compose down --remove-orphans 2>/dev/null || true
rm -rf /dev/shm/aeron-udp-consumer /dev/shm/aeron-udp-producer

# Build
echo "[1/2] Building Docker images..."
docker compose build --quiet
echo "       Done."
echo ""

# Run — wait for consumer to exit (it exits after receiving all messages)
echo "[2/2] Running..."
echo ""
docker compose up --abort-on-container-exit --exit-code-from consumer

# Cleanup
echo ""
docker compose down 2>/dev/null || true
rm -rf /dev/shm/aeron-udp-consumer /dev/shm/aeron-udp-producer
