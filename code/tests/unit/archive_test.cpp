// Copyright 2026 CBK Project. .cbk 容器读写的单元测试。
//
// 重点在两头：正常往返要一字节不差，异常输入要被干净地拒绝。
// 后者比前者更重要——写坏的包能被识别出来，总好过还原到一半才发现。

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "cbk/types.h"
#include "src/archive.h"
#include "src/byte_io.h"
#include "src/platform_win.h"
#include "unit/temp_dir.h"

namespace {

namespace pf = cbk::platform;

cbk::PipelineDesc NativeOnly() {
    cbk::PipelineDesc pipeline;
    pipeline.packer = "cbk-native";
    return pipeline;
}

std::vector<cbk::EntryMeta> MakeEntries() {
    std::vector<cbk::EntryMeta> entries;

    cbk::EntryMeta file;
    file.id = 0;
    file.relative_path = L"文档\\报告.txt";
    file.type = cbk::FileType::kRegular;
    file.attributes = FILE_ATTRIBUTE_ARCHIVE;
    file.creation_time = 0x01D9000000000001ull;
    file.last_access_time = 0x01D9000000000002ull;
    file.last_write_time = 0x01D9000000000003ull;
    file.original_size = 1234;
    file.stored_size = 1000;
    file.data_offset = 0;
    file.crc32 = 0xDEADBEEFu;
    file.sddl = "O:BAG:SYD:(A;;FA;;;SY)";
    entries.push_back(file);

    cbk::EntryMeta dir;
    dir.id = 1;
    dir.relative_path = L"文档";
    dir.type = cbk::FileType::kDirectory;
    dir.attributes = FILE_ATTRIBUTE_DIRECTORY;
    entries.push_back(dir);

    return entries;
}

/// 直接往容器文件的某个偏移写一个字节，用来模拟损坏。
void PokeByte(const std::wstring& path, uint64_t offset, uint8_t value) {
    pf::ScopedHandle handle = pf::OpenPath(path, GENERIC_READ | GENERIC_WRITE, OPEN_EXISTING, true);
    ASSERT_TRUE(handle.IsValid());
    LARGE_INTEGER position;
    position.QuadPart = static_cast<LONGLONG>(offset);
    ASSERT_NE(0, SetFilePointerEx(handle.Get(), position, nullptr, FILE_BEGIN));
    DWORD written = 0;
    ASSERT_NE(0, WriteFile(handle.Get(), &value, 1, &written, nullptr));
}

/// 把文件截短 count 字节。
void TruncateBy(const std::wstring& path, uint64_t count) {
    pf::ScopedHandle handle = pf::OpenPath(path, GENERIC_READ | GENERIC_WRITE, OPEN_EXISTING, true);
    ASSERT_TRUE(handle.IsValid());
    LARGE_INTEGER size;
    ASSERT_NE(0, GetFileSizeEx(handle.Get(), &size));
    LARGE_INTEGER shorter;
    shorter.QuadPart = size.QuadPart - static_cast<LONGLONG>(count);
    ASSERT_NE(0, SetFilePointerEx(handle.Get(), shorter, nullptr, FILE_BEGIN));
    ASSERT_NE(0, SetEndOfFile(handle.Get()));
}

// ============================================================ PipelineDesc

TEST(PipelineDesc, RoundTripsWithStages) {
    cbk::PipelineDesc original;
    original.packer = "tar";
    original.stages = {"lz77", "huffman", "aes128-cbc"};

    const std::string text = original.Serialize();
    EXPECT_EQ("packer=tar;stages=lz77,huffman,aes128-cbc", text);

    cbk::PipelineDesc parsed;
    ASSERT_TRUE(cbk::PipelineDesc::Parse(text, &parsed));
    EXPECT_EQ(original.packer, parsed.packer);
    EXPECT_EQ(original.stages, parsed.stages);
}

TEST(PipelineDesc, RoundTripsWithNoStages) {
    cbk::PipelineDesc original = NativeOnly();
    const std::string text = original.Serialize();
    EXPECT_EQ("packer=cbk-native;stages=", text);

    cbk::PipelineDesc parsed;
    ASSERT_TRUE(cbk::PipelineDesc::Parse(text, &parsed));
    EXPECT_EQ("cbk-native", parsed.packer);
    EXPECT_TRUE(parsed.stages.empty());
}

TEST(PipelineDesc, RejectsMalformedText) {
    cbk::PipelineDesc parsed;
    EXPECT_FALSE(cbk::PipelineDesc::Parse("", &parsed));
    EXPECT_FALSE(cbk::PipelineDesc::Parse("packer=tar", &parsed));              // 缺 stages 段
    EXPECT_FALSE(cbk::PipelineDesc::Parse("stages=;packer=tar", &parsed));      // 顺序反了
    EXPECT_FALSE(cbk::PipelineDesc::Parse("packer=;stages=", &parsed));         // 空打包器名
    EXPECT_FALSE(cbk::PipelineDesc::Parse("packer=tar;stages=a,,b", &parsed));  // 空算法名
}

TEST(PipelineDesc, RejectsNamesWithDelimiterCharacters) {
    // 算法名字符集故意收得很紧，这样解析器永远不需要考虑转义。
    // 源根路径不并进这一段，也正是因为路径里可能出现这些字符。
    cbk::PipelineDesc parsed;
    EXPECT_FALSE(cbk::PipelineDesc::Parse("packer=a;b;stages=", &parsed));
    EXPECT_FALSE(cbk::PipelineDesc::Parse("packer=a=b;stages=", &parsed));
    EXPECT_FALSE(cbk::PipelineDesc::Parse("packer=TAR;stages=", &parsed));  // 大写不收
}

// ============================================================ 索引区

TEST(Index, RoundTrips) {
    const std::vector<cbk::EntryMeta> original = MakeEntries();

    cbk::VectorSink sink;
    cbk::WriteIndex(original, sink);

    cbk::MemorySource source(sink.Buffer());
    std::vector<cbk::EntryMeta> parsed;
    ASSERT_TRUE(cbk::ReadIndex(source, original.size(), &parsed));

    ASSERT_EQ(original.size(), parsed.size());
    EXPECT_EQ(original[0].relative_path, parsed[0].relative_path);
    EXPECT_EQ(original[0].sddl, parsed[0].sddl);
    EXPECT_EQ(original[0].crc32, parsed[0].crc32);
    EXPECT_EQ(original[0].last_write_time, parsed[0].last_write_time);
    EXPECT_EQ(original[1].type, parsed[1].type);
}

TEST(Index, RejectsCountMismatch) {
    const std::vector<cbk::EntryMeta> original = MakeEntries();
    cbk::VectorSink sink;
    cbk::WriteIndex(original, sink);

    cbk::MemorySource source(sink.Buffer());
    std::vector<cbk::EntryMeta> parsed;
    // 头部说有 5 条，索引里只有 2 条——包被改过或者写了一半。
    EXPECT_FALSE(cbk::ReadIndex(source, 5, &parsed));
}

TEST(Index, EmptyIndexIsValid) {
    cbk::VectorSink sink;
    cbk::WriteIndex({}, sink);
    EXPECT_TRUE(sink.Buffer().empty());

    cbk::MemorySource source(sink.Buffer());
    std::vector<cbk::EntryMeta> parsed;
    EXPECT_TRUE(cbk::ReadIndex(source, 0, &parsed));
    EXPECT_TRUE(parsed.empty());
}

// ============================================================ 容器往返

TEST(Archive, RoundTripsHeaderDataAndIndex) {
    cbk_test::TempDir temp;
    const std::wstring archive_path = temp.At(L"round.cbk");
    const std::wstring source_root = L"D:\\some\\源目录";
    const std::string payload = "数据区的内容，随便写点什么。";
    const std::vector<cbk::EntryMeta> entries = MakeEntries();

    {
        cbk::ArchiveWriter writer;
        std::wstring error;
        ASSERT_EQ(cbk::Status::kOk, writer.Open(archive_path, source_root, NativeOnly(), &error))
            << cbk::ToUtf8(error);

        writer.DataSink().Write(reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
        writer.EndData();
        cbk::WriteIndex(entries, writer.IndexSink());

        ASSERT_EQ(cbk::Status::kOk, writer.Close(entries.size(), 4321, &error))
            << cbk::ToUtf8(error);
    }

    cbk::ArchiveReader reader;
    std::wstring error;
    ASSERT_EQ(cbk::Status::kOk, reader.Open(archive_path, &error)) << cbk::ToUtf8(error);

    const cbk::ArchiveHeader& header = reader.Header();
    EXPECT_EQ(cbk::kFormatVersion, header.format_version);
    EXPECT_EQ(entries.size(), header.entry_count);
    EXPECT_EQ(payload.size(), header.data_size);
    EXPECT_EQ(4321u, header.total_original_bytes);
    EXPECT_EQ(source_root, header.source_root);
    EXPECT_EQ("cbk-native", header.pipeline.packer);
    EXPECT_TRUE(header.pipeline.stages.empty());
    EXPECT_NE(0u, header.created_at);

    // 布局不变式：dataOffset == 128 + pipelineDescLen + sourceRootLen
    const uint64_t expected_offset =
        cbk::kFileHeaderSize + NativeOnly().Serialize().size() + cbk::ToUtf8(source_root).size();
    EXPECT_EQ(expected_offset, header.data_offset);
    EXPECT_EQ(header.data_offset + header.data_size, header.index_offset);

    std::unique_ptr<cbk::ISource> data = reader.OpenData();
    ASSERT_NE(nullptr, data);
    std::string read_back(payload.size(), '\0');
    ASSERT_TRUE(cbk::ReadExact(*data, reinterpret_cast<uint8_t*>(&read_back[0]), read_back.size()));
    EXPECT_EQ(payload, read_back);

    std::unique_ptr<cbk::ISource> index = reader.OpenIndex();
    ASSERT_NE(nullptr, index);
    std::vector<cbk::EntryMeta> parsed;
    ASSERT_TRUE(cbk::ReadIndex(*index, header.entry_count, &parsed));
    ASSERT_EQ(entries.size(), parsed.size());
    EXPECT_EQ(entries[0].relative_path, parsed[0].relative_path);

    EXPECT_EQ(cbk::Status::kOk, reader.Verify(&error)) << cbk::ToUtf8(error);
}

TEST(Archive, SourceRootSurvivesDelimiterCharacters) {
    // 这条正是"源根不并进 PipelineDesc"的理由：分号、等号、逗号
    // 在 Windows 路径里都是合法字符，塞进那行 ASCII 就得设计转义规则。
    cbk_test::TempDir temp;
    const std::wstring archive_path = temp.At(L"weird.cbk");
    const std::wstring nasty_root = L"D:\\a;b\\c=d\\e,f\\中文 目录";

    {
        cbk::ArchiveWriter writer;
        std::wstring error;
        ASSERT_EQ(cbk::Status::kOk, writer.Open(archive_path, nasty_root, NativeOnly(), &error));
        writer.EndData();
        ASSERT_EQ(cbk::Status::kOk, writer.Close(0, 0, &error));
    }

    cbk::ArchiveReader reader;
    std::wstring error;
    ASSERT_EQ(cbk::Status::kOk, reader.Open(archive_path, &error)) << cbk::ToUtf8(error);
    EXPECT_EQ(nasty_root, reader.Header().source_root);
}

TEST(Archive, EmptyArchiveIsValid) {
    cbk_test::TempDir temp;
    const std::wstring archive_path = temp.At(L"empty.cbk");

    {
        cbk::ArchiveWriter writer;
        std::wstring error;
        ASSERT_EQ(cbk::Status::kOk, writer.Open(archive_path, L"D:\\空", NativeOnly(), &error));
        writer.EndData();
        ASSERT_EQ(cbk::Status::kOk, writer.Close(0, 0, &error));
    }

    cbk::ArchiveReader reader;
    std::wstring error;
    ASSERT_EQ(cbk::Status::kOk, reader.Open(archive_path, &error));
    EXPECT_EQ(0u, reader.Header().entry_count);
    EXPECT_EQ(0u, reader.Header().data_size);
    EXPECT_EQ(cbk::Status::kOk, reader.Verify(&error));
}

TEST(Archive, LargeDataRegionStreamsCorrectly) {
    cbk_test::TempDir temp;
    const std::wstring archive_path = temp.At(L"big.cbk");
    std::vector<uint8_t> payload(3 * cbk::kIoBlockSize + 777);
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<uint8_t>((i * 13 + 5) & 0xFF);
    }

    {
        cbk::ArchiveWriter writer;
        std::wstring error;
        ASSERT_EQ(cbk::Status::kOk, writer.Open(archive_path, L"D:\\big", NativeOnly(), &error));
        // 分块写进去，模拟引擎的 64 KB 循环。
        for (size_t offset = 0; offset < payload.size(); offset += cbk::kIoBlockSize) {
            const size_t len = (offset + cbk::kIoBlockSize < payload.size())
                                   ? cbk::kIoBlockSize
                                   : payload.size() - offset;
            writer.DataSink().Write(payload.data() + offset, len);
        }
        writer.EndData();
        ASSERT_EQ(cbk::Status::kOk, writer.Close(0, payload.size(), &error));
    }

    cbk::ArchiveReader reader;
    std::wstring error;
    ASSERT_EQ(cbk::Status::kOk, reader.Open(archive_path, &error));
    EXPECT_EQ(payload.size(), reader.Header().data_size);

    std::unique_ptr<cbk::ISource> data = reader.OpenData();
    ASSERT_NE(nullptr, data);
    std::vector<uint8_t> read_back(payload.size());
    ASSERT_TRUE(cbk::ReadExact(*data, read_back.data(), read_back.size()));
    EXPECT_EQ(payload, read_back);
    EXPECT_EQ(cbk::Status::kOk, reader.Verify(&error));
}

// ============================================================ 坏包的拒绝

TEST(Archive, AbortDeletesHalfWrittenFile) {
    cbk_test::TempDir temp;
    const std::wstring archive_path = temp.At(L"aborted.cbk");

    {
        cbk::ArchiveWriter writer;
        std::wstring error;
        ASSERT_EQ(cbk::Status::kOk, writer.Open(archive_path, L"D:\\x", NativeOnly(), &error));
        const uint8_t junk[] = {1, 2, 3};
        writer.DataSink().Write(junk, sizeof(junk));
        // 不调 Close，直接析构——这就是取消或抛异常时会发生的事。
    }

    // 留下一个残缺的 .cbk 比没有文件更糟：它看着像个能用的备份。
    EXPECT_EQ(INVALID_FILE_ATTRIBUTES,
              GetFileAttributesW(pf::ToExtendedPath(archive_path).c_str()));
}

TEST(Archive, RejectsFileThatIsTooSmall) {
    cbk_test::TempDir temp;
    const std::wstring path = temp.MakeFile(L"tiny.cbk", "not an archive");

    cbk::ArchiveReader reader;
    std::wstring error;
    EXPECT_EQ(cbk::Status::kFailed, reader.Open(path, &error));
    EXPECT_FALSE(error.empty());
}

TEST(Archive, RejectsWrongMagic) {
    cbk_test::TempDir temp;
    const std::wstring path = temp.At(L"magic.cbk");
    {
        cbk::ArchiveWriter writer;
        std::wstring error;
        ASSERT_EQ(cbk::Status::kOk, writer.Open(path, L"D:\\x", NativeOnly(), &error));
        writer.EndData();
        ASSERT_EQ(cbk::Status::kOk, writer.Close(0, 0, &error));
    }
    PokeByte(path, 0, 'X');

    cbk::ArchiveReader reader;
    std::wstring error;
    EXPECT_EQ(cbk::Status::kFailed, reader.Open(path, &error));
}

TEST(Archive, RejectsUnknownFormatVersion) {
    cbk_test::TempDir temp;
    const std::wstring path = temp.At(L"version.cbk");
    {
        cbk::ArchiveWriter writer;
        std::wstring error;
        ASSERT_EQ(cbk::Status::kOk, writer.Open(path, L"D:\\x", NativeOnly(), &error));
        writer.EndData();
        ASSERT_EQ(cbk::Status::kOk, writer.Close(0, 0, &error));
    }
    PokeByte(path, 4, 99);  // formatVersion 低字节

    cbk::ArchiveReader reader;
    std::wstring error;
    EXPECT_EQ(cbk::Status::kFailed, reader.Open(path, &error));
}

TEST(Archive, RejectsTruncatedFile) {
    cbk_test::TempDir temp;
    const std::wstring path = temp.At(L"cut.cbk");
    {
        cbk::ArchiveWriter writer;
        std::wstring error;
        ASSERT_EQ(cbk::Status::kOk, writer.Open(path, L"D:\\x", NativeOnly(), &error));
        const std::string payload(500, 'z');
        writer.DataSink().Write(reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
        writer.EndData();
        ASSERT_EQ(cbk::Status::kOk, writer.Close(0, 0, &error));
    }
    TruncateBy(path, 20);

    cbk::ArchiveReader reader;
    std::wstring error;
    EXPECT_EQ(cbk::Status::kFailed, reader.Open(path, &error));
}

TEST(Archive, VerifyCatchesCorruptedDataRegion) {
    cbk_test::TempDir temp;
    const std::wstring path = temp.At(L"corrupt.cbk");
    const std::string payload(500, 'z');
    uint64_t data_offset = 0;
    {
        cbk::ArchiveWriter writer;
        std::wstring error;
        ASSERT_EQ(cbk::Status::kOk, writer.Open(path, L"D:\\x", NativeOnly(), &error));
        writer.DataSink().Write(reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
        writer.EndData();
        ASSERT_EQ(cbk::Status::kOk, writer.Close(0, 0, &error));
    }

    {
        // Open 只做结构检查，改一个数据字节它是发现不了的——
        // 那正是 Verify 存在的意义。
        cbk::ArchiveReader reader;
        std::wstring error;
        ASSERT_EQ(cbk::Status::kOk, reader.Open(path, &error));
        data_offset = reader.Header().data_offset;
        EXPECT_EQ(cbk::Status::kOk, reader.Verify(&error));
    }

    PokeByte(path, data_offset + 100, 'q');

    cbk::ArchiveReader reader;
    std::wstring error;
    ASSERT_EQ(cbk::Status::kOk, reader.Open(path, &error)) << "结构没坏，Open 应该还能过";
    EXPECT_EQ(cbk::Status::kFailed, reader.Verify(&error));
    EXPECT_FALSE(error.empty());
}

TEST(Archive, RejectsSourceRootThatOverflowsHeaderField) {
    // sourceRootLen 只有 2 字节。超了必须明确报错，绝不静默截断——
    // 截断出来的包写得出来，还原时才发现根路径是错的。
    cbk_test::TempDir temp;
    const std::wstring huge_root(40000, L'中');  // UTF-8 之后 12 万字节

    cbk::ArchiveWriter writer;
    std::wstring error;
    EXPECT_EQ(cbk::Status::kBadArgs,
              writer.Open(temp.At(L"overflow.cbk"), huge_root, NativeOnly(), &error));
    EXPECT_FALSE(error.empty());
}

}  // namespace
