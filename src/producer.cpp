#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include <Aeron.h>
#include <concurrent/logbuffer/BufferClaim.h>

#include "aeron_poc/MessageHeader.h"
#include "aeron_poc/MarketData.h"

struct ProducerConfig {
    std::string channel = "aeron:ipc";
    std::int32_t streamId = 1001;
    std::int64_t messages = 1000000;
    bool verifyBuffers = false;
};

static void printUsage(const char* programName) {
    std::cerr << "Usage: " << programName << " [OPTIONS]\n"
              << "\n"
              << "Options:\n"
              << "  --channel <uri>       Aeron channel URI (default: aeron:ipc)\n"
              << "  --stream-id <id>      Stream ID (default: 1001)\n"
              << "  --messages <count>    Number of messages to send, 1-100000000 (default: 1000000)\n"
              << "  --verify-buffers      Enable zero-copy buffer address verification\n"
              << "  --help                Show this help message\n";
}

static bool parseArgs(int argc, char* argv[], ProducerConfig& config) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--channel") == 0) {
            if (i + 1 >= argc) {
                std::cerr << "Error: --channel requires an argument\n";
                return false;
            }
            config.channel = argv[++i];
            if (config.channel.empty()) {
                std::cerr << "Error: --channel must not be empty\n";
                return false;
            }
        } else if (std::strcmp(argv[i], "--stream-id") == 0) {
            if (i + 1 >= argc) {
                std::cerr << "Error: --stream-id requires an argument\n";
                return false;
            }
            ++i;
            char* end = nullptr;
            long val = std::strtol(argv[i], &end, 10);
            if (end == argv[i] || *end != '\0' || val < 1 || val > 2147483647L) {
                std::cerr << "Error: --stream-id must be an integer between 1 and 2147483647\n";
                return false;
            }
            config.streamId = static_cast<std::int32_t>(val);
        } else if (std::strcmp(argv[i], "--messages") == 0) {
            if (i + 1 >= argc) {
                std::cerr << "Error: --messages requires an argument\n";
                return false;
            }
            ++i;
            char* end = nullptr;
            long long val = std::strtoll(argv[i], &end, 10);
            if (end == argv[i] || *end != '\0' || val < 1 || val > 100000000LL) {
                std::cerr << "Error: --messages must be an integer between 1 and 100000000\n";
                return false;
            }
            config.messages = static_cast<std::int64_t>(val);
        } else if (std::strcmp(argv[i], "--verify-buffers") == 0) {
            config.verifyBuffers = true;
        } else if (std::strcmp(argv[i], "--help") == 0) {
            return false;
        } else {
            std::cerr << "Error: unknown argument '" << argv[i] << "'\n";
            return false;
        }
    }
    return true;
}

int main(int argc, char* argv[]) {
    ProducerConfig config;

    if (!parseArgs(argc, argv, config)) {
        printUsage(argv[0]);
        return 3;
    }

    // --- Connect to Aeron media driver with 5-second timeout ---
    aeron::Context ctx;
    ctx.mediaDriverTimeout(5000);

    // Use AERON_DIR environment variable if set (for archive driver compatibility)
    const char* aeronDir = std::getenv("AERON_DIR");
    if (aeronDir != nullptr) {
        ctx.aeronDir(aeronDir);
    }

    std::shared_ptr<aeron::Aeron> aeron;
    try {
        aeron = aeron::Aeron::connect(ctx);
    } catch (const std::exception& e) {
        std::cerr << "Error: failed to connect to Aeron media driver: " << e.what() << "\n";
        return 1;
    }

    // --- Add publication on configured channel/stream-id ---
    std::int64_t publicationId = aeron->addPublication(config.channel, config.streamId);

    std::shared_ptr<aeron::Publication> publication;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!(publication = aeron->findPublication(publicationId))) {
        std::this_thread::yield();
        if (std::chrono::steady_clock::now() > deadline) {
            std::cerr << "Error: timed out waiting for publication to become ready\n";
            return 1;
        }
    }

    // --- Wait for at least one subscriber to connect ---
    std::cerr << "Waiting for subscriber to connect...\n";
    auto connDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!publication->isConnected()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (std::chrono::steady_clock::now() > connDeadline) {
            std::cerr << "Error: no subscriber connected within 10 seconds\n";
            return 1;
        }
    }
    std::cerr << "Subscriber connected. Publishing...\n";

    // --- Publish loop: tryClaim → SBE encode → commit (zero-copy) ---
    constexpr std::size_t encodedLength = aeron_poc::MarketData::sbeBlockAndHeaderLength(); // 44 bytes

    aeron::concurrent::logbuffer::BufferClaim bufferClaim;

    auto startTime = std::chrono::steady_clock::now();

    for (std::int64_t i = 0; i < config.messages; ++i) {
        // tryClaim with back-pressure spin-wait retry (up to 1 second per message)
        std::int64_t result;
        auto backpressureDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);

        while (true) {
            result = publication->tryClaim(static_cast<aeron::util::index_t>(encodedLength), bufferClaim);
            if (result > 0) {
                break; // Successful claim
            }

            // Back-pressure or admin action: spin-wait and retry
            if (std::chrono::steady_clock::now() > backpressureDeadline) {
                std::cerr << "Error: back-pressure timeout after 1 second on message " << i << "\n";
                return 2;
            }
            // Spin-wait (busy loop)
        }

        // SBE encode directly into the claimed Aeron buffer (zero-copy)
        aeron::concurrent::AtomicBuffer& claimBuffer = bufferClaim.buffer();
        aeron::util::index_t claimOffset = bufferClaim.offset();

        char* bufferPtr = reinterpret_cast<char*>(claimBuffer.buffer() + claimOffset);
        std::uint64_t bufferLength = static_cast<std::uint64_t>(bufferClaim.length());

        aeron_poc::MarketData marketData;
        marketData.wrapAndApplyHeader(bufferPtr, 0, bufferLength);

        auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();

        marketData.sequenceNumber(static_cast<std::uint64_t>(i))
                  .timestamp(now)
                  .price(100.50 + static_cast<double>(i % 100) * 0.01)
                  .quantity(static_cast<std::uint32_t>(100 + (i % 1000)))
                  .putSymbol("AAPL    ");

        // --verify-buffers: assert SBE codec buffer address falls within Aeron buffer range
        if (config.verifyBuffers) {
            const char* sbeBuffer = marketData.buffer();
            const char* aeronStart = reinterpret_cast<const char*>(claimBuffer.buffer() + claimOffset);
            const char* aeronEnd = aeronStart + bufferLength;

            if (sbeBuffer < aeronStart || sbeBuffer >= aeronEnd) {
                std::cerr << "Error: buffer verification failed - SBE codec buffer address "
                          << static_cast<const void*>(sbeBuffer)
                          << " is outside Aeron buffer range ["
                          << static_cast<const void*>(aeronStart) << ", "
                          << static_cast<const void*>(aeronEnd) << ")\n";
                bufferClaim.abort();
                return 4;
            }
        }

        bufferClaim.commit();
    }

    auto endTime = std::chrono::steady_clock::now();
    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();
    std::int64_t throughput = (elapsedMs > 0) ? (config.messages * 1000 / elapsedMs) : 0;

    std::cout << "Messages sent: " << config.messages << "\n";
    std::cout << "Elapsed time: " << elapsedMs << " ms\n";
    std::cout << "Throughput: " << throughput << " msgs/sec\n";

    return 0;
}
