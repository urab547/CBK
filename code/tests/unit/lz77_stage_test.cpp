// Copyright 2026 CBK Project.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "cbk/engine.h"
#include "cbk/packer.h"
#include "cbk/stage.h"
#include "src/crc32.h"
#include "src/stages/lz77_stage.h"
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
    cbk::Lz77StageFactory factory;
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

// 手工构造负载，避免只用同一对编码器/解码器验证往返。
Bytes Frame(const Bytes& original, const Bytes& payload) {
    Bytes bytes{'C', 'B', 'K', 'L', 'Z', '0', '0', '1'};
    bytes.resize(21, 0);
    bytes[8] = 2;
    Put32(&bytes, 9, static_cast<uint32_t>(original.size()));
    Put32(&bytes, 13, static_cast<uint32_t>(payload.size()));
    Put32(&bytes, 17, cbk::ComputeCrc32(original.data(), original.size()));
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    bytes.resize(bytes.size() + 13, 0);
    return bytes;
}

Bytes RepeatedAbc() {
    const std::string text = "abcabcabcabcabcabcabcabc";
    return Bytes(text.begin(), text.end());
}

TEST(Lz77Stage, RegistersBothDirectionsAsCompression) {
    cbk::RegisterBuiltinStages();
    auto* factory = cbk::StageRegistry::Instance().Find("lz77");
    ASSERT_NE(nullptr, factory);
    EXPECT_EQ(cbk::StageKind::kCompress, factory->Kind());
    EXPECT_EQ("lz77", factory->CreateForward()->Name());
    EXPECT_EQ("lz77", factory->CreateInverse()->Name());
}

TEST(Lz77Stage, RoundTripsBoundarySizesWithArbitraryChunks) {
    for (size_t size : {0u, 1u, 2u, 3u, 258u, 32768u, 65535u, 65536u, 65537u, 196625u}) {
        SCOPED_TRACE(size);
        const auto input = RandomBytes(size);
        const auto encoded = Transform(input, true);
        EXPECT_EQ(input, Transform(encoded, false, 1));
        EXPECT_EQ(encoded, Transform(input, true, 65536));
    }
}

TEST(Lz77Stage, CompressesSingleSymbolsAllByteValuesAndText) {
    std::vector<Bytes> samples{Bytes(131073, 0), Bytes(131073, 255), Bytes(131073, 'A')};
    Bytes all_bytes(131072);
    for (size_t i = 0; i < all_bytes.size(); ++i) all_bytes[i] = static_cast<uint8_t>(i);
    samples.push_back(all_bytes);
    const std::string sentence = "LZ77 stores repeated sequences as backward references.\n";
    Bytes text;
    for (int i = 0; i < 3000; ++i) text.insert(text.end(), sentence.begin(), sentence.end());
    samples.push_back(text);
    for (const auto& input : samples) {
        const auto encoded = Transform(input, true, 1);
        EXPECT_LT(encoded.size(), input.size() / 10);
        EXPECT_EQ(input, Transform(encoded, false, 13));
        EXPECT_EQ(encoded, Transform(Transform(encoded, true), false));
    }
}

TEST(Lz77Stage, RawFallbackBoundsExpansion) {
    const auto input = RandomBytes(65536);
    const auto encoded = Transform(input, true);
    ASSERT_EQ(input.size() + 34, encoded.size());
    EXPECT_EQ(1, encoded[8]);
}

TEST(Lz77Stage, MatchesIndependentOverlappingReferenceFixture) {
    const auto original = RepeatedAbc();
    // 三个字面量，然后向后 3 字节复制 21 字节；第 4 个标志位为 1。
    const auto fixture = Frame(original, {8, 'a', 'b', 'c', 3, 0, 18});
    EXPECT_EQ(original, Transform(fixture, false, 1));
    EXPECT_EQ(fixture, Transform(original, true));
    EXPECT_EQ(Bytes(259, 'A'), Transform(Frame(Bytes(259, 'A'), {2, 'A', 1, 0, 255}), false));
}

TEST(Lz77Stage, AcceptsMaximumWindowDistanceAndRejectsLargerDistance) {
    Bytes original(32768, 'A');
    Bytes payload;
    // 先用完整字面量组填满窗口，再用 29 个最大长度匹配；最后一组只使用 5 位。
    for (size_t i = 0; i < 4096; ++i) {
        payload.push_back(0);
        payload.insert(payload.end(), 8, 'A');
    }
    for (unsigned group = 0; group < 4; ++group) {
        const unsigned count = group == 3 ? 5 : 8;
        payload.push_back(static_cast<uint8_t>((1u << count) - 1));
        for (unsigned token = 0; token < count; ++token) {
            payload.insert(payload.end(), {0, 128, 255});
            original.insert(original.end(), 258, 'A');
        }
    }
    EXPECT_EQ(original, Transform(Frame(original, payload), false));
    payload[36865] = 1;  // distance = 32769。
    EXPECT_THROW(Transform(Frame(original, payload), false), std::runtime_error);
}

TEST(Lz77Stage, FindsFarMatchesAndResetsHistoryAtBlockBoundary) {
    auto input = RandomBytes(32768);
    const Bytes prefix(input.begin(), input.begin() + 258);
    input.insert(input.end(), prefix.begin(), prefix.end());
    input.resize(65536, 'x');
    const auto first = Transform(input, true);
    ASSERT_EQ(2, first[8]);
    EXPECT_EQ(input, Transform(first, false));
    // 独立编码两块的负载必须等于一次编码两块，证明块间不引用历史。
    const auto second_input = RandomBytes(65536);
    const auto second = Transform(second_input, true);
    Bytes expected(first.begin(), first.end() - 13);
    expected.insert(expected.end(), second.begin() + 8, second.end());
    input.insert(input.end(), second_input.begin(), second_input.end());
    EXPECT_EQ(expected, Transform(input, true));
    EXPECT_EQ(input, Transform(expected, false));
}

TEST(Lz77Stage, FlushesFullBlocksBeforeFinish) {
    cbk::Lz77StageFactory factory;
    auto encoder = factory.CreateForward();
    Buffer compressed;
    const Bytes input(65536, 'x');
    encoder->Process(input.data(), input.size(), compressed);
    auto decoder = factory.CreateInverse();
    Buffer restored;
    decoder->Process(compressed.bytes.data(), compressed.bytes.size(), restored);
    EXPECT_EQ(input, restored.bytes);
    EXPECT_LE(restored.largest_write, 65536u);
    const size_t previous = compressed.bytes.size();
    encoder->Finish(compressed);
    const size_t tail = compressed.bytes.size() - previous;
    decoder->Process(compressed.bytes.data() + previous, tail, restored);
    EXPECT_NO_THROW(decoder->Finish(restored));
}

TEST(Lz77Stage, RejectsEveryTruncationOfRawAndCompressedStreams) {
    for (const auto& input : {Bytes{1, 2, 3}, RepeatedAbc()}) {
        const auto encoded = Transform(input, true);
        for (size_t length = 0; length < encoded.size(); ++length) {
            EXPECT_THROW(Transform(Bytes(encoded.begin(), encoded.begin() + length), false),
                         std::runtime_error);
        }
    }
}

TEST(Lz77Stage, RejectsInvalidHeadersAndEndMarkers) {
    const auto fixture = Transform(RepeatedAbc(), true);
    for (size_t offset : {0u, 7u, 8u, 12u, 16u}) {
        auto bytes = fixture;
        bytes[offset] = 255;
        EXPECT_THROW(Transform(bytes, false), std::runtime_error);
    }
    for (size_t offset : {9u, 13u}) {
        auto bytes = fixture;
        Put32(&bytes, offset, 0);
        EXPECT_THROW(Transform(bytes, false), std::runtime_error);
    }
    auto bytes = Transform({}, true);
    bytes[17] = 1;
    EXPECT_THROW(Transform(bytes, false), std::runtime_error);
    bytes = Transform({}, true);
    bytes.push_back(0);
    EXPECT_THROW(Transform(bytes, false), std::runtime_error);
}

TEST(Lz77Stage, RejectsInvalidReferencesTokensAndUnusedPayload) {
    const auto original = RepeatedAbc();
    const std::vector<Bytes> invalid{
        {8, 'a', 'b', 'c', 0, 0, 18},   // 距离为零。
        {8, 'a', 'b', 'c', 4, 0, 18},   // 引用尚未输出的内容。
        {8, 'a', 'b', 'c', 3, 0, 19},   // 超过声明的输出长度。
        {8, 'a', 'b', 'c', 3, 0},       // 引用截断。
        {0},                           // 缺少字面量。
        {0, 'a'},                      // 后续字面量截断。
        {128 + 8, 'a', 'b', 'c', 3, 0, 18},  // 未使用标志位非零。
        {8, 'a', 'b', 'c', 3, 0, 18, 0},     // 负载多余字节。
    };
    for (const auto& payload : invalid) {
        EXPECT_THROW(Transform(Frame(original, payload), false), std::runtime_error);
    }
}

TEST(Lz77Stage, RejectsChecksumFailureBeforeOutput) {
    for (auto bytes : {Transform(RepeatedAbc(), true), Transform(Bytes{1, 2, 3}, true)}) {
        bytes[17] ^= 1;
        cbk::Lz77StageFactory factory;
        auto decoder = factory.CreateInverse();
        Buffer output;
        EXPECT_THROW(decoder->Process(bytes.data(), bytes.size(), output), std::runtime_error);
        EXPECT_TRUE(output.bytes.empty());
    }
}

TEST(Lz77Stage, RejectsCallsAfterFinish) {
    cbk::Lz77StageFactory factory;
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

TEST(Lz77Stage, EngineRoundTripsAllPackersWithLz77AndHuffmanComposition) {
    cbk::RegisterBuiltinPackers();
    cbk::RegisterBuiltinStages();
    for (const auto* packer : {"cbk-native", "tar", "cpio"}) {
        for (bool huffman : {false, true}) {
            SCOPED_TRACE(packer);
            SCOPED_TRACE(huffman);
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
            backup.stages = {"lz77"};
            if (huffman) backup.stages.push_back("huffman");
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
}

}  // namespace
