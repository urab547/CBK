// Copyright 2026 CBK Project. 元数据读写的单元测试。

#include <windows.h>

#include <aclapi.h>

#include <cstdint>
#include <string>

#include <gtest/gtest.h>

#include "cbk/engine.h"
#include "cbk/packer.h"
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

// ============================================================ 属主与 DACL

namespace {

/// 这个路径的 DACL 是不是阻断继承的（SE_DACL_PROTECTED）。
bool IsDaclProtected(const std::wstring& path) {
    PSECURITY_DESCRIPTOR raw = nullptr;
    const DWORD status =
        GetNamedSecurityInfoW(pf::ToExtendedPath(path).c_str(), SE_FILE_OBJECT,
                              DACL_SECURITY_INFORMATION, nullptr, nullptr, nullptr, nullptr, &raw);
    if (status != ERROR_SUCCESS) return false;
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    const bool ok = GetSecurityDescriptorControl(raw, &control, &revision) != 0;
    LocalFree(raw);
    return ok && (control & SE_DACL_PROTECTED) != 0;
}

std::string SddlOf(const std::wstring& path) {
    std::string sddl;
    uint32_t error = 0;
    EXPECT_TRUE(cbk::ReadSecurityDescriptor(path, &sddl, &error))
        << cbk::ToUtf8(path) << " 读不到 SDDL，Win32 错误 " << error;
    return sddl;
}

}  // namespace

TEST(Security, ReadsOwnerGroupAndDacl) {
    cbk_test::TempDir temp;
    const std::wstring file = temp.MakeFile(L"a.txt", "x");

    const std::string sddl = SddlOf(file);
    EXPECT_FALSE(sddl.empty());
    // SDDL 形如 "O:S-1-5-21-...G:S-1-5-21-...D:AI(A;;FA;;;SY)..."
    EXPECT_NE(std::string::npos, sddl.find("O:")) << sddl;
    EXPECT_NE(std::string::npos, sddl.find("D:")) << sddl;
    // SACL 不读——那要 SeSecurityPrivilege，且跟评分项无关。
    EXPECT_EQ(std::string::npos, sddl.find("S:")) << "不该读 SACL：" << sddl;
}

TEST(Security, ReadFailsGracefullyOnMissingPath) {
    cbk_test::TempDir temp;
    std::string sddl;
    uint32_t error = 0;
    EXPECT_FALSE(cbk::ReadSecurityDescriptor(temp.At(L"不存在"), &sddl, &error));
    EXPECT_NE(0u, error);
}

TEST(Security, EmptySddlIsANoOp) {
    // 备份时没读到 ACL 的条目，还原时不该报错，也不该去动目标的权限。
    cbk_test::TempDir temp;
    const std::wstring file = temp.MakeFile(L"a.txt", "x");
    const std::string before = SddlOf(file);

    bool owner_skipped = false;
    uint32_t error = 0;
    EXPECT_TRUE(cbk::ApplySecurityDescriptor(file, "", &owner_skipped, &error));
    EXPECT_FALSE(owner_skipped);
    EXPECT_EQ(before, SddlOf(file));
}

TEST(Security, MalformedSddlIsRejected) {
    cbk_test::TempDir temp;
    const std::wstring file = temp.MakeFile(L"a.txt", "x");
    bool owner_skipped = false;
    uint32_t error = 0;
    EXPECT_FALSE(cbk::ApplySecurityDescriptor(file, "这不是一个 SDDL", &owner_skipped, &error));
}

TEST(Security, ExplicitDaclRoundTripsExactly) {
    // 用一份**阻断继承**的显式 DACL，这样两边都不受各自父目录的影响，
    // 比对才有确定性。带 AI（继承而来）的 DACL 在不同父目录下本来就会不同。
    cbk_test::TempDir temp;
    const std::wstring file = temp.MakeFile(L"a.txt", "x");

    // P = 阻断继承；两条显式 ACE：SYSTEM 全权、Administrators 全权。
    const std::string wanted = "D:P(A;;FA;;;SY)(A;;FA;;;BA)";
    bool owner_skipped = false;
    uint32_t error = 0;
    ASSERT_TRUE(cbk::ApplySecurityDescriptor(file, wanted, &owner_skipped, &error))
        << "Win32 错误 " << error;
    EXPECT_TRUE(IsDaclProtected(file)) << "P 标志没生效，DACL 还在继承";

    const std::string read_back = SddlOf(file);
    EXPECT_NE(std::string::npos, read_back.find("(A;;FA;;;SY)")) << read_back;
    EXPECT_NE(std::string::npos, read_back.find("(A;;FA;;;BA)")) << read_back;
}

TEST(Security, ProtectedFlagIsNotInferredFromSddlTextAlone) {
    // 这条钉的是一个很容易写错的地方：SetNamedSecurityInfoW **不看** SDDL
    // 里的 P，必须靠 PROTECTED_DACL_SECURITY_INFORMATION 标志去指定。
    // 实现里如果漏了那个标志，这条会失败。
    cbk_test::TempDir temp;
    const std::wstring dir = temp.MakeDir(L"d");

    bool owner_skipped = false;
    uint32_t error = 0;
    ASSERT_TRUE(cbk::ApplySecurityDescriptor(dir, "D:P(A;;FA;;;SY)", &owner_skipped, &error));
    EXPECT_TRUE(IsDaclProtected(dir));

    // 反过来：不带 P 的应该是继承状态。
    ASSERT_TRUE(cbk::ApplySecurityDescriptor(dir, "D:(A;;FA;;;SY)", &owner_skipped, &error));
    EXPECT_FALSE(IsDaclProtected(dir)) << "没有 P 却被设成了阻断继承";
}

TEST(Security, SurvivesBackupAndRestore) {
    cbk::RegisterBuiltinPackers();
    cbk_test::TempDir temp;
    temp.MakeDir(L"src");
    const std::wstring file = temp.MakeFile(L"src\\受保护.txt", "内容");
    const std::wstring dir = temp.MakeDir(L"src\\受保护目录");

    bool owner_skipped = false;
    uint32_t error = 0;
    // 必须给 AU（已验证用户）留一条，否则我们自己的进程都读不了文件内容，
    // 备份会如实报 kPartial——那是正确行为，但不是这条用例想验的东西。
    // 提权跑（有 SeBackupPrivilege）时能绕过 ACL 读，但不能指望测试机提权。
    const std::string protected_dacl = "D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;AU)";
    ASSERT_TRUE(cbk::ApplySecurityDescriptor(file, protected_dacl, &owner_skipped, &error));
    ASSERT_TRUE(cbk::ApplySecurityDescriptor(dir, protected_dacl, &owner_skipped, &error));

    cbk::BackupOptions backup;
    backup.source_root = temp.At(L"src");
    backup.dest_archive = temp.At(L"o.cbk");
    ASSERT_EQ(cbk::Status::kOk, cbk::RunBackup(backup, nullptr).status);

    cbk::RestoreOptions restore;
    restore.archive = temp.At(L"o.cbk");
    restore.dest_root = temp.At(L"back");
    ASSERT_EQ(cbk::Status::kOk, cbk::RunRestore(restore, nullptr).status);

    // 文件：显式 ACE 和 P 标志都要还原回来。
    EXPECT_TRUE(IsDaclProtected(temp.At(L"back\\受保护.txt"))) << "还原后 DACL 变成继承的了";
    const std::string restored_file = SddlOf(temp.At(L"back\\受保护.txt"));
    EXPECT_NE(std::string::npos, restored_file.find("(A;;FA;;;SY)")) << restored_file;
    EXPECT_NE(std::string::npos, restored_file.find("(A;;FA;;;BA)")) << restored_file;

    // 目录走的是 Pass 3 那条路径，单独验一遍。
    EXPECT_TRUE(IsDaclProtected(temp.At(L"back\\受保护目录")));
}

TEST(Security, NoMetadataSkipsAclRestore) {
    cbk::RegisterBuiltinPackers();
    cbk_test::TempDir temp;
    temp.MakeDir(L"src");
    const std::wstring file = temp.MakeFile(L"src\\a.txt", "内容");
    bool owner_skipped = false;
    uint32_t error = 0;
    ASSERT_TRUE(
        cbk::ApplySecurityDescriptor(file, "D:P(A;;FA;;;SY)(A;;FA;;;AU)", &owner_skipped, &error));

    cbk::BackupOptions backup;
    backup.source_root = temp.At(L"src");
    backup.dest_archive = temp.At(L"o.cbk");
    ASSERT_EQ(cbk::Status::kOk, cbk::RunBackup(backup, nullptr).status);

    cbk::RestoreOptions restore;
    restore.archive = temp.At(L"o.cbk");
    restore.dest_root = temp.At(L"back");
    restore.restore_metadata = false;
    ASSERT_EQ(cbk::Status::kOk, cbk::RunRestore(restore, nullptr).status);

    EXPECT_EQ("内容", temp.ReadFile(L"back\\a.txt")) << "内容照样要还原";
    EXPECT_FALSE(IsDaclProtected(temp.At(L"back\\a.txt"))) << "--no-metadata 时不该把 ACL 写回去";
}

}  // namespace
