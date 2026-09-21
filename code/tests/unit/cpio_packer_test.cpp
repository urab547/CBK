// Copyright 2026 CBK Project. newc 格式与引擎集成测试。
#include <gtest/gtest.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#include "cbk/engine.h"
#include "cbk/packer.h"
#include "src/byte_io.h"
#include "src/entry_codec.h"
#include "src/packers/cpio_packer.h"
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
    cbk::CpioPacker packer;
    cbk::VectorSink sink;
    for (const auto& item : items) {
        packer.BeginEntry(item.meta, sink);
        for (size_t i = 0; i < item.data.size(); i += 7) {
            packer.WriteData(reinterpret_cast<const uint8_t*>(item.data.data() + i),
                              std::min<size_t>(7, item.data.size() - i), sink);
        }
        packer.EndEntry(sink);
    }
    packer.Finish(sink);
    return sink.Buffer();
}

class ShortSource : public cbk::ISource {
public:
    explicit ShortSource(const std::vector<uint8_t>& bytes) : source_(bytes) {}
    size_t Read(uint8_t* data, size_t len) override {
        return source_.Read(data, std::min<size_t>(3, len));
    }

private:
    cbk::MemorySource source_;
};

std::vector<Item> Unpack(const std::vector<uint8_t>& bytes) {
    ShortSource source(bytes);
    cbk::CpioPacker packer;
    std::vector<Item> items;
    packer.Unpack(source, [&items](const cbk::EntryMeta& meta) { items.push_back({meta, ""}); },
                  [&items](const uint8_t* data, size_t len) {
                      items.back().data.append(reinterpret_cast<const char*>(data), len);
                  });
    return items;
}

void ExpectEqual(const Item& expected, const Item& actual) {
    cbk::VectorSink a;
    cbk::VectorSink b;
    cbk::WriteEntryMeta(expected.meta, a);
    cbk::WriteEntryMeta(actual.meta, b);
    EXPECT_EQ(a.Buffer(), b.Buffer());
    EXPECT_EQ(expected.data, actual.data);
}

// 独立构造 newc 样本，验证标准字段布局，不复用被测打包器。
std::vector<uint8_t> Record(const std::string& name, const std::string& data,
                            uint32_t mode = 0100644, uint32_t inode = 1, uint32_t links = 1) {
    const std::array<uint32_t, 13> fields{inode, mode, 0, 0, links, 0,
                                         static_cast<uint32_t>(data.size()), 0, 0, 0, 0,
                                         static_cast<uint32_t>(name.size() + 1), 0};
    std::string text = "070701";
    for (uint32_t field : fields) {
        char hex[9] = {};
        std::snprintf(hex, sizeof(hex), "%08x", field);
        text += hex;
    }
    text += name;
    text.push_back('\0');
    while (text.size() % 4 != 0) text.push_back('\0');
    text += data;
    while (text.size() % 4 != 0) text.push_back('\0');
    return std::vector<uint8_t>(text.begin(), text.end());
}

std::vector<uint8_t> Finish(std::vector<uint8_t> bytes) {
    const auto trailer = Record("TRAILER!!!", "", 0);
    bytes.insert(bytes.end(), trailer.begin(), trailer.end());
    return bytes;
}

size_t Field(const std::vector<uint8_t>& bytes, size_t offset, size_t index) {
    return static_cast<size_t>(std::stoull(
        std::string(bytes.begin() + offset + 6 + index * 8,
                    bytes.begin() + offset + 14 + index * 8), nullptr, 16));
}

size_t Next(const std::vector<uint8_t>& bytes, size_t offset) {
    const size_t start = (offset + 110 + Field(bytes, offset, 11) + 3) / 4 * 4;
    return (start + Field(bytes, offset, 6) + 3) / 4 * 4;
}

TEST(CpioPacker, RegisteredUnderCpio) {
    cbk::RegisterBuiltinPackers();
    auto packer = cbk::PackerRegistry::Instance().Create("cpio");
    ASSERT_NE(nullptr, packer);
    EXPECT_EQ("cpio", packer->Name());
}

TEST(CpioPacker, EmptyArchiveHasStandardTrailer) {
    EXPECT_EQ(Record("TRAILER!!!", "", 0, 0), Pack({}));
    EXPECT_TRUE(Unpack(Pack({})).empty());
}

TEST(CpioPacker, AllTypesAndMetadataRoundTrip) {
    Item file = File(L"中文.txt", "content", 123456789012345ull);
    file.meta.attributes = FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_READONLY;
    file.meta.creation_time = 0x01D9ABCDEF012345ull;
    file.meta.last_access_time = file.meta.creation_time + 1;
    file.meta.last_write_time = file.meta.creation_time + 2;
    file.meta.sddl = "O:BAG:SYD:P(A;;FA;;;SY)";
    file.meta.stored_size = 987;
    file.meta.data_offset = 123;
    file.meta.crc32 = 0x12345678;
    std::vector<Item> items{file};
    for (int type = 1; type <= 6; ++type) {
        Item item = File(L"条目" + std::to_wstring(type), "", type);
        item.meta.type = static_cast<cbk::FileType>(type);
        item.meta.link_target = L"..\\中文.txt";
        item.meta.link_is_relative = true;
        item.meta.hardlink_ref_id = file.meta.id;
        item.meta.reparse_tag = 0xA000000C;
        items.push_back(item);
    }
    const auto restored = Unpack(Pack(items));
    ASSERT_EQ(items.size(), restored.size());
    for (size_t i = 0; i < items.size(); ++i) ExpectEqual(items[i], restored[i]);
}

TEST(CpioPacker, BinaryContentAndFourByteBoundariesRoundTrip) {
    for (size_t size : {0u, 1u, 2u, 3u, 4u, 5u, 65536u, 131079u}) {
        SCOPED_TRACE(size);
        std::string data(size, '\0');
        for (size_t i = 0; i < size; ++i) data[i] = static_cast<char>(i & 255);
        const Item file = File(L"payload", data);
        const auto bytes = Pack({file, File(L"next", "ok", 1)});
        EXPECT_EQ(0u, bytes.size() % 4);
        const auto restored = Unpack(bytes);
        ASSERT_EQ(2u, restored.size());
        ExpectEqual(file, restored[0]);
        EXPECT_EQ("ok", restored[1].data);
    }
}

TEST(CpioPacker, LongUnicodeNamesAndLinkTargetsRoundTrip) {
    Item link = File(std::wstring(180, L'长') + L"\\😀.link", "");
    link.meta.type = cbk::FileType::kSymlinkDir;
    link.meta.link_target = L"D:\\" + std::wstring(200, L'中');
    const auto restored = Unpack(Pack({link}));
    ASSERT_EQ(1u, restored.size());
    ExpectEqual(link, restored[0]);
}

TEST(CpioPacker, AFileNamedTrailerDoesNotEndTheArchive) {
    const auto restored = Unpack(Pack({File(L"TRAILER!!!", "not a trailer")}));
    ASSERT_EQ(1u, restored.size());
    EXPECT_EQ(L"TRAILER!!!", restored[0].meta.relative_path);
}

TEST(CpioPacker, ReadsIndependentNewcFixture) {
    auto bytes = Record("./dir/file", "external");
    for (size_t i = 6; i < 110; ++i) {
        if (bytes[i] >= 'a' && bytes[i] <= 'f') bytes[i] -= 'a' - 'A';
    }
    const auto restored = Unpack(Finish(bytes));
    ASSERT_EQ(1u, restored.size());
    EXPECT_EQ(L"dir\\file", restored[0].meta.relative_path);
    EXPECT_EQ("external", restored[0].data);
    EXPECT_EQ(116444736000000000ull, restored[0].meta.last_write_time);
}

TEST(CpioPacker, ReadsStandardDirectorySymlinkAndThreeHardlinks) {
    auto bytes = Record("directory", "", 0040755, 1);
    for (const auto& record : {Record("link", "../target", 0120777, 2),
                               Record("first", "shared", 0100644, 3, 3),
                               Record("second", "", 0100644, 3, 3),
                               Record("third", "", 0100644, 3, 3)}) {
        bytes.insert(bytes.end(), record.begin(), record.end());
    }
    const auto restored = Unpack(Finish(bytes));
    ASSERT_EQ(5u, restored.size());
    EXPECT_EQ(cbk::FileType::kDirectory, restored[0].meta.type);
    EXPECT_EQ(cbk::FileType::kSymlinkFile, restored[1].meta.type);
    EXPECT_EQ(L"../target", restored[1].meta.link_target);
    for (size_t i : {3u, 4u}) {
        EXPECT_EQ(cbk::FileType::kHardlinkRef, restored[i].meta.type);
        EXPECT_EQ(restored[2].meta.id, restored[i].meta.hardlink_ref_id);
    }
}

TEST(CpioPacker, RejectsDeferredHardlinkDataInsteadOfLosingIt) {
    auto bytes = Record("first", "", 0100644, 1, 2);
    const auto second = Record("second", "data", 0100644, 1, 2);
    bytes.insert(bytes.end(), second.begin(), second.end());
    EXPECT_THROW(Unpack(Finish(bytes)), std::runtime_error);
}

TEST(CpioPacker, StreamsWithoutBufferingFileContent) {
    cbk::CpioPacker packer;
    cbk::VectorSink sink;
    auto item = File(L"large", "");
    item.meta.original_size = 0xFFFFFFFFull;
    packer.BeginEntry(item.meta, sink);
    EXPECT_LT(sink.Buffer().size(), 2048u);
    const size_t before = sink.Buffer().size();
    packer.WriteData(reinterpret_cast<const uint8_t*>("abc"), 3, sink);
    EXPECT_EQ(before + 3, sink.Buffer().size());
    EXPECT_THROW(packer.EndEntry(sink), std::runtime_error);
}

TEST(CpioPacker, RejectsFileAboveNewcLimitBeforeWriting) {
    cbk::CpioPacker packer;
    cbk::VectorSink sink;
    auto item = File(L"too-large", "");
    item.meta.original_size = 0x100000000ull;
    EXPECT_THROW(packer.BeginEntry(item.meta, sink), std::runtime_error);
    EXPECT_TRUE(sink.Buffer().empty());
}

TEST(CpioPacker, EnforcesLengthAndCallOrder) {
    cbk::CpioPacker packer;
    cbk::VectorSink sink;
    EXPECT_THROW(packer.EndEntry(sink), std::runtime_error);
    EXPECT_THROW(packer.WriteData(nullptr, 0, sink), std::runtime_error);
    packer.BeginEntry(File(L"x", "abc").meta, sink);
    EXPECT_THROW(packer.Finish(sink), std::runtime_error);
    EXPECT_THROW(packer.BeginEntry(File(L"y", "").meta, sink), std::runtime_error);
    EXPECT_THROW(packer.WriteData(nullptr, 1, sink), std::runtime_error);
    EXPECT_THROW(packer.WriteData(reinterpret_cast<const uint8_t*>("abcd"), 4, sink),
                 std::runtime_error);
    EXPECT_THROW(packer.EndEntry(sink), std::runtime_error);
    packer.WriteData(reinterpret_cast<const uint8_t*>("abc"), 3, sink);
    packer.EndEntry(sink);
    packer.Finish(sink);
    const size_t size = sink.Buffer().size();
    packer.Finish(sink);
    EXPECT_EQ(size, sink.Buffer().size());
    EXPECT_THROW(packer.BeginEntry(File(L"z", "").meta, sink), std::runtime_error);
}

TEST(CpioPacker, RejectsUnsafeAndReservedPaths) {
    for (const auto* name : {L"", L"../escape", L"/absolute", L"C:/drive", L"a//b",
                             L".__cbk_metadata__/x", L".__CBK_METADATA__"}) {
        EXPECT_THROW(Pack({File(name, "")}), std::runtime_error);
    }
    EXPECT_THROW(Unpack(Finish(Record("../escape", ""))), std::runtime_error);
    EXPECT_THROW(Unpack(Finish(Record(std::string("bad\0name", 8), ""))), std::runtime_error);
    EXPECT_THROW(Unpack(Finish(Record(std::string("\xff", 1), ""))), std::runtime_error);
}

TEST(CpioPacker, RejectsDuplicateIdsAndMissingHardlinkTarget) {
    EXPECT_THROW(Pack({File(L"a", ""), File(L"b", "")}), std::runtime_error);
    auto link = File(L"link", "");
    link.meta.type = cbk::FileType::kHardlinkRef;
    link.meta.hardlink_ref_id = 99;
    EXPECT_THROW(Pack({link}), std::runtime_error);
}

TEST(CpioPacker, RejectsMalformedHeadersAndOversizedNames) {
    for (size_t field_offset : {0u, 6u, 14u, 54u}) {
        auto bytes = Finish(Record("x", ""));
        bytes[field_offset] = 'z';
        EXPECT_THROW(Unpack(bytes), std::runtime_error);
    }
    auto bytes = Finish(Record("x", ""));
    std::fill(bytes.begin() + 94, bytes.begin() + 102, 'f');
    EXPECT_THROW(Unpack(bytes), std::runtime_error);
    bytes = Finish(Record("x", ""));
    bytes[109] = '1';
    EXPECT_THROW(Unpack(bytes), std::runtime_error);
    bytes = Finish(Record("x", ""));
    bytes[111] = 'x';  // 名字缺少 NUL。
    EXPECT_THROW(Unpack(bytes), std::runtime_error);
}

TEST(CpioPacker, RejectsTruncatedRegionsAndOrphanMetadata) {
    const auto bytes = Pack({File(L"xx", "content")});
    const size_t member = Next(bytes, 0);
    const size_t data_start = (member + 110 + Field(bytes, member, 11) + 3) / 4 * 4;
    for (size_t length : {size_t{0}, size_t{109}, size_t{130}, member - 1, member + 109,
                          data_start + 2, data_start + 7, bytes.size() - 1}) {
        SCOPED_TRACE(length);
        EXPECT_THROW(Unpack(std::vector<uint8_t>(bytes.begin(), bytes.begin() + length)),
                     std::runtime_error);
    }
    EXPECT_THROW(Unpack(Finish(std::vector<uint8_t>(bytes.begin(), bytes.begin() + member))),
                 std::runtime_error);
}

TEST(CpioPacker, RejectsMetadataMismatchAndOversizedRecord) {
    auto bytes = Pack({File(L"x", "abc")});
    const size_t member = Next(bytes, 0);
    bytes[member + 61] = '4';  // filesize 从 3 改成 4。
    EXPECT_THROW(Unpack(bytes), std::runtime_error);
    bytes = Pack({File(L"x", "abc")});
    std::fill(bytes.begin() + 54, bytes.begin() + 62, 'f');
    EXPECT_THROW(Unpack(bytes), std::runtime_error);
}

TEST(CpioPacker, AcceptsZeroTailButRejectsDataAfterTrailer) {
    auto bytes = Pack({});
    bytes.resize(512, 0);
    EXPECT_TRUE(Unpack(bytes).empty());
    bytes.back() = 1;
    EXPECT_THROW(Unpack(bytes), std::runtime_error);
    EXPECT_THROW(Unpack(Finish(Record("device", "", 0020644))), std::runtime_error);
    EXPECT_THROW(Unpack(Finish(Record("directory", "bad", 0040755))), std::runtime_error);
}

TEST(CpioPacker, EngineRoundTripsContentsMetadataAndThreeHardlinks) {
    cbk::RegisterBuiltinPackers();
    cbk_test::TempDir temp;
    temp.MakeDir(L"src");
    temp.MakeDir(L"src\\中文目录");
    const auto first = temp.MakeFile(L"src\\a", "shared");
    ASSERT_NE(0, CreateHardLinkW(temp.At(L"src\\b").c_str(), first.c_str(), nullptr));
    ASSERT_NE(0, CreateHardLinkW(temp.At(L"src\\c").c_str(), first.c_str(), nullptr));
    temp.MakeFile(L"src\\中文目录\\内容", std::string(131077, 'x'));
    temp.MakeFile(L"src\\empty", "");
    cbk::BackupOptions backup;
    backup.source_root = temp.At(L"src");
    backup.dest_archive = temp.At(L"backup.cbk");
    backup.packer = "cpio";
    ASSERT_EQ(cbk::Status::kOk, cbk::RunBackup(backup, nullptr).status);
    std::wstring error;
    EXPECT_EQ(cbk::Status::kOk, cbk::VerifyArchive(backup.dest_archive, &error));
    cbk::ArchiveInfo info;
    std::vector<cbk::EntryMeta> entries;
    ASSERT_EQ(cbk::Status::kOk,
              cbk::ReadArchiveListing(backup.dest_archive, "", &info, &entries, &error));
    EXPECT_EQ("cpio", info.packer);
    EXPECT_EQ(6u, entries.size());
    cbk::RestoreOptions restore;
    restore.archive = backup.dest_archive;
    restore.dest_root = temp.At(L"back");
    ASSERT_EQ(cbk::Status::kOk, cbk::RunRestore(restore, nullptr).status);
    EXPECT_EQ(std::string(131077, 'x'), temp.ReadFile(L"back\\中文目录\\内容"));
    for (const auto* name : {L"a", L"b", L"c"}) {
        EXPECT_EQ("shared", temp.ReadFile(std::wstring(L"back\\") + name));
        auto handle = cbk::platform::OpenForRead(temp.At(std::wstring(L"back\\") + name), true);
        BY_HANDLE_FILE_INFORMATION attributes = {};
        ASSERT_NE(0, GetFileInformationByHandle(handle.Get(), &attributes));
        EXPECT_EQ(3u, attributes.nNumberOfLinks);
    }
    EXPECT_EQ(INVALID_FILE_ATTRIBUTES,
              GetFileAttributesW(temp.At(L"back\\.__cbk_metadata__").c_str()));
}

}  // namespace
