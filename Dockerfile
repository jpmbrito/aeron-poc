# Lightweight runtime images that use pre-built binaries from the host.
# Build locally first: cmake -B build && cmake --build build
# Then: docker compose build

FROM ubuntu:22.04 AS base

RUN apt-get update && apt-get install -y --no-install-recommends \
    libuuid1 libbsd0 \
    && rm -rf /var/lib/apt/lists/*

# ─────────────────────────────────────────────────────────────────────────────
# Producer image
# ─────────────────────────────────────────────────────────────────────────────
FROM base AS producer

COPY build/producer /usr/local/bin/producer
COPY build/_deps/aeron-build/binaries/aeronmd_s /usr/local/bin/aeronmd

ENV AERON_DIR=/dev/shm/aeron-docker
ENTRYPOINT ["producer"]

# ─────────────────────────────────────────────────────────────────────────────
# Consumer image
# ─────────────────────────────────────────────────────────────────────────────
FROM base AS consumer

COPY build/consumer /usr/local/bin/consumer
COPY build/_deps/aeron-build/binaries/aeronmd_s /usr/local/bin/aeronmd

ENV AERON_DIR=/dev/shm/aeron-docker
ENTRYPOINT ["consumer"]

# ─────────────────────────────────────────────────────────────────────────────
# Demo image (both binaries + aeronmd for UDP demo)
# ─────────────────────────────────────────────────────────────────────────────
FROM base AS demo

COPY build/producer /usr/local/bin/producer
COPY build/consumer /usr/local/bin/consumer
COPY build/_deps/aeron-build/binaries/aeronmd_s /usr/local/bin/aeronmd
