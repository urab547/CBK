// Copyright 2026 CBK Project.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "cbk/engine.h"
#include "cbk/packer.h"
#include "cbk/stage.h"
#include "src/crc32.h"
#include "src/stages/huffman_stage.h"
#include "unit/temp_dir.h"

namespace {

using Bytes = std::vector<uint8_t>;

class Buffer : public cbk::ISink {
public:
    void Write(const uint8_t* data, size_t len) override {
        bytes.insert(bytes.end(), data, data + len);
        largest_write = std::max(largest_write, len);
    }
    Bytes bytes;
    size_t largest_write = 0;
};

Bytes Transform(const Bytes& input, bool forward, size_t chunk = 7919) {
    cbk::HuffmanStageFactory factory;
    auto stage = forward ? factory.CreateForward() : factory.CreateInverse();
    Buffer output;
    stage->Process(nullptr, 0, output);
    for (size_t offset = 0; offset < input.size(); offset += chunk) {
        stage->Process(input.data() + offset, std::min(chunk, input.size() - offset), output);
    }
    stage->Finish(output);
    return output.bytes;
}

Bytes RandomBytes(size_t size) {
    std::mt19937 random(12345);
    Bytes bytes(size);
    for (auto& byte : bytes) byte = static_cast<uint8_t>(random());
    return bytes;
}

void Put32(Bytes* bytes, size_t offset, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) (*bytes)[offset + i] = static_cast<uint8_t>(value >> (8 * i));
}

// 独立构造：等频 A、B 的码字分别为 0、1，不依赖生产编码器。
Bytes Fixture() {
    Bytes bytes{'C', 'B', 'K', 'H', 'F', '0', '0', '1'};
    bytes.resize(21 + 1024 + 1250 + 13, 0);
    bytes[8] = 2;
    Put32(&bytes, 9, 10000);
    Put32(&bytes, 13, 2274);
    Bytes original(5000, 'A');
    original.insert(original.end(), 5000, 'B');
    Put32(&bytes, 17, cbk::ComputeCrc32(original.data(), original.size()));
    Put32(&bytes, 21 + 'A' * 4, 5000);
    Put32(&bytes, 21 + 'B' * 4, 5000);
    std::fill(bytes.begin() + 21 + 1024 + 625, bytes.end() - 13, 0xff);
    return bytes;
}

TEST(HuffmanStage, FactoryRegistersCompressionBothDirections) {
    cbk::RegisterBuiltinStages();
    auto* factory = cbk::StageRegistry::Instance().Find("huffman");
    ASSERT_NE(nullptr, factory);
    EXPECT_EQ(cbk::StageKind::kCompress, factory->Kind());
    EXPECT_EQ("huffman", factory->CreateForward()->Name());
    EXPECT_EQ("huffman", factory->CreateInverse()->Name());
}

TEST(HuffmanStage, RoundTripsBoundarySizesAndArbitraryChunks) {
    for (size_t size : {0u, 1u, 255u, 65535u, 65536u, 65537u, 196625u}) {
        SCOPED_TRACE(size);
        const auto input = RandomBytes(size);
        const auto encoded = Transform(input, true);
        EXPECT_EQ(input, Transform(encoded, false, 1));
        EXPECT_EQ(encoded, Transform(input, true, 65536));
    }
}

TEST(HuffmanStage, RoundTripsSingleSymbolsAndAllByteValues) {
    for (uint8_t symbol : {0, 255, 65}) {
        const Bytes input(131073, symbol);
        const auto encoded = Transform(input, true, 1);
        EXPECT_LT(encoded.size(), input.size() / 10);
        EXPECT_EQ(input, Transform(encoded, false));
    }
    Bytes all_bytes(131072);
    for (size_t i = 0; i < all_bytes.size(); ++i) all_bytes[i] = static_cast<uint8_t>(i);
    EXPECT_EQ(all_bytes, Transform(Transform(all_bytes, true), false));
}

TEST(HuffmanStage, CompressesTextAndPreservesAlreadyCompressedBytes) {
    const std::string sentence = "Huffman compression preserves every byte.\n";
    Bytes text;
    for (int i = 0; i < 5000; ++i) text.insert(text.end(), sentence.begin(), sentence.end());
    const auto encoded = Transform(text, true);
    EXPECT_LT(encoded.size(), text.size());
    EXPECT_EQ(text, Transform(encoded, false, 13));
    EXPECT_EQ(encoded, Transform(Transform(encoded, true), false));
}

TEST(HuffmanStage, RawFallbackBoundsExpansion) {
    const auto input = RandomBytes(65536);
    const auto encoded = Transform(input, true);
    ASSERT_EQ(input.size() + 8 + 13 + 13, encoded.size());
    EXPECT_EQ(1, encoded[8]);
}

TEST(HuffmanStage, MatchesIndependentBitstreamAndTieBreaking) {
    Bytes original(5000, 'A');
    original.insert(original.end(), 5000, 'B');
    EXPECT_EQ(original, Transform(Fixture(), false, 3));
    EXPECT_EQ(Fixture(), Transform(original, true));
}

TEST(HuffmanStage, FlushesFullBlocksBeforeFinish) {
    cbk::HuffmanStageFactory factory;
    auto encoder = factory.CreateForward();
    Buffer compressed;
    const auto input = RandomBytes(65536);
    encoder->Process(input.data(), input.size(), compressed);
    ASSERT_GT(compressed.bytes.size(), 8u);
    auto decoder = factory.CreateInverse();
    Buffer restored;
    decoder->Process(compressed.bytes.data(), compressed.bytes.size(), restored);
    EXPECT_EQ(input, restored.bytes);
    EXPECT_LE(restored.largest_write, 65536u);
    const size_t previous = compressed.bytes.size();
    encoder->Finish(compressed);
    const size_t tail_size = compressed.bytes.size() - previous;
    decoder->Process(compressed.bytes.data() + previous, tail_size, restored);
    EXPECT_NO_THROW(decoder->Finish(restored));
}

TEST(HuffmanStage, RejectsEveryTruncationOfSmallStream) {
    const auto encoded = Transform(Bytes{1, 2, 3, 4, 5}, true);
    for (size_t length = 0; length < encoded.size(); ++length) {
        EXPECT_THROW(Transform(Bytes(encoded.begin(), encoded.begin() + length), false),
                     std::runtime_error);
    }
    const auto fixture = Fixture();
    for (size_t length : {20u, 21u, 100u, 1044u, 1045u, 2294u, 2295u, 2307u}) {
        EXPECT_THROW(Transform(Bytes(fixture.begin(), fixture.begin() + length), false),
                     std::runtime_error);
    }
}

TEST(HuffmanStage, RejectsInvalidMagicHeadersAndFrequencyTables) {
    const auto fixture = Fixture();
    for (size_t offset : {0u, 7u, 8u, 12u, 16u, 21u}) {
        auto bytes = fixture;
        bytes[offset] = 255;
        EXPECT_THROW(Transform(bytes, false), std::runtime_error);
    }
    auto bytes = fixture;
    Put32(&bytes, 21 + 'A' * 4, 0xffffffffu);
    EXPECT_THROW(Transform(bytes, false), std::runtime_error);
    bytes = fixture;
    Put32(&bytes, 13, 1024);  // 码表有效，但缺少由频率决定的位流。
    EXPECT_THROW(Transform(bytes, false), std::runtime_error);
}

TEST(HuffmanStage, RejectsCorruptionBeforeEmittingBlock) {
    for (auto bytes : {Fixture(), Transform(Bytes{1, 2, 3}, true)}) {
        bytes[21] ^= 1;
        cbk::HuffmanStageFactory factory;
        auto decoder = factory.CreateInverse();
        Buffer output;
        EXPECT_THROW(decoder->Process(bytes.data(), bytes.size(), output), std::runtime_error);
        EXPECT_TRUE(output.bytes.empty());
    }
    auto bytes = Fixture();
    bytes[1045] ^= 0x80;  // 改变符号数量。
    EXPECT_THROW(Transform(bytes, false), std::runtime_error);
    bytes = Fixture();
    bytes[1045] ^= 0x80;
    bytes[1670] ^= 0x80;  // 数量相同，顺序改变：由 CRC 检出。
    EXPECT_THROW(Transform(bytes, false), std::runtime_error);
}

TEST(HuffmanStage, RejectsNonzeroPaddingAndInvalidEndMarkers) {
    Bytes original(5000, 'A');
    original.insert(original.end(), 5001, 'B');
    auto bytes = Transform(original, true);
    bytes[bytes.size() - 14] |= 1;
    EXPECT_THROW(Transform(bytes, false), std::runtime_error);
    bytes = Transform({}, true);
    bytes[9] = 1;
    EXPECT_THROW(Transform(bytes, false), std::runtime_error);
    bytes = Transform({}, true);
    bytes.push_back(0);
    EXPECT_THROW(Transform(bytes, false), std::runtime_error);
}

TEST(HuffmanStage, RejectsCallsAfterFinish) {
    cbk::HuffmanStageFactory factory;
    for (bool forward : {true, false}) {
        auto stage = forward ? factory.CreateForward() : factory.CreateInverse();
        Buffer output;
        if (!forward) {
            const auto empty = Transform({}, true);
            stage->Process(empty.data(), empty.size(), output);
        }
        stage->Finish(output);
        EXPECT_THROW(stage->Finish(output), std::runtime_error);
        EXPECT_THROW(stage->Process(nullptr, 0, output), std::runtime_error);
    }
}

TEST(HuffmanStage, EngineRoundTripsAllPackersAndChainedStages) {
    cbk::RegisterBuiltinPackers();
    cbk::RegisterBuiltinStages();
    for (const auto* packer : {"cbk-native", "tar", "cpio"}) {
        SCOPED_TRACE(packer);
        cbk_test::TempDir temp;
        temp.MakeDir(L"src");
        temp.MakeDir(L"src\\中文目录");
        const auto random = RandomBytes(131077);
        const std::string binary(random.begin(), random.end());
        temp.MakeFile(L"src\\中文目录\\binary", binary);
        temp.MakeFile(L"src\\repeated", std::string(196613, 'x'));
        temp.MakeFile(L"src\\empty", "");
        cbk::BackupOptions backup;
        backup.source_root = temp.At(L"src");
        backup.dest_archive = temp.At(L"backup.cbk");
        backup.packer = packer;
        backup.stages = {"huffman", "huffman"};
        ASSERT_EQ(cbk::Status::kOk, cbk::RunBackup(backup, nullptr).status);
        std::wstring error;
        EXPECT_EQ(cbk::Status::kOk, cbk::VerifyArchive(backup.dest_archive, &error));
        cbk::ArchiveInfo info;
        std::vector<cbk::EntryMeta> entries;
        ASSERT_EQ(cbk::Status::kOk,
                  cbk::ReadArchiveListing(backup.dest_archive, "", &info, &entries, &error));
        EXPECT_EQ(backup.stages, info.stages);
        EXPECT_EQ(4u, entries.size());
        cbk::RestoreOptions restore;
        restore.archive = backup.dest_archive;
        restore.dest_root = temp.At(L"back");
        ASSERT_EQ(cbk::Status::kOk, cbk::RunRestore(restore, nullptr).status);
        EXPECT_EQ(binary, temp.ReadFile(L"back\\中文目录\\binary"));
        EXPECT_EQ(std::string(196613, 'x'), temp.ReadFile(L"back\\repeated"));
        EXPECT_EQ("", temp.ReadFile(L"back\\empty"));
    }
}

}  // namespace
