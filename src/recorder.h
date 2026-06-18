#ifndef RECORDER_H
#define RECORDER_H

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>

#include "aeron_poc/MessageHeader.h"
#include "aeron_poc/MarketData.h"

struct RecorderConfig {
    enum class Mode { RECORD, REPLAY };
    Mode mode = Mode::RECORD;
    std::string channel = "aeron:ipc";
    std::int32_t streamId = 1001;
    std::int64_t recordingId = -1;
    bool verify = false;
    bool modeSet = false;
};

inline void printUsage(const char* programName) {
    std::cerr << "Usage: " << programName << " [OPTIONS]\n"
              << "\n"
              << "Modes (mutually exclusive, one required):\n"
              << "  --record              Record messages from channel to archive\n"
              << "  --replay              Replay messages from a recording\n"
              << "\n"
              << "Options:\n"
              << "  --recording-id <id>   Recording ID to replay (required for --replay)\n"
              << "  --verify              Verify replayed messages with SBE decode\n"
              << "  --channel <uri>       Aeron channel URI (default: aeron:ipc)\n"
              << "  --stream-id <id>      Stream ID (default: 1001, range: 1-2147483647)\n"
              << "  --help                Show this help message\n";
}

inline bool parseArgs(int argc, char* argv[], RecorderConfig& config) {
    bool recordSet = false;
    bool replaySet = false;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--record") == 0) {
            recordSet = true;
        } else if (std::strcmp(argv[i], "--replay") == 0) {
            replaySet = true;
        } else if (std::strcmp(argv[i], "--recording-id") == 0) {
            if (i + 1 >= argc) {
                std::cerr << "Error: --recording-id requires an argument\n";
                return false;
            }
            ++i;
            char* end = nullptr;
            long long val = std::strtoll(argv[i], &end, 10);
            if (end == argv[i] || *end != '\0' || val < 0) {
                std::cerr << "Error: --recording-id must be a non-negative integer\n";
                return false;
            }
            config.recordingId = static_cast<std::int64_t>(val);
        } else if (std::strcmp(argv[i], "--verify") == 0) {
            config.verify = true;
        } else if (std::strcmp(argv[i], "--channel") == 0) {
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
            long long val = std::strtoll(argv[i], &end, 10);
            if (end == argv[i] || *end != '\0' || val < 1 || val > 2147483647LL) {
                std::cerr << "Error: --stream-id must be an integer between 1 and 2147483647\n";
                return false;
            }
            config.streamId = static_cast<std::int32_t>(val);
        } else if (std::strcmp(argv[i], "--help") == 0) {
            return false;
        } else {
            std::cerr << "Error: unknown argument '" << argv[i] << "'\n";
            return false;
        }
    }

    // Validate mutually exclusive modes
    if (recordSet && replaySet) {
        std::cerr << "Error: --record and --replay are mutually exclusive\n";
        return false;
    }
    if (!recordSet && !replaySet) {
        std::cerr << "Error: one of --record or --replay must be specified\n";
        return false;
    }

    if (recordSet) {
        config.mode = RecorderConfig::Mode::RECORD;
    } else {
        config.mode = RecorderConfig::Mode::REPLAY;
    }

    // Validate replay requires recording-id
    if (replaySet && config.recordingId < 0) {
        std::cerr << "Error: --replay requires --recording-id\n";
        return false;
    }

    config.modeSet = true;
    return true;
}

inline bool verifyMarketData(const char* buffer, std::size_t length) {
    // Need at least 8 bytes for the SBE message header
    if (length < aeron_poc::MessageHeader::encodedLength()) {
        return false;
    }

    // Decode the message header
    aeron_poc::MessageHeader hdr(
        const_cast<char*>(buffer), length);

    // Check templateId matches MarketData (id=1)
    if (hdr.templateId() != aeron_poc::MarketData::SBE_TEMPLATE_ID) {
        return false;
    }

    std::uint16_t blockLength = hdr.blockLength();

    // Check that the buffer contains the full message body
    if (length < aeron_poc::MessageHeader::encodedLength() + blockLength) {
        return false;
    }

    // Decode the MarketData message body (offset past the header)
    aeron_poc::MarketData msg;
    msg.wrapForDecode(
        const_cast<char*>(buffer),
        aeron_poc::MessageHeader::encodedLength(),
        blockLength,
        hdr.version(),
        length);

    // Validate fields
    if (msg.sequenceNumber() == UINT64_C(0xFFFFFFFFFFFFFFFF)) {
        return false;
    }
    if (msg.timestamp() == std::numeric_limits<std::int64_t>::min()) {
        return false;
    }
    if (std::isnan(msg.price())) {
        return false;
    }
    if (msg.quantity() == UINT32_C(0xFFFFFFFF)) {
        return false;
    }

    // Validate symbol: all 8 bytes must be printable ASCII (32–126)
    const char* sym = msg.symbol();
    for (std::size_t i = 0; i < 8; ++i) {
        unsigned char c = static_cast<unsigned char>(sym[i]);
        if (c < 32 || c > 126) {
            return false;
        }
    }

    return true;
}

#endif // RECORDER_H
