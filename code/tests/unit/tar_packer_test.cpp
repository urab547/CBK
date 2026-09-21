// Copyright 2026 CBK Project. tar 格式、异常输入与引擎接入测试。
#include <gtest/gtest.h>
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "cbk/engine.h"
#include "cbk/packer.h"
#include "cbk/text.h"
#include "src/byte_io.h"
#include "src/entry_codec.h"
#include "src/packers/tar_packer.h"
#include "unit/temp_dir.h"

namespace {

struct Item {
    cbk::EntryMeta meta;
    std::string data;
};

Item File(const std::wstring& name, const std::string& data, uint64_t id = 0) {
    Item item;
    item.meta.id = id;
    item.meta.relative_path = name;
    item.meta.original_size = data.size();
    item.data = data;
    return item;
}

std::vector<uint8_t> Pack(const std::vector<Item>& items) {
    cbk::TarPacker packer;
    cbk::VectorSink sink;
    for (const Item& item : items) {
        packer.BeginEntry(item.meta, sink);
        // 不按 512 或 64 KB 对齐，验证多次写入的累计长度。
        for (size_t pos = 0; pos < item.data.size(); pos += 7) {
            packer.WriteData(reinterpret_cast<const uint8_t*>(item.data.data() + pos),
                              std::min<size_t>(7, item.data.size() - pos), sink);
        }
        packer.EndEntry(sink);
    }
    packer.Finish(sink);
    return sink.Buffer();
}

// 故意每次只返回 3 字节：ISource 允许短读，不能把短读误认为 EOF。
class ShortSource : public cbk::ISource {
public:
    explicit ShortSource(const std::vector<uint8_t>& bytes) : source_(bytes) {}
    size_t Read(uint8_t* out, size_t len) override {
        const size_t chunk = std::min<size_t>(3, len);
        return source_.Read(out, chunk);
    }

private:
    cbk::MemorySource source_;
};

std::vector<Item> Unpack(const std::vector<uint8_t>& bytes) {
    ShortSource source(bytes);
    cbk::TarPacker packer;
    std::vector<Item> items;
    packer.Unpack(source, [&items](const cbk::EntryMeta& meta) { items.push_back({meta, ""}); },
                  [&items](const uint8_t* data, size_t len) {
                      items.back().data.append(reinterpret_cast<const char*>(data), len);
                  });
    return items;
}

void ExpectEqual(const Item& expected, const Item& actual) {
    // 用已存在的字段编码比较每一个 EntryMeta 字段，避免漏验 SDDL/链接标志。
    cbk::VectorSink a;
    cbk::VectorSink b;
    cbk::WriteEntryMeta(expected.meta, a);
    cbk::WriteEntryMeta(actual.meta, b);
    EXPECT_EQ(a.Buffer(), b.Buffer());
    EXPECT_EQ(expected.data, actual.data);
}

// 独立手造 ustar 头；不调用生产代码的头部构造函数，防止自写自读掩盖格式错误。
void Checksum(std::vector<uint8_t>* bytes, size_t offset = 0) {
    std::fill(bytes->begin() + offset + 148, bytes->begin() + offset + 156, ' ');
    unsigned sum = 0;
    for (size_t i = offset; i < offset + 512; ++i) sum += (*bytes)[i];
    char field[8] = {};
    std::snprintf(field, sizeof(field), "%06o", sum);
    std::copy(field, field + 7, bytes->begin() + offset + 148);
}

std::vector<uint8_t> Standard(const std::string& name, const std::string& data,
                              char type = '0', const std::string& link = "") {
    std::vector<uint8_t> bytes(512, 0);
    std::copy(name.begin(), name.end(), bytes.begin());
    std::memcpy(bytes.data() + 100, "0000644", 7);
    std::memcpy(bytes.data() + 108, "0000000", 7);
    std::memcpy(bytes.data() + 116, "0000000", 7);
    char size[12] = {};
    std::snprintf(size, sizeof(size), "%011o", static_cast<unsigned>(data.size()));
    std::memcpy(bytes.data() + 124, size, 12);
    std::memcpy(bytes.data() + 136, "00000000000", 11);
    bytes[156] = static_cast<uint8_t>(type);
    std::copy(link.begin(), link.end(), bytes.begin() + 157);
    std::memcpy(bytes.data() + 257, "ustar\00000", 8);
    Checksum(&bytes);
    bytes.insert(bytes.end(), data.begin(), data.end());
    bytes.resize((bytes.size() + 511) / 512 * 512 + 1024, 0);
    return bytes;
}

size_t MemberOffset(const std::vector<uint8_t>& bytes) {
    const std::string octal(reinterpret_cast<const char*>(bytes.data() + 124), 11);
    const size_t size = static_cast<size_t>(std::stoull(octal, nullptr, 8));
    return 512 + (size + 511) / 512 * 512;
}

TEST(TarPacker, RegisteredUnderTar) {
    cbk::RegisterBuiltinPackers();
    const auto packer = cbk::PackerRegistry::Instance().Create("tar");
    ASSERT_NE(nullptr, packer);
    EXPECT_EQ("tar", packer->Name());
}

TEST(TarPacker, EmptyArchiveHasTwoZeroBlocks) {
    const auto bytes = Pack({});
    EXPECT_EQ(std::vector<uint8_t>(1024, 0), bytes);
    EXPECT_TRUE(Unpack(bytes).empty());
}

TEST(TarPacker, EveryEntryKindAndEveryMetadataFieldRoundTrips) {
    Item file = File(L"普通文件.txt", "hello, 世界!");
    file.meta.attributes = FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_READONLY;
    file.meta.creation_time = 0x01D9ABCDEF012345ull;
    file.meta.last_access_time = file.meta.creation_time + 1;
    file.meta.last_write_time = file.meta.creation_time + 2;
    file.meta.sddl = "O:BAG:SYD:P(A;;FA;;;SY)";
    file.meta.crc32 = 0x12345678;
    file.meta.data_offset = 123;
    file.meta.stored_size = 456;
    std::vector<Item> items{file};
    for (int type = 1; type <= 6; ++type) {
        Item item = File(L"条目" + std::to_wstring(type), "", type);
        item.meta.type = static_cast<cbk::FileType>(type);
        item.meta.link_target = L"..\\目标目录";
        item.meta.link_is_relative = true;
        item.meta.reparse_tag = 0xA000000C;
        items.push_back(item);
    }
    const auto actual = Unpack(Pack(items));
    ASSERT_EQ(items.size(), actual.size());
    for (size_t i = 0; i < items.size(); ++i) ExpectEqual(items[i], actual[i]);
}

TEST(TarPacker, BlockBoundariesAndBinaryContentRoundTrip) {
    for (size_t size : {0u, 1u, 511u, 512u, 513u, 65536u, 300123u}) {
        SCOPED_TRACE(size);
        std::string data(size, '\0');
        for (size_t i = 0; i < size; ++i) data[i] = static_cast<char>(i & 255);
        const std::vector<Item> items{File(L"a.bin", data), File(L"b.txt", "after", 1)};
        const auto bytes = Pack(items);
        EXPECT_EQ(0u, bytes.size() % 512);
        const auto actual = Unpack(bytes);
        ASSERT_EQ(2u, actual.size());
        ExpectEqual(items[0], actual[0]);
        ExpectEqual(items[1], actual[1]);
    }
}

TEST(TarPacker, LongUnicodePathsAndLinkTargetsUsePax) {
    Item file = File(std::wstring(180, L'中') + L"\\emoji_😀.txt", "内容");
    Item link = File(L"链接", "", 1);
    link.meta.type = cbk::FileType::kSymlinkFile;
    link.meta.link_target = L"D:\\" + std::wstring(160, L'长');
    const auto actual = Unpack(Pack({file, link}));
    ASSERT_EQ(2u, actual.size());
    ExpectEqual(file, actual[0]);
    ExpectEqual(link, actual[1]);
}

TEST(TarPacker, PaxLengthHandlesDigitsNewlinesAndEquals) {
    for (size_t length : {1u, 9u, 80u, 90u, 99u, 100u, 900u}) {
        Item item = File(std::wstring(length, L'a') + L"=\n.txt", "data");
        const auto actual = Unpack(Pack({item}));
        ASSERT_EQ(1u, actual.size());
        ExpectEqual(item, actual.front());
    }
}

TEST(TarPacker, WritesStandardHeaderAndPadding) {
    const auto bytes = Pack({File(L"hello.txt", "abc")});
    const size_t offset = MemberOffset(bytes);
    EXPECT_EQ('x', bytes[156]);
    EXPECT_EQ('0', bytes[offset + 156]);
    EXPECT_EQ(0, std::memcmp(bytes.data() + offset + 257, "ustar\00000", 8));
    EXPECT_EQ(0, std::memcmp(bytes.data() + offset + 124, "00000000003", 11));
    EXPECT_EQ(0, std::memcmp(bytes.data() + offset + 512, "abc", 3));
    EXPECT_TRUE(std::all_of(bytes.begin() + offset + 515, bytes.end(),
                            [](uint8_t b) { return b == 0; }));
    auto checked = bytes;
    Checksum(&checked, offset);
    EXPECT_EQ(bytes, checked);
}

TEST(TarPacker, ReadsIndependentUstarFixture) {
    const auto items = Unpack(Standard("external.txt", "external bytes"));
    ASSERT_EQ(1u, items.size());
    EXPECT_EQ(L"external.txt", items[0].meta.relative_path);
    EXPECT_EQ("external bytes", items[0].data);
    EXPECT_EQ(116444736000000000ull, items[0].meta.last_write_time);
}

TEST(TarPacker, ReadsUstarPrefixField) {
    auto bytes = Standard("leaf.txt", "x");
    std::memcpy(bytes.data() + 345, "parent/child", 12);
    Checksum(&bytes);
    const auto items = Unpack(bytes);
    ASSERT_EQ(1u, items.size());
    EXPECT_EQ(L"parent\\child\\leaf.txt", items[0].meta.relative_path);
}

TEST(TarPacker, ReadsStandardDirectoryAndSymlink) {
    const auto dirs = Unpack(Standard("dir/", "", '5'));
    ASSERT_EQ(1u, dirs.size());
    EXPECT_EQ(cbk::FileType::kDirectory, dirs[0].meta.type);
    const auto links = Unpack(Standard("link", "", '2', "../target"));
    ASSERT_EQ(1u, links.size());
    EXPECT_EQ(cbk::FileType::kSymlinkFile, links[0].meta.type);
    EXPECT_EQ(L"../target", links[0].meta.link_target);
    EXPECT_TRUE(links[0].meta.link_is_relative);
}

TEST(TarPacker, PaxLargeSizeDoesNotAllocateFileContent) {
    cbk::TarPacker packer;
    cbk::VectorSink sink;
    Item file = File(L"huge.bin", "");
    file.meta.original_size = 1ull << 34;
    packer.BeginEntry(file.meta, sink);
    EXPECT_LT(sink.Buffer().size(), 4096u);
    const std::string bytes(sink.Buffer().begin(), sink.Buffer().end());
    EXPECT_NE(std::string::npos, bytes.find("size=17179869184\n"));
    EXPECT_THROW(packer.EndEntry(sink), std::runtime_error);
}

TEST(TarPacker, ContentIsForwardedBeforeEndEntry) {
    cbk::TarPacker packer;
    cbk::VectorSink sink;
    packer.BeginEntry(File(L"x", "abc").meta, sink);
    const size_t before = sink.Buffer().size();
    packer.WriteData(reinterpret_cast<const uint8_t*>("abc"), 3, sink);
    EXPECT_EQ(before + 3, sink.Buffer().size());
    packer.EndEntry(sink);
    packer.Finish(sink);
}

TEST(TarPacker, RejectsGrowingAndShrinkingFiles) {
    cbk::TarPacker packer;
    cbk::VectorSink sink;
    packer.BeginEntry(File(L"x", "abc").meta, sink);
    EXPECT_THROW(packer.WriteData(reinterpret_cast<const uint8_t*>("abcd"), 4, sink),
                 std::runtime_error);
    packer.WriteData(reinterpret_cast<const uint8_t*>("ab"), 2, sink);
    EXPECT_THROW(packer.EndEntry(sink), std::runtime_error);
}

TEST(TarPacker, EnforcesCallOrderAndIdempotentFinish) {
    cbk::TarPacker packer;
    cbk::VectorSink sink;
    EXPECT_THROW(packer.EndEntry(sink), std::runtime_error);
    EXPECT_THROW(packer.WriteData(nullptr, 0, sink), std::runtime_error);
    packer.BeginEntry(File(L"x", "").meta, sink);
    EXPECT_THROW(packer.BeginEntry(File(L"y", "").meta, sink), std::runtime_error);
    EXPECT_THROW(packer.Finish(sink), std::runtime_error);
    packer.EndEntry(sink);
    packer.Finish(sink);
    const size_t size = sink.Buffer().size();
    packer.Finish(sink);
    EXPECT_EQ(size, sink.Buffer().size());
    EXPECT_THROW(packer.BeginEntry(File(L"z", "").meta, sink), std::runtime_error);
}

TEST(TarPacker, RejectsHardlinkWithoutEarlierTarget) {
    Item item = File(L"link", "", 1);
    item.meta.type = cbk::FileType::kHardlinkRef;
    EXPECT_THROW(Pack({item}), std::runtime_error);
    EXPECT_THROW(Unpack(Standard("link", "", '1', "missing")), std::runtime_error);
}

TEST(TarPacker, RejectsUnsafePaths) {
    for (const auto& name : {L"", L"../outside", L"C:/outside", L"/outside", L"a/../../b"}) {
        EXPECT_THROW(Pack({File(name, "")}), std::runtime_error);
        EXPECT_THROW(Unpack(Standard(cbk::ToUtf8(name), "")), std::runtime_error);
    }
}

TEST(TarPacker, RejectsBadChecksumAndOctalFields) {
    auto bytes = Standard("a", "abc");
    bytes[0] ^= 1;
    EXPECT_THROW(Unpack(bytes), std::runtime_error);
    bytes = Standard("a", "abc");
    bytes[124] = '9';
    Checksum(&bytes);
    EXPECT_THROW(Unpack(bytes), std::runtime_error);
}

TEST(TarPacker, RejectsTruncationAtEveryRegion) {
    const auto original = Pack({File(L"a", "abc")});
    const size_t member = MemberOffset(original);
    for (size_t count : {size_t{0}, size_t{511}, size_t{520}, member - 1, member + 511,
                         member + 514, member + 515, original.size() - 512}) {
        SCOPED_TRACE(count);
        const std::vector<uint8_t> truncated(original.begin(), original.begin() + count);
        EXPECT_THROW(Unpack(truncated), std::runtime_error);
    }
}

TEST(TarPacker, RejectsInvalidPaxLengthAndOversizedPax) {
    auto bytes = Pack({File(L"a", "abc")});
    bytes[512] = '0';
    EXPECT_THROW(Unpack(bytes), std::runtime_error);
    bytes = Pack({File(L"a", "abc")});
    std::memcpy(bytes.data() + 124, "00040000000", 11);  // 8 MiB，不分配便拒绝。
    Checksum(&bytes);
    EXPECT_THROW(Unpack(bytes), std::runtime_error);
}

TEST(TarPacker, RejectsCorruptedCbkMetadata) {
    auto bytes = Pack({File(L"a", "abc")});
    const std::string text(bytes.begin(), bytes.end());
    const size_t pos = text.find("CBK.meta=");
    ASSERT_NE(std::string::npos, pos);
    bytes[pos + 9] = 'z';
    EXPECT_THROW(Unpack(bytes), std::runtime_error);
}

TEST(TarPacker, RejectsMismatchBetweenPaxMetadataAndHeader) {
    auto bytes = Pack({File(L"a", "abc")});
    const size_t member = MemberOffset(bytes);
    bytes[member + 156] = '5';
    Checksum(&bytes, member);
    EXPECT_THROW(Unpack(bytes), std::runtime_error);
}

TEST(TarPacker, RejectsUnsupportedTarVariants) {
    for (char type : {'g', 'L', 'K', 'S', '3', '4', '6'}) {
        EXPECT_THROW(Unpack(Standard("a", "", type)), std::runtime_error);
    }
}

TEST(TarPacker, RejectsOrphanPaxAndSparseExtensions) {
    EXPECT_THROW(Unpack(Standard("PaxHeaders/x", "9 path=a\n", 'x')), std::runtime_error);
    auto bytes = Standard("PaxHeaders/x", "22 GNU.sparse.major=1\n", 'x');
    EXPECT_THROW(Unpack(bytes), std::runtime_error);
}

TEST(TarPacker, EnginePreservesHardlinkIdentityWithTar) {
    cbk::RegisterBuiltinPackers();
    cbk_test::TempDir temp;
    temp.MakeDir(L"src");
    const std::wstring first = temp.MakeFile(L"src\\a.txt", "shared content");
    ASSERT_NE(0, CreateHardLinkW(temp.At(L"src\\b.txt").c_str(), first.c_str(), nullptr));
    cbk::BackupOptions backup;
    backup.source_root = temp.At(L"src");
    backup.dest_archive = temp.At(L"links.cbk");
    backup.packer = "tar";
    ASSERT_EQ(cbk::Status::kOk, cbk::RunBackup(backup, nullptr).status);
    cbk::RestoreOptions restore;
    restore.archive = backup.dest_archive;
    restore.dest_root = temp.At(L"back");
    ASSERT_EQ(cbk::Status::kOk, cbk::RunRestore(restore, nullptr).status);
    auto a = cbk::platform::OpenForRead(temp.At(L"back\\a.txt"), true);
    auto b = cbk::platform::OpenForRead(temp.At(L"back\\b.txt"), true);
    BY_HANDLE_FILE_INFORMATION ia = {};
    BY_HANDLE_FILE_INFORMATION ib = {};
    ASSERT_NE(0, GetFileInformationByHandle(a.Get(), &ia));
    ASSERT_NE(0, GetFileInformationByHandle(b.Get(), &ib));
    EXPECT_EQ(ia.nFileIndexHigh, ib.nFileIndexHigh);
    EXPECT_EQ(ia.nFileIndexLow, ib.nFileIndexLow);
    EXPECT_EQ(2u, ia.nNumberOfLinks);
    EXPECT_EQ("shared content", temp.ReadFile(L"back\\b.txt"));
}

TEST(TarPacker, AcceptsTrailingZeroPaddingButRejectsTrailingData) {
    auto bytes = Pack({});
    bytes.resize(10240, 0);
    EXPECT_TRUE(Unpack(bytes).empty());
    bytes.back() = 1;
    EXPECT_THROW(Unpack(bytes), std::runtime_error);
}

TEST(TarPacker, EngineBacksUpListsVerifiesAndRestoresUsingTar) {
    cbk::RegisterBuiltinPackers();
    cbk_test::TempDir temp;
    temp.MakeDir(L"src");
    temp.MakeDir(L"src\\子目录");
    const std::string payload(131075, 'a');
    temp.MakeFile(L"src\\子目录\\内容.txt", payload);
    temp.MakeFile(L"src\\empty", "");
    cbk::BackupOptions backup;
    backup.source_root = temp.At(L"src");
    backup.dest_archive = temp.At(L"archive.cbk");
    backup.packer = "tar";
    ASSERT_EQ(cbk::Status::kOk, cbk::RunBackup(backup, nullptr).status);
    std::wstring error;
    ASSERT_EQ(cbk::Status::kOk, cbk::VerifyArchive(backup.dest_archive, &error));
    cbk::ArchiveInfo info;
    std::vector<cbk::EntryMeta> entries;
    ASSERT_EQ(cbk::Status::kOk,
              cbk::ReadArchiveListing(backup.dest_archive, "", &info, &entries, &error));
    EXPECT_EQ("tar", info.packer);
    EXPECT_EQ(3u, entries.size());
    cbk::RestoreOptions restore;
    restore.archive = backup.dest_archive;
    restore.dest_root = temp.At(L"back");
    ASSERT_EQ(cbk::Status::kOk, cbk::RunRestore(restore, nullptr).status);
    EXPECT_EQ(payload, temp.ReadFile(L"back\\子目录\\内容.txt"));
    EXPECT_EQ("", temp.ReadFile(L"back\\empty"));
}

}  // namespace
