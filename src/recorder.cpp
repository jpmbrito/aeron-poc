#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <thread>

#include "client/AeronArchive.h"

#include "recorder.h"

static volatile std::sig_atomic_t g_running = 1;

static void signalHandler(int /*signal*/) {
    g_running = 0;
}

// Forward declarations for record/replay implementations
static int runRecord(const RecorderConfig& config);
static int runReplay(const RecorderConfig& config);

static int runRecord(const RecorderConfig& config) {
    // Install signal handlers for graceful shutdown
    struct sigaction sa{};
    sa.sa_handler = signalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    // Determine Aeron directory for the archive media driver
    std::string aeronDir = "/dev/shm/aeron-archive-";
    const char* user = std::getenv("USER");
    if (user == nullptr) {
        user = std::getenv("LOGNAME");
    }
    if (user != nullptr) {
        aeronDir += user;
    } else {
        aeronDir += "unknown";
    }

    // Create archive context
    aeron::archive::client::Context ctx;
    ctx.controlRequestChannel("aeron:udp?endpoint=localhost:8010");
    ctx.controlRequestStreamId(100);
    ctx.controlResponseChannel("aeron:udp?endpoint=localhost:0");
    ctx.controlResponseStreamId(200);
    ctx.recordingEventsChannel("aeron:udp?endpoint=localhost:8011");
    ctx.messageTimeoutNs(10000000000LL);
    ctx.aeronDirectoryName(aeronDir);

    // Connect to the archive
    std::shared_ptr<aeron::archive::client::AeronArchive> archive;
    try {
        archive = aeron::archive::client::AeronArchive::connect(ctx);
    } catch (const std::exception& e) {
        std::cerr << "Error: failed to connect to archive: " << e.what() << "\n";
        return 1;
    }

    // Start recording on the configured channel and stream
    std::int64_t subscriptionId = 0;
    try {
        subscriptionId = archive->startRecording(
            config.channel, config.streamId, aeron::archive::client::AeronArchive::SourceLocation::LOCAL);
        (void)subscriptionId;  // Used for stopRecording by subscriptionId if needed
    } catch (const std::exception& e) {
        std::cerr << "Error: failed to start recording: " << e.what() << "\n";
        return 1;
    }

    // Find the recording ID via listRecordingsForUri
    // The recording might not appear immediately — poll until it shows up or until signal
    std::int64_t recordingId = -1;
    auto consumer = [&recordingId](
        std::int64_t /*controlSessionId*/,
        std::int64_t /*correlationId*/,
        std::int64_t recId,
        std::int64_t /*startTimestamp*/,
        std::int64_t /*stopTimestamp*/,
        std::int64_t /*startPosition*/,
        std::int64_t /*stopPosition*/,
        std::int32_t /*initialTermId*/,
        std::int32_t /*segmentFileLength*/,
        std::int32_t /*termBufferLength*/,
        std::int32_t /*mtuLength*/,
        std::int32_t /*sessionId*/,
        std::int32_t /*streamId*/,
        const std::string& /*strippedChannel*/,
        const std::string& /*originalChannel*/,
        const std::string& /*sourceIdentity*/)
    {
        recordingId = recId;
    };

    // Print recording-id: ready to signal the script we're waiting for data
    // The actual recording ID will be printed once data flows
    std::cout << "recording-id: ready" << std::endl;

    // Idle loop: sleep and poll for recording signals to keep the archive client alive
    // Also periodically check for the recording ID to print it
    bool idPrinted = false;
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        try {
            archive->pollForRecordingSignals();
        } catch (...) {
            // Ignore errors during idle polling
        }

        // Try to find the recording ID if not yet printed
        if (!idPrinted) {
            try {
                std::int32_t found = archive->listRecordingsForUri(
                    0, 100, config.channel, config.streamId, consumer);
                if (found > 0 && recordingId >= 0) {
                    std::cout << "recording-id: " << recordingId << std::endl;
                    idPrinted = true;
                }
            } catch (...) {
                // Ignore — will retry next iteration
            }
        }
    }

    // On signal: stop the recording and exit cleanly
    try {
        archive->stopRecording(config.channel, config.streamId);
    } catch (const std::exception& e) {
        std::cerr << "Warning: failed to stop recording: " << e.what() << "\n";
    }

    return 0;
}

static int runReplay(const RecorderConfig& config) {
    // Determine Aeron directory for the archive media driver (same as runRecord)
    std::string aeronDir = "/dev/shm/aeron-archive-";
    const char* user = std::getenv("USER");
    if (user == nullptr) {
        user = std::getenv("LOGNAME");
    }
    if (user != nullptr) {
        aeronDir += user;
    } else {
        aeronDir += "unknown";
    }

    // Create archive context (same configuration as runRecord)
    aeron::archive::client::Context ctx;
    ctx.controlRequestChannel("aeron:udp?endpoint=localhost:8010");
    ctx.controlRequestStreamId(100);
    ctx.controlResponseChannel("aeron:udp?endpoint=localhost:0");
    ctx.controlResponseStreamId(200);
    ctx.recordingEventsChannel("aeron:udp?endpoint=localhost:8011");
    ctx.messageTimeoutNs(10000000000LL);  // 10 seconds
    ctx.aeronDirectoryName(aeronDir);

    // Connect to the archive
    std::shared_ptr<aeron::archive::client::AeronArchive> archive;
    try {
        archive = aeron::archive::client::AeronArchive::connect(ctx);
    } catch (const std::exception& e) {
        std::cerr << "Error: failed to connect to archive: " << e.what() << "\n";
        return 1;
    }

    // Replay channel and stream ID (use IPC with a distinct stream ID to avoid conflicts)
    const std::string replayChannel = "aeron:ipc";
    const std::int32_t replayStreamId = 1002;

    // Start replay — returns a subscription for consuming the replay
    std::shared_ptr<aeron::Subscription> subscription;
    try {
        subscription = archive->replay(
            config.recordingId,
            static_cast<std::int64_t>(0),       // startPosition
            std::numeric_limits<std::int64_t>::max(),  // length (entire recording)
            replayChannel,
            replayStreamId);
    } catch (const std::exception& e) {
        std::cerr << "Error: failed to start replay for recording " << config.recordingId
                  << ": " << e.what() << "\n";
        return 1;
    }

    // Poll the subscription until end-of-stream
    std::int64_t messageCount = 0;
    std::int64_t verifiedCount = 0;
    std::int64_t failedCount = 0;
    bool imageWasAvailable = false;
    const int fragmentLimit = 10;

    auto handler = [&](aeron::concurrent::AtomicBuffer& buffer,
                       aeron::util::index_t offset,
                       aeron::util::index_t length,
                       aeron::Header& /*header*/) {
        ++messageCount;
        if (config.verify) {
            const char* data = reinterpret_cast<const char*>(buffer.buffer()) + offset;
            if (verifyMarketData(data, static_cast<std::size_t>(length))) {
                ++verifiedCount;
            } else {
                ++failedCount;
            }
        }
    };

    while (true) {
        int fragments = subscription->poll(handler, fragmentLimit);

        // Track whether we've ever seen an image (connection established)
        if (!imageWasAvailable && subscription->imageCount() > 0) {
            imageWasAvailable = true;
        }

        // End-of-stream: image was connected but is now closed or gone
        if (imageWasAvailable && subscription->imageCount() > 0) {
            auto image = subscription->imageByIndex(0);
            if (image->isClosed()) {
                break;
            }
        } else if (imageWasAvailable && subscription->imageCount() == 0) {
            break;
        }

        // If no fragments and no image yet appeared, keep polling (brief sleep to avoid busy-spin)
        if (fragments == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    std::cout << "messages-replayed: " << messageCount << std::endl;

    if (config.verify) {
        std::cout << "verified: " << verifiedCount << std::endl;
        std::cout << "failed: " << failedCount << std::endl;
        if (failedCount > 0) {
            return 1;
        }
    }

    return 0;
}

int main(int argc, char* argv[]) {
    RecorderConfig config;

    if (!parseArgs(argc, argv, config)) {
        printUsage(argv[0]);
        return 3;
    }

    if (config.mode == RecorderConfig::Mode::RECORD) {
        return runRecord(config);
    } else {
        return runReplay(config);
    }
}
