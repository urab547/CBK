#include <windows.h>

#include <cstdint>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "cbk/stage.h"
#include "src/byte_io.h"
#include "src/stages/vigenere_stage.h"
#include "src/stages/xor_cbc_stage.h"
#include "src/stages/xor_stage.h"

namespace cbk {
// ensure builtin registration is available
void RegisterBuiltinStages();
}  // namespace cbk

using namespace cbk;

static std::vector<uint8_t> RunProcess(IStage* stage, const std::vector<uint8_t>& input,
                                       size_t chunk) {
    VectorSink sink;
    for (size_t off = 0; off < input.size(); off += chunk) {
        size_t len = std::min(chunk, input.size() - off);
        stage->Process(input.data() + off, len, sink);
    }
    stage->Finish(sink);
    return sink.Buffer();
}

TEST(Stages, RoundTripAll) {
    // register builtin stages so factories are present
    RegisterBuiltinStages();

    std::vector<std::string> names = {"xor", "vigenere", "xor-cbc"};

    std::vector<std::vector<uint8_t>> payloads;
    // empty
    payloads.push_back({});
    // small text (ASCII only to avoid source encoding issues)
    std::string text = "hello, world!";
    payloads.push_back(std::vector<uint8_t>(text.begin(), text.end()));
    // random large
    std::mt19937 rng(12345);
    std::vector<uint8_t> rnd(10000);
    for (size_t i = 0; i < rnd.size(); ++i) rnd[i] = static_cast<uint8_t>(rng() & 0xFF);
    payloads.push_back(rnd);

    const std::string pwd = "P@ssw0rd";

    for (const auto& name : names) {
        SCOPED_TRACE("algorithm=" + name);
        IStageFactory* factory = StageRegistry::Instance().Find(name);
        ASSERT_NE(factory, nullptr);

        for (const auto& payload : payloads) {
            // forward
            auto fwd = factory->CreateForward();
            fwd->SetPassword(pwd);
            std::vector<uint8_t> enc = RunProcess(fwd.get(), payload, 13);

            // inverse
            auto inv = factory->CreateInverse();
            inv->SetPassword(pwd);
            std::vector<uint8_t> dec = RunProcess(inv.get(), enc, 11);

            EXPECT_EQ(payload, dec);
        }
    }
}

TEST(Stages, CbcHandlesIvAndCiphertextInOneCallAndArbitraryChunks) {
    XorCbcStageFactory factory;
    for (size_t size : {0u, 1u, 15u, 16u, 17u, 1024u, 65537u}) {
        const std::vector<uint8_t> plain(size, 42);
        auto encoder = factory.CreateForward();
        encoder->SetPassword("test");
        const auto encoded = RunProcess(encoder.get(), plain, 65536);
        EXPECT_EQ(16 + (size / 16 + 1) * 16, encoded.size());
        for (size_t chunk : {1u, 16u, 31u, 65536u, 100000u}) {
            auto decoder = factory.CreateInverse();
            decoder->SetPassword("test");
            EXPECT_EQ(plain, RunProcess(decoder.get(), encoded, chunk));
        }
    }
}

TEST(Stages, CbcStreamsBeforeFinishAndRejectsTruncationAndBadPadding) {
    XorCbcStageFactory factory;
    auto encoder = factory.CreateForward();
    encoder->SetPassword("test");
    const std::vector<uint8_t> plain(1024, 42);
    const auto encoded = RunProcess(encoder.get(), plain, 65536);
    auto decoder = factory.CreateInverse();
    decoder->SetPassword("test");
    VectorSink sink;
    decoder->Process(encoded.data(), encoded.size(), sink);
    EXPECT_EQ(plain, sink.Buffer());
    decoder->Finish(sink);
    EXPECT_EQ(plain, sink.Buffer());
    for (size_t length : {0u, 15u, 16u, 17u, 1055u}) {
        auto broken = factory.CreateInverse();
        broken->SetPassword("test");
        const std::vector<uint8_t> truncated(encoded.begin(), encoded.begin() + length);
        EXPECT_THROW(RunProcess(broken.get(), truncated, 65536), std::runtime_error);
    }
    auto corrupt = encoded;
    corrupt.back() ^= 1;
    auto broken = factory.CreateInverse();
    broken->SetPassword("test");
    EXPECT_THROW(RunProcess(broken.get(), corrupt, 65536), std::runtime_error);
}
