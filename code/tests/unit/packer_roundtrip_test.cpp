// Copyright 2026 CBK Project. cbk-native 打包器的往返测试。
//
// 这套用例**不依赖 Scanner，也不碰文件系统**——EntryMeta 全是手捏的。
// 队友 A 写 tar / cpio 打包器时可以照着改个类名直接复用，
// 这也是当初把 IPacker 的契约定成现在这样的目的。

#include <windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "cbk/packer.h"
#include "cbk/types.h"
#include "src/byte_io.h"
#include "src/packers/native_packer.h"

namespace {

/// 一条条目：元数据 + 内容。
struct Item {
    cbk::EntryMeta meta;
    std::string content;
};

/// 把若干条目打包成一段字节。
std::vector<uint8_t> Pack(cbk::IPacker* packer, const std::vector<Item>& items) {
    cbk::VectorSink sink;
    for (const Item& item : items) {
        packer->BeginEntry(item.meta, sink);
        // 故意分成好几块喂进去：WriteData 的契约就是"会被调很多次，
        // 每次长度不保证对齐"，一次性喂完的测试测不出这一点。
        const size_t chunk = 7;
        for (size_t offset = 0; offset < item.content.size(); offset += chunk) {
            const size_t len =
                (offset + chunk < item.content.size()) ? chunk : item.content.size() - offset;
            packer->WriteData(reinterpret_cast<const uint8_t*>(item.content.data()) + offset, len,
                              sink);
        }
        packer->EndEntry(sink);
    }
    packer->Finish(sink);
    return sink.Buffer();
}

/// 解包回来。
std::vector<Item> Unpack(cbk::IPacker* packer, const std::vector<uint8_t>& bytes) {
    cbk::MemorySource source(bytes);
    std::vector<Item> out;
    packer->Unpack(
        source, [&out](const cbk::EntryMeta& meta) { out.push_back(Item{meta, std::string()}); },
        [&out](const uint8_t* data, size_t len) {
            out.back().content.append(reinterpret_cast<const char*>(data), len);
        });
    return out;
}

void ExpectMetaEqual(const cbk::EntryMeta& expected, const cbk::EntryMeta& actual) {
    EXPECT_EQ(expected.id, actual.id);
    EXPECT_EQ(expected.relative_path, actual.relative_path);
    EXPECT_EQ(expected.type, actual.type);
    EXPECT_EQ(expected.attributes, actual.attributes);
    EXPECT_EQ(expected.creation_time, actual.creation_time);
    EXPECT_EQ(expected.last_access_time, actual.last_access_time);
    EXPECT_EQ(expected.last_write_time, actual.last_write_time);
    EXPECT_EQ(expected.sddl, actual.sddl);
    EXPECT_EQ(expected.original_size, actual.original_size);
    EXPECT_EQ(expected.stored_size, actual.stored_size);
    EXPECT_EQ(expected.data_offset, actual.data_offset);
    EXPECT_EQ(expected.crc32, actual.crc32);
    EXPECT_EQ(expected.link_target, actual.link_target);
    EXPECT_EQ(expected.link_is_relative, actual.link_is_relative);
    EXPECT_EQ(expected.reparse_tag, actual.reparse_tag);
    EXPECT_EQ(expected.hardlink_ref_id, actual.hardlink_ref_id);
}

/// 造一组覆盖各种类型的条目。
std::vector<Item> MakeSampleItems() {
    std::vector<Item> items;

    cbk::EntryMeta file;
    file.id = 0;
    file.relative_path = L"普通文件.txt";
    file.type = cbk::FileType::kRegular;
    file.attributes = FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_READONLY;
    file.creation_time = 0x01D9ABCDEF012345ull;
    file.last_access_time = 0x01D9ABCDEF012346ull;
    file.last_write_time = 0x01D9ABCDEF012347ull;
    file.original_size = 13;
    file.crc32 = 0xCBF43926u;
    file.sddl = "O:BAG:SYD:(A;;FA;;;SY)(A;;FA;;;BA)";
    items.push_back(Item{file, "hello, 世界!"});

    cbk::EntryMeta directory;
    directory.id = 1;
    directory.relative_path = L"子目录";
    directory.type = cbk::FileType::kDirectory;
    directory.attributes = FILE_ATTRIBUTE_DIRECTORY;
    items.push_back(Item{directory, std::string()});

    cbk::EntryMeta symlink;
    symlink.id = 2;
    symlink.relative_path = L"子目录\\指向别处的链接";
    symlink.type = cbk::FileType::kSymlinkDir;
    symlink.attributes = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT;
    symlink.link_target = L"..\\目标目录";
    symlink.link_is_relative = true;
    symlink.reparse_tag = 0xA000000Cu;
    items.push_back(Item{symlink, std::string()});

    cbk::EntryMeta hardlink;
    hardlink.id = 3;
    hardlink.relative_path = L"硬链接.txt";
    hardlink.type = cbk::FileType::kHardlinkRef;
    hardlink.hardlink_ref_id = 0;
    items.push_back(Item{hardlink, std::string()});

    cbk::EntryMeta empty;
    empty.id = 4;
    empty.relative_path = L"零字节.bin";
    empty.type = cbk::FileType::kRegular;
    empty.original_size = 0;
    items.push_back(Item{empty, std::string()});

    return items;
}

TEST(NativePacker, RegisteredUnderItsName) {
    cbk::RegisterBuiltinPackers();
    EXPECT_TRUE(cbk::PackerRegistry::Instance().Has(cbk::kNativePackerName));
    EXPECT_NE(nullptr, cbk::PackerRegistry::Instance().Create(cbk::kNativePackerName));
}

TEST(NativePacker, RoundTripsEveryEntryKind) {
    cbk::NativePacker packer;
    const std::vector<Item> original = MakeSampleItems();

    const std::vector<uint8_t> packed = Pack(&packer, original);
    const std::vector<Item> restored = Unpack(&packer, packed);

    ASSERT_EQ(original.size(), restored.size());
    for (size_t i = 0; i < original.size(); ++i) {
        SCOPED_TRACE("条目 " + std::to_string(i));
        ExpectMetaEqual(original[i].meta, restored[i].meta);
        EXPECT_EQ(original[i].content, restored[i].content);
    }
}

TEST(NativePacker, HandlesEmptyArchive) {
    cbk::NativePacker packer;
    const std::vector<uint8_t> packed = Pack(&packer, {});
    EXPECT_TRUE(packed.empty());
    EXPECT_TRUE(Unpack(&packer, packed).empty());
}

TEST(NativePacker, ContentSurvivesArbitraryBytes) {
    // 内容里出现 0x00、0xFF、以及"看起来像长度前缀"的字节序列，
    // 都不能让解析器串位。
    std::string payload;
    for (int i = 0; i < 256; ++i) payload.push_back(static_cast<char>(i));
    payload += std::string("\0\0\0\0", 4);
    payload += std::string("\xFF\xFF\xFF\xFF", 4);

    cbk::EntryMeta meta;
    meta.relative_path = L"binary.bin";
    meta.type = cbk::FileType::kRegular;
    meta.original_size = payload.size();

    cbk::NativePacker packer;
    const std::vector<Item> restored = Unpack(&packer, Pack(&packer, {Item{meta, payload}}));
    ASSERT_EQ(1u, restored.size());
    EXPECT_EQ(payload, restored[0].content);
}

TEST(NativePacker, LargeContentSpansManyChunks) {
    std::string payload(300 * 1024, '\0');
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<char>((i * 7 + 3) & 0xFF);
    }

    cbk::EntryMeta meta;
    meta.relative_path = L"big.bin";
    meta.type = cbk::FileType::kRegular;
    meta.original_size = payload.size();

    cbk::NativePacker packer;
    const std::vector<Item> restored = Unpack(&packer, Pack(&packer, {Item{meta, payload}}));
    ASSERT_EQ(1u, restored.size());
    EXPECT_EQ(payload.size(), restored[0].content.size());
    EXPECT_EQ(payload, restored[0].content);
}

TEST(NativePacker, DoesNotTrustOriginalSizeWhenReadingContent) {
    // 备份期间文件被别人改大改小是常事。内容分块自带长度，所以哪怕
    // original_size 跟实际写入量对不上，也只是这一条的大小字段不准，
    // 不会让后面所有条目错位。
    cbk::EntryMeta lying;
    lying.id = 0;
    lying.relative_path = L"size-lies.txt";
    lying.type = cbk::FileType::kRegular;
    lying.original_size = 999999;  // 谎报

    cbk::EntryMeta next;
    next.id = 1;
    next.relative_path = L"after.txt";
    next.type = cbk::FileType::kRegular;
    next.original_size = 2;

    cbk::NativePacker packer;
    const std::vector<Item> items = {Item{lying, "short"}, Item{next, "ok"}};
    const std::vector<Item> restored = Unpack(&packer, Pack(&packer, items));

    ASSERT_EQ(2u, restored.size());
    EXPECT_EQ("short", restored[0].content);
    EXPECT_EQ(L"after.txt", restored[1].meta.relative_path);
    EXPECT_EQ("ok", restored[1].content);
}

TEST(NativePacker, RejectsTruncatedStream) {
    cbk::EntryMeta meta;
    meta.relative_path = L"a.txt";
    meta.type = cbk::FileType::kRegular;
    meta.original_size = 5;

    cbk::NativePacker packer;
    std::vector<uint8_t> packed = Pack(&packer, {Item{meta, "hello"}});
    ASSERT_GT(packed.size(), 8u);
    packed.resize(packed.size() - 3);  // 砍掉尾巴

    cbk::MemorySource source(packed);
    EXPECT_THROW(packer.Unpack(
                     source, [](const cbk::EntryMeta&) {}, [](const uint8_t*, size_t) {}),
                 std::runtime_error);
}

TEST(NativePacker, RejectsGarbage) {
    const std::vector<uint8_t> garbage = {0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x02};
    cbk::MemorySource source(garbage);
    cbk::NativePacker packer;
    EXPECT_THROW(packer.Unpack(
                     source, [](const cbk::EntryMeta&) {}, [](const uint8_t*, size_t) {}),
                 std::runtime_error);
}

TEST(NativePacker, RejectsOutOfRangeFileType) {
    // 手工造一条 type 字段越界的记录，确认解析器挡得住。
    std::vector<uint8_t> record;
    cbk::ByteWriter body(&record);
    body.WriteU64(0);   // id
    body.WriteU8(200);  // type —— 合法范围是 0..6
    body.WriteU32(0);   // attributes
    body.WriteU64(0);   // creation
    body.WriteU64(0);   // access
    body.WriteU64(0);   // write
    body.WriteU64(0);   // original_size
    body.WriteU64(0);   // stored_size
    body.WriteU64(0);   // data_offset
    body.WriteU32(0);   // crc32
    body.WriteU8(0);    // link_is_relative
    body.WriteU32(0);   // reparse_tag
    body.WriteU64(0);   // hardlink_ref_id
    body.WriteLengthPrefixed("x");
    body.WriteLengthPrefixed("");
    body.WriteLengthPrefixed("");

    std::vector<uint8_t> framed;
    cbk::ByteWriter frame(&framed);
    frame.WriteU32(static_cast<uint32_t>(record.size()));
    frame.WriteBytes(record.data(), record.size());

    cbk::MemorySource source(framed);
    cbk::NativePacker packer;
    EXPECT_THROW(packer.Unpack(
                     source, [](const cbk::EntryMeta&) {}, [](const uint8_t*, size_t) {}),
                 std::runtime_error);
}

}  // namespace
