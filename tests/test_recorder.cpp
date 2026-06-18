#include <gtest/gtest.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include "aeron_poc/MessageHeader.h"
#include "aeron_poc/MarketData.h"
#include "../src/recorder.h"

// ==============================================================================
// Helper: Encode a valid MarketData message into a buffer (header + body)
// ==============================================================================
static std::vector<char> encodeValidMarketData(
    std::uint64_t sequenceNumber = 1,
    std::int64_t timestamp = 1000000,
    double price = 123.45,
    std::uint32_t quantity = 100,
    const char* symbol = "AAPL    ")
{
    // Total size = MessageHeader (8 bytes) + MarketData block (36 bytes) = 44 bytes
    const std::size_t totalSize = aeron_poc::MessageHeader::encodedLength() +
                                  aeron_poc::MarketData::SBE_BLOCK_LENGTH;
    std::vector<char> buffer(totalSize, 0);

    // Use wrapAndApplyHeader to encode header + body in one shot
    aeron_poc::MarketData msg;
    msg.wrapAndApplyHeader(buffer.data(), 0, buffer.size());
    msg.sequenceNumber(sequenceNumber);
    msg.timestamp(timestamp);
    msg.price(price);
    msg.quantity(quantity);
    msg.putSymbol(symbol);

    return buffer;
}

// ==============================================================================
// Helper for parseArgs tests: create argv from strings
// ==============================================================================
class ArgBuilder {
public:
    ArgBuilder() {
        addArg("recorder"); // program name
    }

    void addArg(const char* arg) {
        argStrings_.push_back(arg);
    }

    int argc() const { return static_cast<int>(argStrings_.size()); }

    char** argv() {
        ptrs_.clear();
        for (auto& s : argStrings_) {
            ptrs_.push_back(const_cast<char*>(s));
        }
        return ptrs_.data();
    }

private:
    std::vector<const char*> argStrings_;
    std::vector<char*> ptrs_;
};

// ==============================================================================
// CLI Parsing Tests
// ==============================================================================

TEST(ParseArgsTest, RecordModeSetsDefaultsCorrectly) {
    ArgBuilder args;
    args.addArg("--record");

    RecorderConfig config;
    ASSERT_TRUE(parseArgs(args.argc(), args.argv(), config));
    EXPECT_EQ(config.mode, RecorderConfig::Mode::RECORD);
    EXPECT_EQ(config.channel, "aeron:ipc");
    EXPECT_EQ(config.streamId, 1001);
    EXPECT_FALSE(config.verify);
    EXPECT_TRUE(config.modeSet);
}

TEST(ParseArgsTest, ReplayModeWithAllFlags) {
    ArgBuilder args;
    args.addArg("--replay");
    args.addArg("--recording-id");
    args.addArg("42");
    args.addArg("--verify");

    RecorderConfig config;
    ASSERT_TRUE(parseArgs(args.argc(), args.argv(), config));
    EXPECT_EQ(config.mode, RecorderConfig::Mode::REPLAY);
    EXPECT_EQ(config.recordingId, 42);
    EXPECT_TRUE(config.verify);
    EXPECT_TRUE(config.modeSet);
}

TEST(ParseArgsTest, MissingModeFlagReturnsFalse) {
    ArgBuilder args;
    // No --record or --replay

    RecorderConfig config;
    EXPECT_FALSE(parseArgs(args.argc(), args.argv(), config));
}

TEST(ParseArgsTest, BothModesSpecifiedReturnsFalse) {
    ArgBuilder args;
    args.addArg("--record");
    args.addArg("--replay");

    RecorderConfig config;
    EXPECT_FALSE(parseArgs(args.argc(), args.argv(), config));
}

TEST(ParseArgsTest, ReplayWithoutRecordingIdReturnsFalse) {
    ArgBuilder args;
    args.addArg("--replay");

    RecorderConfig config;
    EXPECT_FALSE(parseArgs(args.argc(), args.argv(), config));
}

TEST(ParseArgsTest, StreamIdZeroRejected) {
    ArgBuilder args;
    args.addArg("--record");
    args.addArg("--stream-id");
    args.addArg("0");

    RecorderConfig config;
    EXPECT_FALSE(parseArgs(args.argc(), args.argv(), config));
}

TEST(ParseArgsTest, StreamIdNegativeRejected) {
    ArgBuilder args;
    args.addArg("--record");
    args.addArg("--stream-id");
    args.addArg("-1");

    RecorderConfig config;
    EXPECT_FALSE(parseArgs(args.argc(), args.argv(), config));
}

TEST(ParseArgsTest, StreamIdOverflowRejected) {
    ArgBuilder args;
    args.addArg("--record");
    args.addArg("--stream-id");
    args.addArg("2147483648");

    RecorderConfig config;
    EXPECT_FALSE(parseArgs(args.argc(), args.argv(), config));
}

TEST(ParseArgsTest, StreamIdMinAccepted) {
    ArgBuilder args;
    args.addArg("--record");
    args.addArg("--stream-id");
    args.addArg("1");

    RecorderConfig config;
    ASSERT_TRUE(parseArgs(args.argc(), args.argv(), config));
    EXPECT_EQ(config.streamId, 1);
}

TEST(ParseArgsTest, StreamIdMaxAccepted) {
    ArgBuilder args;
    args.addArg("--record");
    args.addArg("--stream-id");
    args.addArg("2147483647");

    RecorderConfig config;
    ASSERT_TRUE(parseArgs(args.argc(), args.argv(), config));
    EXPECT_EQ(config.streamId, 2147483647);
}

// ==============================================================================
// verifyMarketData Tests
// ==============================================================================

TEST(VerifyMarketDataTest, ValidMessagePasses) {
    auto buffer = encodeValidMarketData();
    EXPECT_TRUE(verifyMarketData(buffer.data(), buffer.size()));
}

TEST(VerifyMarketDataTest, NaNPriceFails) {
    auto buffer = encodeValidMarketData(1, 1000000, std::numeric_limits<double>::quiet_NaN(), 100, "AAPL    ");
    EXPECT_FALSE(verifyMarketData(buffer.data(), buffer.size()));
}

TEST(VerifyMarketDataTest, NullSentinelSequenceNumberFails) {
    auto buffer = encodeValidMarketData(UINT64_C(0xFFFFFFFFFFFFFFFF), 1000000, 123.45, 100, "AAPL    ");
    EXPECT_FALSE(verifyMarketData(buffer.data(), buffer.size()));
}

TEST(VerifyMarketDataTest, NonPrintableSymbolFails) {
    // Encode a valid message first, then corrupt the symbol
    auto buffer = encodeValidMarketData();

    // Symbol is at offset 28 from message body start, which is after header (8 bytes)
    // So symbol starts at byte 8 + 28 = 36 in the buffer
    buffer[36] = 0x01; // non-printable character

    EXPECT_FALSE(verifyMarketData(buffer.data(), buffer.size()));
}

TEST(VerifyMarketDataTest, ShortBufferFails) {
    // 35 bytes is less than the SBE block length requirement
    std::vector<char> buffer(35, 0);
    EXPECT_FALSE(verifyMarketData(buffer.data(), buffer.size()));
}

TEST(VerifyMarketDataTest, WrongTemplateIdFails) {
    auto buffer = encodeValidMarketData();

    // templateId is at offset 2 in the MessageHeader (uint16_t little-endian)
    // Set it to 99 instead of 1
    std::uint16_t wrongId = 99;
    std::memcpy(buffer.data() + 2, &wrongId, sizeof(std::uint16_t));

    EXPECT_FALSE(verifyMarketData(buffer.data(), buffer.size()));
}
