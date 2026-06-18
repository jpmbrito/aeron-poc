# Aeron SBE PoC

A proof of concept repository demonstrating high-performance inter-process communication (IPC) using [Aeron](https://github.com/real-logic/aeron) with [SBE](https://github.com/real-logic/simple-binary-encoding) zero-copy serialization in C++.

Two binaries — a **producer** and a **consumer** — communicate over an Aeron IPC channel backed by shared memory (`/dev/shm`). Messages are encoded/decoded using SBE directly in Aeron's log buffers with no intermediate copies. A **recorder** binary demonstrates Aeron Archive for durable message recording and replay.

## Architecture

```
┌──────────────────┐       /dev/shm        ┌──────────────────┐
│    Producer       │◄────────────────────►│  Aeron Media     │
│  tryClaim → SBE  │    shared memory      │     Driver       │
│  encode → commit │                       │                  │
└──────────────────┘                       └────────┬─────────┘
                                                    │
                                                    │ shared memory
                                                    ▼
                                           ┌──────────────────┐
                                           │    Consumer       │
                                           │  poll → SBE      │
                                           │  decode → validate│
                                           └──────────────────┘
```

## Message Format

SBE `MarketData` message (44 bytes on wire):

| Field | Type | Size |
|-------|------|------|
| messageHeader | composite | 8 bytes |
| sequenceNumber | uint64 | 8 bytes |
| timestamp | int64 | 8 bytes |
| price | double | 8 bytes |
| quantity | uint32 | 4 bytes |
| symbol | char[8] | 8 bytes |

## Prerequisites

- CMake 3.14+
- C++17 compiler (GCC 7+ or Clang 5+)
- Java 17+ runtime (for SBE code generation and Aeron Archive Media Driver)
- Linux (for Aeron IPC via `/dev/shm` and `perf` profiling)

## Build

```bash
cmake -B build
cmake --build build
```

This fetches Aeron (v1.42.1) and SBE (v1.33.0) automatically via CMake FetchContent, generates the SBE C++ codec headers from `schema/messages.xml`, and compiles both binaries with `-O2 -g -fno-omit-frame-pointer` for profiling support.

## Usage

### 1. Start the media driver

```bash
./scripts/start_media_driver.sh
# Prints the driver PID to stdout
```

### 2. Run the producer

```bash
./build/producer [OPTIONS]

Options:
  --channel <uri>       Aeron channel URI (default: aeron:ipc)
  --stream-id <id>      Stream ID (default: 1001)
  --messages <count>    Number of messages to send, 1-100000000 (default: 1000000)
  --verify-buffers      Enable zero-copy buffer address verification
```

### 3. Run the consumer

```bash
./build/consumer [OPTIONS]

Options:
  --channel <uri>       Aeron channel URI (default: aeron:ipc)
  --stream-id <id>      Stream ID (default: 1001)
  --messages <count>    Number of messages to receive, 1-100000000 (default: 1000000)
  --timeout <seconds>   Receive timeout in seconds, 1-300 (default: 10)
  --verify-buffers      Enable buffer address verification
```

### Quick integration test

```bash
# Basic test (1000 messages, verifies delivery)
./scripts/run_integration_test.sh

# With perf profiling (auto-bumps to 1M messages, collects perf data)
./scripts/run_integration_test.sh --perf
```

This starts the media driver, launches both binaries, and verifies 0 failed validations. With `--perf`, it wraps both binaries in `perf record -g` and outputs `perf-producer.data` / `perf-consumer.data` for analysis. Customize message count with `MESSAGE_COUNT=100000 ./scripts/run_integration_test.sh`.

## Profiling with perf

```bash
# Run integration test with profiling (auto-bumps to 1M messages)
./scripts/run_integration_test.sh --perf

# Or profile manually
perf record -g ./build/producer --messages 1000000
perf report -i perf-producer.data
```

Both binaries preserve frame pointers and include debug symbols for accurate stack unwinding and source-level annotation.

If perf is blocked by permissions:

```bash
sudo sysctl kernel.perf_event_paranoid=-1
sudo sysctl kernel.kptr_restrict=0
```

### Sample Results

Environment: Linux (WSL2), single-machine IPC, 1M messages of 44 bytes each.

**Throughput:**

| Binary | Messages | Elapsed | Throughput |
|--------|----------|---------|------------|
| Producer | 1,000,000 | 93 ms | ~10.7M msgs/sec |
| Consumer | 1,000,000 | 175 ms | ~5.7M msgs/sec |

**Producer profile breakdown (735 samples):**

| Self% | Function | Interpretation |
|-------|----------|----------------|
| 18.9% | `Publication::tryClaim` | Atomic CAS to claim log buffer slot — the expected IPC bottleneck |
| 12.5% | `[vdso]` (clock_gettime) | `steady_clock::now()` calls for timestamps and deadline checks |
| 5.5% | `main` | SBE encoding loop (field writes into claimed buffer) — confirms zero-copy is cheap |
| 2.9% | `__sched_yield` | Thread yield during startup publication-find loop |
| 2.8% | `std::_Hashtable` | Aeron internal publication lookup |

**Key observations:**

- No `malloc`/`operator new` in the hot path — zero heap allocations confirmed
- SBE encoding (5.5%) is a small fraction of total cost, validating the zero-copy design
- `tryClaim` (19%) is the dominant cost and represents the fundamental IPC mechanism
- Clock calls (14% combined) are the second-largest cost — could be reduced by using `rdtsc` directly or batching timestamps

## Exit Codes

| Code | Meaning |
|------|---------|
| 0 | Success |
| 1 | Media driver connection failure |
| 2 | Back-pressure timeout (producer) / Receive timeout (consumer) |
| 3 | Invalid CLI arguments |
| 4 | Buffer verification failure (with `--verify-buffers`) |

## Zero-Copy Verification

Pass `--verify-buffers` to either binary to enable runtime assertions that confirm SBE codecs are operating directly on Aeron buffers without intermediate allocations.

## Project Structure

```
├── CMakeLists.txt              # Build system with FetchContent
├── schema/
│   └── messages.xml            # SBE message schema
├── src/
│   ├── producer.cpp            # Producer binary
│   ├── consumer.cpp            # Consumer binary
│   ├── recorder.cpp            # Archive recorder binary
│   └── recorder.h              # Shared recorder types (config, parsing, verification)
├── tests/
│   ├── test_main.cpp           # Google Test + RapidCheck tests
│   └── test_recorder.cpp       # Recorder unit + property tests
└── scripts/
    ├── start_media_driver.sh           # Launch C aeronmd for IPC
    ├── start_archive_media_driver.sh   # Launch Java Archive Media Driver
    ├── run_integration_test.sh         # End-to-end IPC test (with optional --perf)
    └── run_archive_test.sh             # Archive record-replay-verify test
```

## Aeron Archive (Record & Replay)

The project includes a **recorder** binary that uses Aeron Archive to persist messages to disk and replay them on demand.

### Prerequisites (in addition to base requirements)

- Java 17+ (the Archive is a Java service — `aeron-all` and `aeron-archive` JARs are downloaded automatically on first run)
- The producer/consumer must set `AERON_DIR` to the archive driver's directory when publishing through it

### 1. Start the Archive Media Driver

```bash
./scripts/start_archive_media_driver.sh
# Prints the driver PID to stdout
# Uses /dev/shm/aeron-archive-$USER (separate from regular driver)
# Archives stored in /tmp/aeron-archive (configurable via ARCHIVE_DIR)
```

### 2. Record messages

```bash
# Start recording (runs until SIGINT)
./build/recorder --record --channel aeron:ipc --stream-id 1001
# Output: recording-id: ready
# (actual recording-id printed once data flows)

# In another terminal, run the producer through the archive driver
AERON_DIR=/dev/shm/aeron-archive-$USER ./build/producer --messages 10000

# Stop recording with Ctrl+C
```

### 3. Replay messages

```bash
# Replay without verification
./build/recorder --replay --recording-id 0
# Output: messages-replayed: 10000

# Replay with SBE verification
./build/recorder --replay --recording-id 0 --verify
# Output:
#   messages-replayed: 10000
#   verified: 10000
#   failed: 0
```

### Recorder CLI

```
./build/recorder [MODE] [OPTIONS]

Modes (mutually exclusive, one required):
  --record              Record messages from the channel
  --replay              Replay a previous recording

Options:
  --recording-id <id>   Recording ID to replay (required for --replay)
  --verify              Verify replayed messages with SBE decode (replay only)
  --channel <uri>       Aeron channel URI (default: aeron:ipc)
  --stream-id <id>      Stream ID (default: 1001)
```

### Archive integration test

```bash
./scripts/run_archive_test.sh
```

This starts the archive media driver, records messages from the producer, replays them with SBE verification, and reports pass/fail.

## Tests

```bash
cd build && ctest --output-on-failure
```

Unit tests use Google Test. Property-based tests use RapidCheck.

## Docker (UDP Network Mode)

Run the producer and consumer in **separate Docker containers**, each with its own media driver, communicating over UDP:

```bash
./scripts/run_udp_test.sh
```

Each container runs its own `aeronmd` with a distinct Aeron directory. They communicate over real UDP on `localhost:40123` — not shared memory. Uses `network_mode: host` because Aeron's bidirectional UDP protocol (status messages, NAKs on ephemeral ports) doesn't work through Docker bridge NAT.

```
┌─────────────────────────┐      UDP localhost:40123     ┌─────────────────────────┐
│  producer container      │ ─────────────────────────► │  consumer container      │
│  aeronmd (aeron-udp-prod)│                            │  aeronmd (aeron-udp-cons)│
│  producer binary         │ ◄───── status/NAKs ─────── │  consumer binary         │
└─────────────────────────┘                             └─────────────────────────┘
```

Customize message count:

```bash
MESSAGE_COUNT=100000 ./scripts/run_udp_test.sh
```
