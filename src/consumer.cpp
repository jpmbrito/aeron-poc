#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include <Aeron.h>
#include "aeron_poc/MessageHeader.h"
#include "aeron_poc/MarketData.h"

struct ConsumerConfig {
    std::string channel = "aeron:ipc";
    std::int32_t streamId = 1001;
    std::int64_t messageCount = 1000000;
    std::int32_t timeoutSeconds = 10;
    bool verifyBuffers = false;
};

static void printUsage(const char* programName) {
    std::cerr << "Usage: " << programName << " [options]\n"
              << "Options:\n"
              << "  --channel <uri>       Aeron channel URI (default: aeron:ipc)\n"
              << "  --stream-id <id>      Stream ID (default: 1001)\n"
              << "  --messages <count>    Number of messages to receive, 1-100000000 (default: 1000000)\n"
              << "  --timeout <seconds>   Receive timeout in seconds, 1-300 (default: 10)\n"
              << "  --verify-buffers      Enable buffer address verification\n";
}

static bool parseArgs(int argc, char* argv[], ConsumerConfig& config) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--channel") == 0) {
            if (i + 1 >= argc) {
                std::cerr << "Error: --channel requires a value\n";
                return false;
            }
            config.channel = argv[++i];
        } else if (std::strcmp(argv[i], "--stream-id") == 0) {
            if (i + 1 >= argc) {
                std::cerr << "Error: --stream-id requires a value\n";
                return false;
            }
            char* end = nullptr;
            long val = std::strtol(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0' || val < 1 || val > 2147483647L) {
                std::cerr << "Error: --stream-id must be between 1 and 2147483647\n";
                return false;
            }
            config.streamId = static_cast<std::int32_t>(val);
        } else if (std::strcmp(argv[i], "--messages") == 0) {
            if (i + 1 >= argc) {
                std::cerr << "Error: --messages requires a value\n";
                return false;
            }
            char* end = nullptr;
            long long val = std::strtoll(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0' || val < 1 || val > 100000000LL) {
                std::cerr << "Error: --messages must be between 1 and 100000000\n";
                return false;
            }
            config.messageCount = static_cast<std::int64_t>(val);
        } else if (std::strcmp(argv[i], "--timeout") == 0) {
            if (i + 1 >= argc) {
                std::cerr << "Error: --timeout requires a value\n";
                return false;
            }
            char* end = nullptr;
            long val = std::strtol(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0' || val < 1 || val > 300) {
                std::cerr << "Error: --timeout must be between 1 and 300 seconds\n";
                return false;
            }
            config.timeoutSeconds = static_cast<std::int32_t>(val);
        } else if (std::strcmp(argv[i], "--verify-buffers") == 0) {
            config.verifyBuffers = true;
        } else {
            std::cerr << "Error: unknown option: " << argv[i] << "\n";
            return false;
        }
    }
    return true;
}

/// Validate decoded MarketData fields.
/// Returns true if all fields pass validation:
/// - price: must be finite (not NaN, not infinity)
/// - symbol: each byte must be printable ASCII (0x20-0x7E) or null (0x00)
/// - sequenceNumber, timestamp, quantity: any value is valid for their types
static bool validateFields(aeron_poc::MarketData& decoder) {
    // Validate price: must be finite and non-NaN
    double price = decoder.price();
    if (!std::isfinite(price)) {
        return false;
    }

    // Validate symbol: each char must be printable ASCII (0x20-0x7E) or null (0x00)
    const char* sym = decoder.symbol();
    for (std::size_t i = 0; i < aeron_poc::MarketData::symbolLength(); ++i) {
        unsigned char c = static_cast<unsigned char>(sym[i]);
        if (c != 0x00 && (c < 0x20 || c > 0x7E)) {
            return false;
        }
    }

    // sequenceNumber (uint64), timestamp (int64), quantity (uint32) are valid for any value
    // Access them to ensure they're readable (zero-copy decode)
    (void)decoder.sequenceNumber();
    (void)decoder.timestamp();
    (void)decoder.quantity();

    return true;
}

int main(int argc, char* argv[]) {
    ConsumerConfig config;

    if (!parseArgs(argc, argv, config)) {
        printUsage(argv[0]);
        return 3;
    }

    // Connect to Aeron media driver with 5-second timeout
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

    // Add subscription on configured channel/stream-id
    std::int64_t subscriptionId = aeron->addSubscription(config.channel, config.streamId);

    // Wait for subscription to be ready
    std::shared_ptr<aeron::Subscription> subscription;
    while (!(subscription = aeron->findSubscription(subscriptionId))) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Counters for received messages and failed validations
    std::int64_t receivedCount = 0;
    std::int64_t failedCount = 0;
    const int fragmentLimit = 10;

    // Fragment handler: SBE decode directly from Aeron fragment buffer (zero-copy)
    auto handler = [&](aeron::concurrent::AtomicBuffer& buffer,
                       aeron::util::index_t offset,
                       aeron::util::index_t length,
                       aeron::Header& header) {
        // SBE message header is 8 bytes
        constexpr std::uint64_t headerSize = aeron_poc::MessageHeader::encodedLength();

        // Decode message header directly from the Aeron buffer (zero-copy)
        char* bufPtr = reinterpret_cast<char*>(buffer.buffer() + offset);
        std::uint64_t bufLen = static_cast<std::uint64_t>(length);

        aeron_poc::MessageHeader msgHeader(bufPtr, bufLen);
        std::uint16_t blockLength = msgHeader.blockLength();
        std::uint16_t version = msgHeader.version();

        // Decode MarketData directly from the same buffer after the header (zero-copy)
        aeron_poc::MarketData marketData;
        marketData.wrapForDecode(bufPtr, headerSize, blockLength, version, bufLen);

        // Verify buffer address if --verify-buffers is set
        if (config.verifyBuffers) {
            const char* codecBuf = marketData.buffer();
            const char* aeronBufStart = reinterpret_cast<const char*>(buffer.buffer());
            const char* aeronBufEnd = aeronBufStart + buffer.capacity();
            if (codecBuf < aeronBufStart || codecBuf >= aeronBufEnd) {
                std::cerr << "Error: SBE codec buffer address " << static_cast<const void*>(codecBuf)
                          << " is outside Aeron fragment buffer range ["
                          << static_cast<const void*>(aeronBufStart) << ", "
                          << static_cast<const void*>(aeronBufEnd) << ")\n";
                std::exit(4);
            }
        }

        // Validate fields
        if (!validateFields(marketData)) {
            ++failedCount;
        }

        ++receivedCount;
    };

    // Capture start time before the poll loop
    auto startTime = std::chrono::steady_clock::now();

    // Poll loop: receive messages until messageCount is reached
    while (receivedCount < config.messageCount) {
        subscription->poll(handler, fragmentLimit);

        // Timeout check: if no messages received at all within the configured timeout, exit
        if (receivedCount == 0) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - startTime).count();
            if (elapsed >= config.timeoutSeconds) {
                std::cerr << "Error: no messages received within " << config.timeoutSeconds << " seconds\n";
                return 2;
            }
        }
    }

    // Calculate elapsed time and print summary
    auto endTime = std::chrono::steady_clock::now();
    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();

    std::cout << "Messages received: " << receivedCount << "\n";
    std::cout << "Failed validations: " << failedCount << "\n";
    std::cout << "Elapsed time: " << elapsedMs << " ms\n";

    return 0;
}
