// Copyright 2026 CBK Project. CRC32 与小端读写的单元测试。
//
// 这两个组件是容器格式的地基，出一点错就是"包写得出来但读不回去"，
// 而且往往要等到还原阶段才暴露。所以用例写得密一些，尤其是边界：
// 空输入、越界、截断、流分块返回。

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "src/byte_io.h"
#include "src/crc32.h"

namespace {

// ============================================================ CRC32

TEST(Crc32, MatchesStandardCheckVector) {
    // IEEE 802.3 CRC-32 的标准校验值："123456789" -> 0xCBF43926。
    // 这个数字跟 zip / png / zlib 用的是同一个算法，可以拿外部工具对。
    const std::string input = "123456789";
    EXPECT_EQ(0xCBF43926u,
              cbk::ComputeCrc32(reinterpret_cast<const uint8_t*>(input.data()), input.size()));
}

TEST(Crc32, EmptyInputIsZero) {
    EXPECT_EQ(0u, cbk::ComputeCrc32(nullptr, 0));
    cbk::Crc32 crc;
    EXPECT_EQ(0u, crc.Value());
}

TEST(Crc32, ChunkedUpdateEqualsSingleShot) {
    // 备份时内容是 64 KB 一块读进来的，分块累积必须和一次性算完等价。
    std::vector<uint8_t> data(10000);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<uint8_t>(i * 31 + 7);
    }
    const uint32_t expected = cbk::ComputeCrc32(data.data(), data.size());

    for (size_t chunk : {size_t{1}, size_t{7}, size_t{64}, size_t{4096}}) {
        cbk::Crc32 crc;
        for (size_t offset = 0; offset < data.size(); offset += chunk) {
            const size_t len = (offset + chunk < data.size()) ? chunk : data.size() - offset;
            crc.Update(data.data() + offset, len);
        }
        EXPECT_EQ(expected, crc.Value()) << "分块大小 " << chunk;
    }
}

TEST(Crc32, DetectsSingleBitFlip) {
    std::vector<uint8_t> data(256, 0xAB);
    const uint32_t original = cbk::ComputeCrc32(data.data(), data.size());
    data[128] ^= 0x01;
    EXPECT_NE(original, cbk::ComputeCrc32(data.data(), data.size()));
}

TEST(Crc32, ResetStartsOver) {
    const std::string input = "123456789";
    cbk::Crc32 crc;
    crc.Update(reinterpret_cast<const uint8_t*>("garbage"), 7);
    crc.Reset();
    crc.Update(reinterpret_cast<const uint8_t*>(input.data()), input.size());
    EXPECT_EQ(0xCBF43926u, crc.Value());
}

TEST(Crc32, ValueCanBeTakenMidStream) {
    cbk::Crc32 crc;
    crc.Update(reinterpret_cast<const uint8_t*>("1234"), 4);
    const uint32_t midway = crc.Value();
    crc.Update(reinterpret_cast<const uint8_t*>("56789"), 5);
    EXPECT_EQ(0xCBF43926u, crc.Value());
    EXPECT_NE(midway, crc.Value());
}

// ============================================================ 小端往返

TEST(ByteIo, LittleEndianRoundTrip) {
    std::vector<uint8_t> buffer;
    cbk::ByteWriter writer(&buffer);
    writer.WriteU8(0x12);
    writer.WriteU16(0x3456);
    writer.WriteU32(0x789ABCDEu);
    writer.WriteU64(0x0123456789ABCDEFull);

    cbk::ByteReader reader(buffer);
    EXPECT_EQ(0x12u, reader.ReadU8());
    EXPECT_EQ(0x3456u, reader.ReadU16());
    EXPECT_EQ(0x789ABCDEu, reader.ReadU32());
    EXPECT_EQ(0x0123456789ABCDEFull, reader.ReadU64());
    EXPECT_TRUE(reader.IsOk());
    EXPECT_EQ(0u, reader.Remaining());
}

TEST(ByteIo, ByteOrderIsActuallyLittleEndian) {
    // 钉死字节序。换个大端架构编译时，这个用例会立刻挂——
    // 比"包读不出来"好查一万倍。
    std::vector<uint8_t> buffer;
    cbk::ByteWriter writer(&buffer);
    writer.WriteU32(0x11223344u);

    ASSERT_EQ(4u, buffer.size());
    EXPECT_EQ(0x44u, buffer[0]);
    EXPECT_EQ(0x33u, buffer[1]);
    EXPECT_EQ(0x22u, buffer[2]);
    EXPECT_EQ(0x11u, buffer[3]);
}

TEST(ByteIo, LengthPrefixedRoundTrip) {
    std::vector<uint8_t> buffer;
    cbk::ByteWriter writer(&buffer);
    writer.WriteLengthPrefixed("");
    writer.WriteLengthPrefixed("hello");
    writer.WriteLengthPrefixed(std::string("\0embedded\0nul", 13));

    cbk::ByteReader reader(buffer);
    std::string value;

    ASSERT_TRUE(reader.ReadLengthPrefixed(&value));
    EXPECT_TRUE(value.empty());

    ASSERT_TRUE(reader.ReadLengthPrefixed(&value));
    EXPECT_EQ("hello", value);

    // 路径转 UTF-8 之后理论上不会有内嵌 NUL，但长度前缀的意义就在于
    // 不依赖这个假设。
    ASSERT_TRUE(reader.ReadLengthPrefixed(&value));
    EXPECT_EQ(std::string("\0embedded\0nul", 13), value);

    EXPECT_TRUE(reader.IsOk());
}

TEST(ByteIo, ZerosAndBytes) {
    std::vector<uint8_t> buffer;
    cbk::ByteWriter writer(&buffer);
    writer.WriteZeros(3);
    const uint8_t payload[] = {0xAA, 0xBB};
    writer.WriteBytes(payload, sizeof(payload));

    ASSERT_EQ(5u, buffer.size());
    EXPECT_EQ(0u, buffer[0]);
    EXPECT_EQ(0u, buffer[2]);
    EXPECT_EQ(0xAAu, buffer[3]);
    EXPECT_EQ(0xBBu, buffer[4]);
}

// ============================================================ 越界与截断

TEST(ByteReader, OverreadSetsErrorInsteadOfCrashing) {
    // 容器文件可能被截断、被改坏、或者根本不是我们生成的。
    // 读越界必须变成"这个包坏了"，而不是崩溃或读到垃圾。
    const std::vector<uint8_t> buffer = {0x01, 0x02};
    cbk::ByteReader reader(buffer);

    EXPECT_EQ(0x0201u, reader.ReadU16());
    EXPECT_TRUE(reader.IsOk());

    EXPECT_EQ(0u, reader.ReadU32());
    EXPECT_FALSE(reader.IsOk());
}

TEST(ByteReader, StaysFailedOnceOverread) {
    const std::vector<uint8_t> buffer = {0x01};
    cbk::ByteReader reader(buffer);
    reader.ReadU64();
    EXPECT_FALSE(reader.IsOk());

    // 出错之后不再前进，后续读取一律返回 0。这样调用方可以连读一串字段
    // 最后只查一次 IsOk()，不用每行都判断。
    EXPECT_EQ(0u, reader.ReadU8());
    EXPECT_FALSE(reader.IsOk());
    EXPECT_EQ(0u, reader.Remaining());
}

TEST(ByteReader, TruncatedLengthPrefixIsRejected) {
    // 长度前缀说有 100 字节，实际只剩 2 字节。这正是被截断的包的样子。
    std::vector<uint8_t> buffer;
    cbk::ByteWriter writer(&buffer);
    writer.WriteU32(100);
    writer.WriteU8(0xAA);
    writer.WriteU8(0xBB);

    cbk::ByteReader reader(buffer);
    std::string value;
    EXPECT_FALSE(reader.ReadLengthPrefixed(&value));
    EXPECT_FALSE(reader.IsOk());
}

TEST(ByteReader, SkipRespectsBounds) {
    const std::vector<uint8_t> buffer = {1, 2, 3, 4};
    cbk::ByteReader reader(buffer);
    reader.Skip(2);
    EXPECT_TRUE(reader.IsOk());
    EXPECT_EQ(2u, reader.Offset());

    reader.Skip(99);
    EXPECT_FALSE(reader.IsOk());
}

TEST(ByteReader, EmptyBufferIsUsableButYieldsNothing) {
    cbk::ByteReader reader(nullptr, 0);
    EXPECT_TRUE(reader.IsOk());
    EXPECT_EQ(0u, reader.Remaining());
    EXPECT_EQ(0u, reader.ReadU8());
    EXPECT_FALSE(reader.IsOk());
}

// ============================================================ ISink / ISource

TEST(VectorSink, CollectsEverythingWritten) {
    cbk::VectorSink sink;
    const uint8_t first[] = {1, 2, 3};
    const uint8_t second[] = {4, 5};
    sink.Write(first, sizeof(first));
    sink.Write(second, sizeof(second));

    const std::vector<uint8_t> expected = {1, 2, 3, 4, 5};
    EXPECT_EQ(expected, sink.Buffer());
}

TEST(VectorSink, IgnoresEmptyWrites) {
    cbk::VectorSink sink;
    sink.Write(nullptr, 0);
    const uint8_t byte = 7;
    sink.Write(&byte, 0);
    EXPECT_TRUE(sink.Buffer().empty());
}

TEST(MemorySource, ReadsThenReportsEnd) {
    const std::vector<uint8_t> data = {1, 2, 3, 4, 5};
    cbk::MemorySource source(data);

    uint8_t buffer[3] = {};
    EXPECT_EQ(3u, source.Read(buffer, 3));
    EXPECT_EQ(2u, source.Read(buffer, 3));  // 只剩 2 个
    EXPECT_EQ(0u, source.Read(buffer, 3));  // 结束
}

/// 每次最多吐 1 字节的 source，专门用来逼出"没有循环读满"的 bug。
class DribblingSource : public cbk::ISource {
public:
    explicit DribblingSource(std::vector<uint8_t> data) : data_(std::move(data)) {}

    size_t Read(uint8_t* buf, size_t len) override {
        if (offset_ >= data_.size() || len == 0) return 0;
        buf[0] = data_[offset_++];
        return 1;
    }

private:
    std::vector<uint8_t> data_;
    size_t offset_ = 0;
};

TEST(ReadExact, LoopsUntilFilled) {
    // ISource 的契约是"返回值小于 len 不代表结束"。漏掉循环的 bug
    // 在小文件上永远不出现，一上大文件就随机解析失败。
    DribblingSource source(std::vector<uint8_t>{9, 8, 7, 6});
    uint8_t buffer[4] = {};
    EXPECT_TRUE(cbk::ReadExact(source, buffer, 4));
    EXPECT_EQ(9, buffer[0]);
    EXPECT_EQ(6, buffer[3]);
}

TEST(ReadExact, ReportsFailureWhenStreamEndsEarly) {
    DribblingSource source(std::vector<uint8_t>{1, 2});
    uint8_t buffer[4] = {};
    EXPECT_FALSE(cbk::ReadExact(source, buffer, 4));
}

TEST(ReadExact, ZeroLengthAlwaysSucceeds) {
    DribblingSource source(std::vector<uint8_t>{});
    EXPECT_TRUE(cbk::ReadExact(source, nullptr, 0));
}

}  // namespace
