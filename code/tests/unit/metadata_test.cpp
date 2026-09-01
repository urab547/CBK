// Copyright 2026 CBK Project. 元数据读写的单元测试。

#include <windows.h>

#include <cstdint>
#include <string>

#include <gtest/gtest.h>

#include "cbk/text.h"
#include "cbk/types.h"
#include "src/metadata.h"
#include "src/platform_win.h"
#include "src/scanner.h"
#include "unit/temp_dir.h"

namespace {

namespace pf = cbk::platform;

uint64_t LastWriteOf(const std::wstring& path) {
    cbk::EntryMeta meta;
    EXPECT_TRUE(cbk::ReadMetadataByPath(path, true, &meta));
    return meta.last_write_time;
}

// ============================================================ 陈旧元数据

TEST(Metadata, DirectoryTimestampFromFindDataCanBeStale) {
    // 这条用例钉住的是一个真实踩过的坑，而且它**不是**还原侧的 bug。
    //
    // FindFirstFileW 返回的元数据来自父目录的索引项，NTFS 更新它是惰性的。
    // 刚往一个目录里写完文件就去遍历，读到的 lastWriteTime 会比对象自己
    // MFT 记录里的权威值旧——实测差过 8 毫秒。往返测试会挂在目录时间戳上，
    // 看着像 Pass 3 写错了，其实是备份的第一步就记错了。
    //
    // Scanner 现在对非普通文件会从句柄重读一遍，这条用例保证它别退回去。
    cbk_test::TempDir temp;
    temp.MakeDir(L"d");
    temp.MakeFile(L"d\\child.txt", "刚写进去的内容");

    cbk::Scanner scanner(temp.Path(), cbk::ScanOptions{}, nullptr);
    cbk::EntryMeta directory;
    bool found = false;
    scanner.Scan(
        [&](const cbk::EntryMeta& meta) {
            if (meta.relative_path == L"d") {
                directory = meta;
                found = true;
            }
        },
        nullptr);
    ASSERT_TRUE(found);

    EXPECT_EQ(LastWriteOf(temp.At(L"d")), directory.last_write_time)
        << "Scanner 记的目录时间戳不是权威值——它又去信 WIN32_FIND_DATAW 了";
}

TEST(Metadata, ReadFromHandleMatchesReadByPath) {
    cbk_test::TempDir temp;
    const std::wstring file = temp.MakeFile(L"a.txt", "content");

    cbk::EntryMeta by_path;
    ASSERT_TRUE(cbk::ReadMetadataByPath(file, true, &by_path));

    pf::ScopedHandle handle = pf::OpenForRead(file, true);
    ASSERT_TRUE(handle.IsValid());
    cbk::EntryMeta by_handle;
    ASSERT_TRUE(cbk::ReadMetadataFromHandle(handle.Get(), &by_handle));

    EXPECT_EQ(by_path.last_write_time, by_handle.last_write_time);
    EXPECT_EQ(by_path.creation_time, by_handle.creation_time);
    EXPECT_EQ(by_path.attributes, by_handle.attributes);
}

TEST(Metadata, ReadByPathFailsGracefullyOnMissingFile) {
    cbk_test::TempDir temp;
    cbk::EntryMeta meta;
    EXPECT_FALSE(cbk::ReadMetadataByPath(temp.At(L"不存在"), true, &meta));
}

// ============================================================ 属性位

TEST(Metadata, SettableMaskExcludesFilesystemManagedBits) {
    // 这几位是文件系统自己维护的，塞给 SetFileAttributesW 会让整个调用
    // 返回 ERROR_INVALID_PARAMETER——不是"忽略这几位"，是整个失败。
    EXPECT_EQ(0u, cbk::kSettableAttributes & FILE_ATTRIBUTE_DIRECTORY);
    EXPECT_EQ(0u, cbk::kSettableAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
    EXPECT_EQ(0u, cbk::kSettableAttributes & FILE_ATTRIBUTE_COMPRESSED);
    EXPECT_EQ(0u, cbk::kSettableAttributes & FILE_ATTRIBUTE_ENCRYPTED);
    EXPECT_EQ(0u, cbk::kSettableAttributes & FILE_ATTRIBUTE_SPARSE_FILE);

    EXPECT_NE(0u, cbk::kSettableAttributes & FILE_ATTRIBUTE_READONLY);
    EXPECT_NE(0u, cbk::kSettableAttributes & FILE_ATTRIBUTE_HIDDEN);
    EXPECT_NE(0u, cbk::kSettableAttributes & FILE_ATTRIBUTE_SYSTEM);
    EXPECT_NE(0u, cbk::kSettableAttributes & FILE_ATTRIBUTE_ARCHIVE);
}

TEST(Metadata, ApplyAttributesMasksOutUnsettableBits) {
    cbk_test::TempDir temp;
    const std::wstring file = temp.MakeFile(L"a.txt", "x");

    cbk::EntryMeta meta;
    // 混进几个不可设置的位。不屏蔽的话整个调用会失败。
    meta.attributes = FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_DIRECTORY |
                      FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_COMPRESSED;

    uint32_t error = 0;
    EXPECT_TRUE(cbk::ApplyAttributes(file, meta, &error)) << "Win32 错误 " << error;

    const DWORD actual = GetFileAttributesW(pf::ToExtendedPath(file).c_str());
    ASSERT_NE(INVALID_FILE_ATTRIBUTES, actual);
    EXPECT_NE(0u, actual & FILE_ATTRIBUTE_HIDDEN);
    EXPECT_EQ(0u, actual & FILE_ATTRIBUTE_DIRECTORY);
}

TEST(Metadata, ApplyAttributesWithNothingToSetIsNoOp) {
    // 一位都不用设时不能去调 SetFileAttributesW：它不接受 0，
    // 得传 FILE_ATTRIBUTE_NORMAL，而那会把已有的位全清掉。
    cbk_test::TempDir temp;
    const std::wstring file = temp.MakeFile(L"a.txt", "x");
    ASSERT_NE(0, SetFileAttributesW(pf::ToExtendedPath(file).c_str(), FILE_ATTRIBUTE_HIDDEN));

    cbk::EntryMeta meta;
    meta.attributes = FILE_ATTRIBUTE_DIRECTORY;  // 全是不可设置的位

    uint32_t error = 0;
    EXPECT_TRUE(cbk::ApplyAttributes(file, meta, &error));
    const DWORD actual = GetFileAttributesW(pf::ToExtendedPath(file).c_str());
    EXPECT_NE(0u, actual & FILE_ATTRIBUTE_HIDDEN) << "原有的 HIDDEN 位被抹掉了";
}

// ============================================================ 时间戳

TEST(Metadata, ApplyTimestampsRoundTripsAt100NsPrecision) {
    cbk_test::TempDir temp;
    const std::wstring file = temp.MakeFile(L"a.txt", "x");

    cbk::EntryMeta meta;
    // 一个精确到 100ns、末位不为零的值。存 time_t（秒）会把它抹平。
    meta.creation_time = 0x01D9ABCDEF012345ull;
    meta.last_access_time = 0x01D9ABCDEF012346ull;
    meta.last_write_time = 0x01D9ABCDEF012347ull;

    uint32_t error = 0;
    ASSERT_TRUE(cbk::ApplyTimestamps(file, meta, true, &error)) << "Win32 错误 " << error;

    cbk::EntryMeta read_back;
    ASSERT_TRUE(cbk::ReadMetadataByPath(file, true, &read_back));
    EXPECT_EQ(meta.creation_time, read_back.creation_time);
    EXPECT_EQ(meta.last_write_time, read_back.last_write_time);
}

TEST(Metadata, ApplyTimestampsWorksOnDirectories) {
    // 目录也要能设——打开目录句柄靠的是 FILE_FLAG_BACKUP_SEMANTICS。
    cbk_test::TempDir temp;
    const std::wstring dir = temp.MakeDir(L"d");

    cbk::EntryMeta meta;
    meta.last_write_time = 0x01D9ABCDEF012347ull;

    uint32_t error = 0;
    ASSERT_TRUE(cbk::ApplyTimestamps(dir, meta, true, &error)) << "Win32 错误 " << error;
    EXPECT_EQ(meta.last_write_time, LastWriteOf(dir));
}

TEST(Metadata, ZeroTimestampMeansLeaveAlone) {
    // 0 表示"没记录"。传 nullptr 给 SetFileTime 就是这一项别动，
    // 比写成 1601-01-01 强。
    cbk_test::TempDir temp;
    const std::wstring file = temp.MakeFile(L"a.txt", "x");
    const uint64_t before = LastWriteOf(file);

    cbk::EntryMeta meta;  // 三个时间戳全是 0
    uint32_t error = 0;
    EXPECT_TRUE(cbk::ApplyTimestamps(file, meta, true, &error));
    EXPECT_EQ(before, LastWriteOf(file));
}

TEST(Metadata, ApplyTimestampsFailsOnMissingFile) {
    cbk_test::TempDir temp;
    cbk::EntryMeta meta;
    meta.last_write_time = 0x01D9ABCDEF012347ull;
    uint32_t error = 0;
    EXPECT_FALSE(cbk::ApplyTimestamps(temp.At(L"不存在"), meta, true, &error));
    EXPECT_NE(0u, error);
}

}  // namespace
