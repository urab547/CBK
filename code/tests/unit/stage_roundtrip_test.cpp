#include <windows.h>

#include <cstdint>
#include <memory>
#include <random>
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
