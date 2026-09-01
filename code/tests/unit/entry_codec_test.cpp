// Copyright 2026 CBK Project. EntryMeta 编解码的单元测试。
//
// 索引区和 cbk-native 打包器共用这套编码，所以它错一个字节，
// 两处一起坏。用例集中在两件事：字段一个都不能丢，坏数据一定被挡住。

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "cbk/types.h"
#include "src/byte_io.h"
#include "src/entry_codec.h"

namespace {

cbk::EntryMeta MakeFullyPopulated() {
    cbk::EntryMeta meta;
    meta.id = 0x0123456789ABCDEFull;
    meta.relative_path = L"深\\一点\\的 路径\\文件名 with mixed 中英.txt";
    meta.type = cbk::FileType::kSymlinkDir;
    meta.attributes = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT;
    meta.creation_time = 0x01D9111111111111ull;
    meta.last_access_time = 0x01D9222222222222ull;
    meta.last_write_time = 0x01D9333333333333ull;
    meta.sddl = "O:S-1-5-21-1-2-3-1001G:S-1-5-18D:AI(A;;FA;;;SY)(A;;0x1200a9;;;BU)";
    meta.original_size = 0xFFFFFFFF01ull;
    meta.stored_size = 0x1234ull;
    meta.data_offset = 0xABCDEFull;
    meta.crc32 = 0xCAFEBABEu;
    meta.link_target = L"..\\相对目标";
    meta.link_is_relative = true;
    meta.reparse_tag = 0xA000000Cu;
    meta.hardlink_ref_id = 42;
    return meta;
}

void ExpectEqual(const cbk::EntryMeta& a, const cbk::EntryMeta& b) {
    EXPECT_EQ(a.id, b.id);
    EXPECT_EQ(a.relative_path, b.relative_path);
    EXPECT_EQ(a.type, b.type);
    EXPECT_EQ(a.attributes, b.attributes);
    EXPECT_EQ(a.creation_time, b.creation_time);
    EXPECT_EQ(a.last_access_time, b.last_access_time);
    EXPECT_EQ(a.last_write_time, b.last_write_time);
    EXPECT_EQ(a.sddl, b.sddl);
    EXPECT_EQ(a.original_size, b.original_size);
    EXPECT_EQ(a.stored_size, b.stored_size);
    EXPECT_EQ(a.data_offset, b.data_offset);
    EXPECT_EQ(a.crc32, b.crc32);
    EXPECT_EQ(a.link_target, b.link_target);
    EXPECT_EQ(a.link_is_relative, b.link_is_relative);
    EXPECT_EQ(a.reparse_tag, b.reparse_tag);
    EXPECT_EQ(a.hardlink_ref_id, b.hardlink_ref_id);
}

TEST(EntryCodec, RoundTripsEveryField) {
    const cbk::EntryMeta original = MakeFullyPopulated();

    cbk::VectorSink sink;
    cbk::WriteEntryMeta(original, sink);

    cbk::MemorySource source(sink.Buffer());
    cbk::EntryMeta parsed;
    bool end_of_stream = false;
    ASSERT_TRUE(cbk::ReadEntryMeta(source, &parsed, &end_of_stream));
    EXPECT_FALSE(end_of_stream);
    ExpectEqual(original, parsed);
}

TEST(EntryCodec, RoundTripsDefaultConstructedMeta) {
    const cbk::EntryMeta original;

    cbk::VectorSink sink;
    cbk::WriteEntryMeta(original, sink);

    cbk::MemorySource source(sink.Buffer());
    cbk::EntryMeta parsed;
    bool end_of_stream = false;
    ASSERT_TRUE(cbk::ReadEntryMeta(source, &parsed, &end_of_stream));
    ExpectEqual(original, parsed);
}

TEST(EntryCodec, TimestampsKeep100NsPrecision) {
    // 存 FILETIME 原值而不是 time_t。转成秒会把这个数抹平，
    // 往返测试就是靠这条钉住的。
    cbk::EntryMeta original;
    original.last_write_time = 0x01D9ABCDEF012345ull;

    cbk::VectorSink sink;
    cbk::WriteEntryMeta(original, sink);
    cbk::MemorySource source(sink.Buffer());
    cbk::EntryMeta parsed;
    bool end_of_stream = false;
    ASSERT_TRUE(cbk::ReadEntryMeta(source, &parsed, &end_of_stream));

    EXPECT_EQ(0x01D9ABCDEF012345ull, parsed.last_write_time);
}

TEST(EntryCodec, ReadsSeveralRecordsBackToBack) {
    cbk::VectorSink sink;
    for (uint64_t i = 0; i < 5; ++i) {
        cbk::EntryMeta meta;
        meta.id = i;
        meta.relative_path = L"文件" + std::to_wstring(i);
        cbk::WriteEntryMeta(meta, sink);
    }

    cbk::MemorySource source(sink.Buffer());
    for (uint64_t i = 0; i < 5; ++i) {
        cbk::EntryMeta parsed;
        bool end_of_stream = false;
        ASSERT_TRUE(cbk::ReadEntryMeta(source, &parsed, &end_of_stream)) << "第 " << i << " 条";
        EXPECT_EQ(i, parsed.id);
    }

    cbk::EntryMeta extra;
    bool end_of_stream = false;
    EXPECT_FALSE(cbk::ReadEntryMeta(source, &extra, &end_of_stream));
    EXPECT_TRUE(end_of_stream) << "读完之后必须报告流正常结束，而不是报损坏";
}

TEST(EntryCodec, EmptyStreamReportsCleanEnd) {
    cbk::MemorySource source(nullptr, 0);
    cbk::EntryMeta parsed;
    bool end_of_stream = false;
    EXPECT_FALSE(cbk::ReadEntryMeta(source, &parsed, &end_of_stream));
    EXPECT_TRUE(end_of_stream);
}

TEST(EntryCodec, TruncatedRecordIsCorruptionNotCleanEnd) {
    // 这两种情况必须分得开：流干净结束是正常的，读到一半没了是包坏了。
    cbk::VectorSink sink;
    cbk::WriteEntryMeta(MakeFullyPopulated(), sink);

    std::vector<uint8_t> truncated = sink.Buffer();
    truncated.resize(truncated.size() / 2);

    cbk::MemorySource source(truncated);
    cbk::EntryMeta parsed;
    bool end_of_stream = false;
    EXPECT_FALSE(cbk::ReadEntryMeta(source, &parsed, &end_of_stream));
    EXPECT_FALSE(end_of_stream);
}

TEST(EntryCodec, RejectsAbsurdRecordLength) {
    // 长度前缀被改坏成一个天文数字。不挡的话解析器会直接去申请 4 GB。
    std::vector<uint8_t> bytes;
    cbk::ByteWriter writer(&bytes);
    writer.WriteU32(0xFFFFFFF0u);
    writer.WriteZeros(16);

    cbk::MemorySource source(bytes);
    cbk::EntryMeta parsed;
    bool end_of_stream = false;
    EXPECT_FALSE(cbk::ReadEntryMeta(source, &parsed, &end_of_stream));
    EXPECT_FALSE(end_of_stream);
}

TEST(EntryCodec, RejectsZeroLengthRecord) {
    std::vector<uint8_t> bytes;
    cbk::ByteWriter writer(&bytes);
    writer.WriteU32(0);

    cbk::MemorySource source(bytes);
    cbk::EntryMeta parsed;
    bool end_of_stream = false;
    EXPECT_FALSE(cbk::ReadEntryMeta(source, &parsed, &end_of_stream));
    EXPECT_FALSE(end_of_stream);
}

TEST(EntryCodec, RejectsOutOfRangeFileType) {
    cbk::EntryMeta meta;
    cbk::VectorSink sink;
    cbk::WriteEntryMeta(meta, sink);

    std::vector<uint8_t> bytes = sink.Buffer();
    // 记录体的第 9 个字节是 type（前面是 4 字节长度前缀 + 8 字节 id）。
    ASSERT_GT(bytes.size(), 12u);
    bytes[12] = 250;

    cbk::MemorySource source(bytes);
    cbk::EntryMeta parsed;
    bool end_of_stream = false;
    EXPECT_FALSE(cbk::ReadEntryMeta(source, &parsed, &end_of_stream));
}

TEST(EntryCodec, PathsAreStoredAsUtf8) {
    // 容器里一律 UTF-8。写进去的字节数应该等于 UTF-8 长度，
    // 不是 UTF-16 的字节数——写错的话跨机器读会乱码。
    cbk::EntryMeta meta;
    meta.relative_path = L"中文";  // UTF-8 是 6 字节，UTF-16 是 4 字节

    cbk::VectorSink sink;
    cbk::WriteEntryMeta(meta, sink);
    const std::vector<uint8_t>& bytes = sink.Buffer();

    const std::vector<uint8_t> utf8_needle = {0xE4, 0xB8, 0xAD, 0xE6, 0x96, 0x87};
    const auto found =
        std::search(bytes.begin(), bytes.end(), utf8_needle.begin(), utf8_needle.end());
    EXPECT_NE(bytes.end(), found) << "路径没有按 UTF-8 存";
}

}  // namespace
