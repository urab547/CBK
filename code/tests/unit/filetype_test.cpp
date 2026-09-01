// Copyright 2026 CBK Project. 文件类型支持的测试：符号链接、junction、硬链接。
//
// 三种类型的权限要求完全不同，这直接决定了哪些用例在什么机器上能跑：
//
//   junction  —— 不需要任何特权，哪台机器都能跑
//   硬链接    —— 不需要任何特权，哪台机器都能跑
//   符号链接  —— 要么管理员，要么开了开发者模式，否则建不出来
//
// 所以只有符号链接那几条带 GTEST_SKIP。CI 的运行器是提权的，在那边三种
// 都会真正执行。

#include <windows.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "cbk/engine.h"
#include "cbk/packer.h"
#include "cbk/stage.h"
#include "cbk/text.h"
#include "cbk/types.h"
#include "src/platform_win.h"
#include "src/reparse.h"
#include "src/scanner.h"
#include "unit/temp_dir.h"

namespace {

namespace pf = cbk::platform;

void EnsureRegistered() {
    static bool done = [] {
        cbk::RegisterBuiltinPackers();
        cbk::RegisterBuiltinStages();
        return true;
    }();
    (void)done;
}

/// 这台机器能不能建符号链接。建不了的用例直接跳过，不判失败——
/// 不能因为测试机没开开发者模式就让 CI 之外的人跑不过测试。
bool CanCreateSymlinks(const cbk_test::TempDir& temp) {
    static int cached = -1;
    if (cached >= 0) return cached != 0;
    const std::wstring probe = temp.At(L"__symlink_probe");
    uint32_t error = 0;
    cached = cbk::CreateSymlink(probe, L"nowhere", false, &error) ? 1 : 0;
    if (cached != 0) DeleteFileW(pf::ToExtendedPath(probe).c_str());
    return cached != 0;
}

std::vector<cbk::EntryMeta> ScanTree(const std::wstring& root, bool follow) {
    cbk::ScanOptions options;
    options.follow_symlinks = follow;
    cbk::Scanner scanner(root, options, nullptr);
    std::vector<cbk::EntryMeta> entries;
    scanner.Scan([&entries](const cbk::EntryMeta& meta) { entries.push_back(meta); }, nullptr);
    return entries;
}

std::map<std::wstring, cbk::EntryMeta> ByPath(const std::vector<cbk::EntryMeta>& entries) {
    std::map<std::wstring, cbk::EntryMeta> index;
    for (const cbk::EntryMeta& entry : entries) index[entry.relative_path] = entry;
    return index;
}

cbk::BackupOptions BackupTo(const std::wstring& source, const std::wstring& archive) {
    cbk::BackupOptions options;
    options.source_root = source;
    options.dest_archive = archive;
    return options;
}

cbk::RestoreOptions RestoreTo(const std::wstring& archive, const std::wstring& dest) {
    cbk::RestoreOptions options;
    options.archive = archive;
    options.dest_root = dest;
    return options;
}

/// 一个文件实体被几个目录项指着。
uint32_t LinkCountOf(const std::wstring& path) {
    pf::ScopedHandle handle = pf::OpenForRead(path, true);
    if (!handle.IsValid()) return 0;
    BY_HANDLE_FILE_INFORMATION info = {};
    if (GetFileInformationByHandle(handle.Get(), &info) == 0) return 0;
    return info.nNumberOfLinks;
}

/// 文件实体的唯一身份。两个路径指向同一个实体时这个值相同。
uint64_t FileIndexOf(const std::wstring& path) {
    pf::ScopedHandle handle = pf::OpenForRead(path, true);
    if (!handle.IsValid()) return 0;
    BY_HANDLE_FILE_INFORMATION info = {};
    if (GetFileInformationByHandle(handle.Get(), &info) == 0) return 0;
    return (static_cast<uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
}

// ============================================================ junction

TEST(FileType, JunctionRoundTrips) {
    EnsureRegistered();
    cbk_test::TempDir temp;
    temp.MakeDir(L"src");
    const std::wstring target = temp.MakeDir(L"目标目录");
    temp.MakeFile(L"目标目录\\里面.txt", "链接指向的内容");

    uint32_t error = 0;
    ASSERT_TRUE(cbk::CreateJunction(temp.At(L"src\\联接"), target, &error))
        << "建 junction 失败，Win32 错误 " << error << "（junction 不该需要任何特权）";

    // ---- 备份侧：类型和目标都要记对 ----
    const auto scanned = ByPath(ScanTree(temp.At(L"src"), false));
    ASSERT_EQ(1u, scanned.count(L"联接"));
    const cbk::EntryMeta& entry = scanned.at(L"联接");
    EXPECT_EQ(cbk::FileType::kJunction, entry.type);
    EXPECT_EQ(static_cast<uint32_t>(IO_REPARSE_TAG_MOUNT_POINT), entry.reparse_tag);
    EXPECT_EQ(pf::NormalizePath(target), entry.link_target);

    // 默认不跟随：链接底下的东西不能被当成第二份备份进来。
    EXPECT_EQ(0u, scanned.count(L"联接\\里面.txt"));

    // ---- 还原侧 ----
    ASSERT_EQ(cbk::Status::kOk,
              cbk::RunBackup(BackupTo(temp.At(L"src"), temp.At(L"o.cbk")), nullptr).status);
    ASSERT_EQ(cbk::Status::kOk,
              cbk::RunRestore(RestoreTo(temp.At(L"o.cbk"), temp.At(L"back")), nullptr).status);

    cbk::ReparseInfo restored;
    ASSERT_TRUE(cbk::ReadReparsePoint(temp.At(L"back\\联接"), &restored, &error))
        << "还原出来的不是重解析点，Win32 错误 " << error;
    EXPECT_EQ(cbk::FileType::kJunction, restored.type);
    EXPECT_EQ(pf::NormalizePath(target), restored.target);

    // 建出来的确实是活的 junction：穿过它能读到目标里的文件。
    pf::ScopedHandle through =
        pf::OpenForRead(temp.At(L"back\\联接\\里面.txt"), /*no_follow=*/false);
    EXPECT_TRUE(through.IsValid()) << "junction 建出来了但穿不过去";
}

TEST(FileType, FollowModeTurnsJunctionIntoRealDirectory) {
    // 跟随模式下，链接要按它指向的东西记，不能记成链接。
    //
    // 不这么做会得到一个自相矛盾的包：类型写着"目录联接"，底下却挂着子项。
    // 还原时先建出联接、再往里写子项，等于把文件写进了联接指向的真实目录，
    // 直接污染源数据。
    EnsureRegistered();
    cbk_test::TempDir temp;
    temp.MakeDir(L"src");
    const std::wstring target = temp.MakeDir(L"目标目录");
    temp.MakeFile(L"目标目录\\里面.txt", "内容");

    uint32_t error = 0;
    ASSERT_TRUE(cbk::CreateJunction(temp.At(L"src\\联接"), target, &error));

    const auto followed = ByPath(ScanTree(temp.At(L"src"), true));
    ASSERT_EQ(1u, followed.count(L"联接"));
    EXPECT_EQ(cbk::FileType::kDirectory, followed.at(L"联接").type)
        << "跟随模式下还记成 junction，还原时会往真实目录里写东西";
    EXPECT_TRUE(followed.at(L"联接").link_target.empty());
    EXPECT_EQ(1u, followed.count(L"联接\\里面.txt")) << "跟随模式应该走进去";
}

TEST(FileType, UnknownReparseTagIsRecordedNotRestored) {
    // 手头造不出 OneDrive 占位符那种重解析点，所以这条只验证类型枚举和
    // ToString 的约定还在——真遇到时靠的是 scanner 里的 default 分支。
    EXPECT_STREQ("unsupported", cbk::ToString(cbk::FileType::kUnsupported));
    EXPECT_STREQ("junction", cbk::ToString(cbk::FileType::kJunction));
    EXPECT_STREQ("symlink-file", cbk::ToString(cbk::FileType::kSymlinkFile));
    EXPECT_STREQ("symlink-dir", cbk::ToString(cbk::FileType::kSymlinkDir));
}

// ============================================================ 硬链接

TEST(FileType, HardlinksAreDeduplicatedAndRestoredAsLinks) {
    EnsureRegistered();
    cbk_test::TempDir temp;
    temp.MakeDir(L"src");
    const std::string payload = "这份内容只该在包里存一次";
    temp.MakeFile(L"src\\原文件.txt", payload);

    ASSERT_NE(0, CreateHardLinkW(pf::ToExtendedPath(temp.At(L"src\\硬链接甲.txt")).c_str(),
                                 pf::ToExtendedPath(temp.At(L"src\\原文件.txt")).c_str(), nullptr))
        << "建硬链接失败（硬链接不该需要特权）";
    ASSERT_NE(0, CreateHardLinkW(pf::ToExtendedPath(temp.At(L"src\\硬链接乙.txt")).c_str(),
                                 pf::ToExtendedPath(temp.At(L"src\\原文件.txt")).c_str(), nullptr));
    ASSERT_EQ(3u, LinkCountOf(temp.At(L"src\\原文件.txt")));

    ASSERT_EQ(cbk::Status::kOk,
              cbk::RunBackup(BackupTo(temp.At(L"src"), temp.At(L"o.cbk")), nullptr).status);

    // ---- 包里应该只有一份内容 ----
    cbk::ArchiveInfo info;
    std::vector<cbk::EntryMeta> entries;
    std::wstring error;
    ASSERT_EQ(cbk::Status::kOk,
              cbk::ReadArchiveListing(temp.At(L"o.cbk"), "", &info, &entries, &error))
        << cbk::ToUtf8(error);
    ASSERT_EQ(3u, entries.size());

    int regular = 0;
    int refs = 0;
    uint64_t content_id = 0;
    for (const cbk::EntryMeta& entry : entries) {
        if (entry.type == cbk::FileType::kRegular) {
            ++regular;
            content_id = entry.id;
        } else if (entry.type == cbk::FileType::kHardlinkRef) {
            ++refs;
        }
    }
    EXPECT_EQ(1, regular) << "三个目录项指向同一份内容，只该有一条存内容";
    EXPECT_EQ(2, refs);

    for (const cbk::EntryMeta& entry : entries) {
        if (entry.type != cbk::FileType::kHardlinkRef) continue;
        EXPECT_EQ(content_id, entry.hardlink_ref_id);
        EXPECT_LT(entry.hardlink_ref_id, entry.id)
            << "硬链接必须指向更小的 id，否则按 id 升序还原时目标还没建出来";
        EXPECT_EQ(0u, entry.original_size) << "引用条目不该占内容长度";
    }

    // ---- 还原之后仍然是同一个实体，不是三份拷贝 ----
    ASSERT_EQ(cbk::Status::kOk,
              cbk::RunRestore(RestoreTo(temp.At(L"o.cbk"), temp.At(L"back")), nullptr).status);

    EXPECT_EQ(payload, temp.ReadFile(L"back\\原文件.txt"));
    EXPECT_EQ(payload, temp.ReadFile(L"back\\硬链接甲.txt"));
    EXPECT_EQ(payload, temp.ReadFile(L"back\\硬链接乙.txt"));

    const uint64_t index = FileIndexOf(temp.At(L"back\\原文件.txt"));
    EXPECT_NE(0u, index);
    EXPECT_EQ(index, FileIndexOf(temp.At(L"back\\硬链接甲.txt")))
        << "还原成了独立的拷贝，硬链接关系丢了";
    EXPECT_EQ(index, FileIndexOf(temp.At(L"back\\硬链接乙.txt")));
    EXPECT_EQ(3u, LinkCountOf(temp.At(L"back\\原文件.txt")));
}

TEST(FileType, SingleLinkedFilesAreNotTouchedByDedup) {
    // 只对 nNumberOfLinks > 1 的文件查表。普通文件走的是原来那条路，
    // 这条用例保证去重逻辑没把它们也卷进去。
    EnsureRegistered();
    cbk_test::TempDir temp;
    temp.MakeDir(L"src");
    temp.MakeFile(L"src\\甲.txt", "内容甲");
    temp.MakeFile(L"src\\乙.txt", "内容乙");

    ASSERT_EQ(cbk::Status::kOk,
              cbk::RunBackup(BackupTo(temp.At(L"src"), temp.At(L"o.cbk")), nullptr).status);

    cbk::ArchiveInfo info;
    std::vector<cbk::EntryMeta> entries;
    std::wstring error;
    ASSERT_EQ(cbk::Status::kOk,
              cbk::ReadArchiveListing(temp.At(L"o.cbk"), "", &info, &entries, &error));
    for (const cbk::EntryMeta& entry : entries) {
        EXPECT_NE(cbk::FileType::kHardlinkRef, entry.type) << cbk::ToUtf8(entry.relative_path);
    }

    ASSERT_EQ(cbk::Status::kOk,
              cbk::RunRestore(RestoreTo(temp.At(L"o.cbk"), temp.At(L"back")), nullptr).status);
    EXPECT_EQ("内容甲", temp.ReadFile(L"back\\甲.txt"));
    EXPECT_EQ("内容乙", temp.ReadFile(L"back\\乙.txt"));
}

// ============================================================ 符号链接

TEST(FileType, AbsoluteFileSymlinkRoundTrips) {
    EnsureRegistered();
    cbk_test::TempDir temp;
    temp.MakeDir(L"src");
    const std::wstring target = temp.MakeFile(L"目标.txt", "目标文件的内容");
    if (!CanCreateSymlinks(temp)) {
        GTEST_SKIP() << "这台机器建不了符号链接（既非管理员，也没开开发者模式）";
    }

    uint32_t error = 0;
    ASSERT_TRUE(cbk::CreateSymlink(temp.At(L"src\\文件链接"), target, false, &error));

    const auto scanned = ByPath(ScanTree(temp.At(L"src"), false));
    ASSERT_EQ(1u, scanned.count(L"文件链接"));
    EXPECT_EQ(cbk::FileType::kSymlinkFile, scanned.at(L"文件链接").type);
    EXPECT_EQ(target, scanned.at(L"文件链接").link_target);
    EXPECT_FALSE(scanned.at(L"文件链接").link_is_relative);

    ASSERT_EQ(cbk::Status::kOk,
              cbk::RunBackup(BackupTo(temp.At(L"src"), temp.At(L"o.cbk")), nullptr).status);
    ASSERT_EQ(cbk::Status::kOk,
              cbk::RunRestore(RestoreTo(temp.At(L"o.cbk"), temp.At(L"back")), nullptr).status);

    cbk::ReparseInfo restored;
    ASSERT_TRUE(cbk::ReadReparsePoint(temp.At(L"back\\文件链接"), &restored, &error));
    EXPECT_EQ(cbk::FileType::kSymlinkFile, restored.type);
    EXPECT_EQ(target, restored.target);
}

TEST(FileType, RelativeSymlinkStaysRelative) {
    // 相对链接必须原样保留。解析成绝对路径再存的话，还原到别的目录下
    // 就指错地方了——这正是不用 GetFinalPathNameByHandle 的原因。
    EnsureRegistered();
    cbk_test::TempDir temp;
    temp.MakeDir(L"src");
    temp.MakeFile(L"src\\邻居.txt", "同级文件");
    if (!CanCreateSymlinks(temp)) {
        GTEST_SKIP() << "这台机器建不了符号链接";
    }

    uint32_t error = 0;
    ASSERT_TRUE(cbk::CreateSymlink(temp.At(L"src\\相对链接"), L"邻居.txt", false, &error));

    const auto scanned = ByPath(ScanTree(temp.At(L"src"), false));
    ASSERT_EQ(1u, scanned.count(L"相对链接"));
    EXPECT_EQ(L"邻居.txt", scanned.at(L"相对链接").link_target) << "相对目标被解析成绝对路径了";
    EXPECT_TRUE(scanned.at(L"相对链接").link_is_relative);

    ASSERT_EQ(cbk::Status::kOk,
              cbk::RunBackup(BackupTo(temp.At(L"src"), temp.At(L"o.cbk")), nullptr).status);
    ASSERT_EQ(cbk::Status::kOk,
              cbk::RunRestore(RestoreTo(temp.At(L"o.cbk"), temp.At(L"back")), nullptr).status);

    cbk::ReparseInfo restored;
    ASSERT_TRUE(cbk::ReadReparsePoint(temp.At(L"back\\相对链接"), &restored, &error));
    EXPECT_EQ(L"邻居.txt", restored.target);
    EXPECT_TRUE(restored.is_relative);

    // 相对链接跟着树一起搬到新目录之后，仍然指向新目录里的邻居。
    pf::ScopedHandle through = pf::OpenForRead(temp.At(L"back\\相对链接"), false);
    EXPECT_TRUE(through.IsValid()) << "搬家之后相对链接失效了";
}

TEST(FileType, DirectorySymlinkKeepsItsDirectoryFlag) {
    // 目录链接和文件链接必须分开记：还原时 CreateSymbolicLinkW 要靠这个
    // 决定加不加 SYMBOLIC_LINK_FLAG_DIRECTORY，判错了建出来的链接是坏的。
    EnsureRegistered();
    cbk_test::TempDir temp;
    temp.MakeDir(L"src");
    const std::wstring target = temp.MakeDir(L"目标目录");
    temp.MakeFile(L"目标目录\\里面.txt", "内容");
    if (!CanCreateSymlinks(temp)) {
        GTEST_SKIP() << "这台机器建不了符号链接";
    }

    uint32_t error = 0;
    ASSERT_TRUE(cbk::CreateSymlink(temp.At(L"src\\目录链接"), target, true, &error));

    const auto scanned = ByPath(ScanTree(temp.At(L"src"), false));
    ASSERT_EQ(1u, scanned.count(L"目录链接"));
    EXPECT_EQ(cbk::FileType::kSymlinkDir, scanned.at(L"目录链接").type);
    EXPECT_EQ(0u, scanned.count(L"目录链接\\里面.txt")) << "默认不该跟随";

    ASSERT_EQ(cbk::Status::kOk,
              cbk::RunBackup(BackupTo(temp.At(L"src"), temp.At(L"o.cbk")), nullptr).status);
    ASSERT_EQ(cbk::Status::kOk,
              cbk::RunRestore(RestoreTo(temp.At(L"o.cbk"), temp.At(L"back")), nullptr).status);

    cbk::ReparseInfo restored;
    ASSERT_TRUE(cbk::ReadReparsePoint(temp.At(L"back\\目录链接"), &restored, &error));
    EXPECT_EQ(cbk::FileType::kSymlinkDir, restored.type) << "还原成了文件链接，DIRECTORY 标志丢了";

    pf::ScopedHandle through = pf::OpenForRead(temp.At(L"back\\目录链接\\里面.txt"), false);
    EXPECT_TRUE(through.IsValid()) << "目录链接建出来了但穿不过去";
}

TEST(FileType, MixedTreeRoundTripsInOneGo) {
    // 一棵同时含普通文件、目录、junction、硬链接的树。分开测各自没问题、
    // 混在一起却错位的情况，只有这种用例能抓住。
    EnsureRegistered();
    cbk_test::TempDir temp;
    temp.MakeDir(L"src");
    temp.MakeFile(L"src\\甲.txt", "甲的内容");
    temp.MakeDir(L"src\\子目录");
    temp.MakeFile(L"src\\子目录\\乙.txt", "乙的内容");
    ASSERT_NE(0, CreateHardLinkW(pf::ToExtendedPath(temp.At(L"src\\甲的硬链接.txt")).c_str(),
                                 pf::ToExtendedPath(temp.At(L"src\\甲.txt")).c_str(), nullptr));
    const std::wstring target = temp.MakeDir(L"外部目标");
    uint32_t error = 0;
    ASSERT_TRUE(cbk::CreateJunction(temp.At(L"src\\联接"), target, &error));

    ASSERT_EQ(cbk::Status::kOk,
              cbk::RunBackup(BackupTo(temp.At(L"src"), temp.At(L"o.cbk")), nullptr).status);
    std::wstring verify_error;
    ASSERT_EQ(cbk::Status::kOk, cbk::VerifyArchive(temp.At(L"o.cbk"), &verify_error))
        << cbk::ToUtf8(verify_error);
    ASSERT_EQ(cbk::Status::kOk,
              cbk::RunRestore(RestoreTo(temp.At(L"o.cbk"), temp.At(L"back")), nullptr).status);

    EXPECT_EQ("甲的内容", temp.ReadFile(L"back\\甲.txt"));
    EXPECT_EQ("甲的内容", temp.ReadFile(L"back\\甲的硬链接.txt"));
    EXPECT_EQ("乙的内容", temp.ReadFile(L"back\\子目录\\乙.txt"));
    EXPECT_EQ(FileIndexOf(temp.At(L"back\\甲.txt")), FileIndexOf(temp.At(L"back\\甲的硬链接.txt")));

    cbk::ReparseInfo restored;
    ASSERT_TRUE(cbk::ReadReparsePoint(temp.At(L"back\\联接"), &restored, &error));
    EXPECT_EQ(cbk::FileType::kJunction, restored.type);
}

}  // namespace
