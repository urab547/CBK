// Copyright 2026 CBK Project. 目录树遍历的单元测试。
//
// 这些用例都在真实文件系统上跑——遍历这种东西，用假对象测出来的
// "通过"没什么说服力，属性位、重解析点标签、枚举中途出错这些
// 只有真跑一遍才暴露。

#include <windows.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "cbk/event.h"
#include "cbk/types.h"
#include "src/platform_win.h"
#include "src/scanner.h"
#include "unit/temp_dir.h"

namespace {

namespace pf = cbk::platform;

/// 收集遍历结果和 warn 事件。
class CollectingObserver : public cbk::IProgressObserver {
public:
    void OnWarn(const cbk::WarnInfo& info) override { warns.push_back(info); }
    bool IsCancelled() override {
        ++cancel_checks;
        return cancel_after > 0 && cancel_checks > cancel_after;
    }

    std::vector<cbk::WarnInfo> warns;
    int cancel_checks = 0;
    int cancel_after = 0;  ///< 大于 0 时，查到第 N 次之后开始返回取消
};

struct ScanResult {
    cbk::Status status = cbk::Status::kOk;
    std::vector<cbk::EntryMeta> entries;
    cbk::ScanStats stats;
};

ScanResult RunScan(const std::wstring& root, cbk::ScanOptions options,
                   cbk::IProgressObserver* observer) {
    ScanResult result;
    cbk::Scanner scanner(root, options, observer);
    result.status = scanner.Scan(
        [&result](const cbk::EntryMeta& meta) { result.entries.push_back(meta); }, &result.stats);
    return result;
}

ScanResult RunScan(const std::wstring& root) {
    return RunScan(root, cbk::ScanOptions{}, nullptr);
}

/// 按相对路径建索引，方便断言。
std::map<std::wstring, cbk::EntryMeta> ByPath(const std::vector<cbk::EntryMeta>& entries) {
    std::map<std::wstring, cbk::EntryMeta> index;
    for (const cbk::EntryMeta& entry : entries) index[entry.relative_path] = entry;
    return index;
}

/// 尝试建一个符号链接。开发者模式没开、又不是管理员时会失败，
/// 那种情况下相关用例直接跳过——不能因为测试机没开开发者模式就判失败。
bool TryCreateSymlink(const std::wstring& link, const std::wstring& target, bool directory) {
    DWORD flags = directory ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0;
    flags |= 0x2;  // SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
    if (CreateSymbolicLinkW(pf::ToExtendedPath(link).c_str(), target.c_str(), flags) != 0) {
        return true;
    }
    // 老系统上带 0x2 会直接报 ERROR_INVALID_PARAMETER，去掉再试一次。
    flags = directory ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0;
    return CreateSymbolicLinkW(pf::ToExtendedPath(link).c_str(), target.c_str(), flags) != 0;
}

// ============================================================ 基本遍历

TEST(Scanner, FindsFilesAndDirectories) {
    cbk_test::TempDir temp;
    temp.MakeFile(L"a.txt", "aaa");
    temp.MakeDir(L"sub");
    temp.MakeFile(L"sub\\b.txt", "bbbb");
    temp.MakeDir(L"sub\\deeper");
    temp.MakeFile(L"sub\\deeper\\c.bin", "ccccc");

    const ScanResult result = RunScan(temp.Path());
    EXPECT_EQ(cbk::Status::kOk, result.status);
    EXPECT_EQ(5u, result.entries.size());
    EXPECT_EQ(5u, result.stats.entries);
    EXPECT_EQ(0u, result.stats.skipped);
    EXPECT_EQ(3u + 4u + 5u, result.stats.total_bytes);

    const auto index = ByPath(result.entries);
    ASSERT_EQ(1u, index.count(L"a.txt"));
    EXPECT_EQ(cbk::FileType::kRegular, index.at(L"a.txt").type);
    EXPECT_EQ(3u, index.at(L"a.txt").original_size);

    ASSERT_EQ(1u, index.count(L"sub"));
    EXPECT_EQ(cbk::FileType::kDirectory, index.at(L"sub").type);
    EXPECT_EQ(0u, index.at(L"sub").original_size) << "目录的 original_size 必须是 0";

    ASSERT_EQ(1u, index.count(L"sub\\deeper\\c.bin"));
    EXPECT_EQ(5u, index.at(L"sub\\deeper\\c.bin").original_size);
}

TEST(Scanner, DoesNotEmitTheRootItself) {
    // 源根不产出条目：还原目标是另一个目录，把源根的元数据套上去没有意义。
    // 这也保证了 relative_path 永远非空。
    cbk_test::TempDir temp;
    temp.MakeFile(L"only.txt", "x");

    const ScanResult result = RunScan(temp.Path());
    ASSERT_EQ(1u, result.entries.size());
    EXPECT_EQ(L"only.txt", result.entries[0].relative_path);
    for (const cbk::EntryMeta& entry : result.entries) {
        EXPECT_FALSE(entry.relative_path.empty());
        EXPECT_NE(L'\\', entry.relative_path[0]) << "相对路径不能以分隔符开头";
    }
}

TEST(Scanner, EmptyDirectoryYieldsNothing) {
    cbk_test::TempDir temp;
    const ScanResult result = RunScan(temp.Path());
    EXPECT_EQ(cbk::Status::kOk, result.status);
    EXPECT_TRUE(result.entries.empty());
}

TEST(Scanner, MissingRootFails) {
    cbk_test::TempDir temp;
    const ScanResult result = RunScan(temp.At(L"根本不存在"));
    EXPECT_EQ(cbk::Status::kFailed, result.status);
    EXPECT_TRUE(result.entries.empty());
}

TEST(Scanner, IdsAreSequentialAndParentsComeFirst) {
    // 还原时"先建所有目录、再按 id 升序还原内容"能成立，全靠这条顺序保证。
    // 硬链接引用一定指向更小的 id，也是靠它。
    cbk_test::TempDir temp;
    temp.MakeDir(L"p");
    temp.MakeDir(L"p\\q");
    temp.MakeFile(L"p\\q\\leaf.txt", "leaf");
    temp.MakeFile(L"p\\mid.txt", "mid");

    const ScanResult result = RunScan(temp.Path());
    ASSERT_EQ(4u, result.entries.size());

    std::map<std::wstring, uint64_t> id_of;
    for (size_t i = 0; i < result.entries.size(); ++i) {
        EXPECT_EQ(i, result.entries[i].id) << "id 必须从 0 开始连续递增";
        id_of[result.entries[i].relative_path] = result.entries[i].id;
    }

    EXPECT_LT(id_of[L"p"], id_of[L"p\\q"]);
    EXPECT_LT(id_of[L"p"], id_of[L"p\\mid.txt"]);
    EXPECT_LT(id_of[L"p\\q"], id_of[L"p\\q\\leaf.txt"]);
}

// ============================================================ 元数据字段

TEST(Scanner, CapturesAttributesAndTimestamps) {
    cbk_test::TempDir temp;
    const std::wstring file = temp.MakeFile(L"hidden.txt", "data");
    ASSERT_NE(0, SetFileAttributesW(pf::ToExtendedPath(file).c_str(),
                                    FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_ARCHIVE));

    const ScanResult result = RunScan(temp.Path());
    ASSERT_EQ(1u, result.entries.size());
    const cbk::EntryMeta& entry = result.entries[0];

    EXPECT_NE(0u, entry.attributes & FILE_ATTRIBUTE_HIDDEN);
    // 时间戳存 FILETIME 原值（100ns 精度），不转 time_t——转成秒会丢精度，
    // 往返测试立刻挂。
    EXPECT_NE(0u, entry.creation_time);
    EXPECT_NE(0u, entry.last_write_time);
    EXPECT_GT(entry.creation_time, 0x01C0000000000000ull) << "看起来不像一个 FILETIME";
}

TEST(Scanner, LeavesFieldsForLaterModulesEmpty) {
    // 明确记录当前的分工边界：这几个字段 Scanner 不填。
    // link_target 由 #6 填、hardlink_ref_id 由 #7 填、sddl 由 #9 填。
    cbk_test::TempDir temp;
    temp.MakeFile(L"plain.txt", "x");

    const ScanResult result = RunScan(temp.Path());
    ASSERT_EQ(1u, result.entries.size());
    EXPECT_TRUE(result.entries[0].sddl.empty());
    EXPECT_TRUE(result.entries[0].link_target.empty());
    EXPECT_EQ(0u, result.entries[0].hardlink_ref_id);
}

// ============================================================ 名字与深度

TEST(Scanner, HandlesChineseSpacesAndDots) {
    cbk_test::TempDir temp;
    temp.MakeFile(L"中文文件名.txt", "1");
    temp.MakeFile(L"with spaces.txt", "22");
    temp.MakeFile(L"a.b.c.tar.gz", "333");
    temp.MakeDir(L"含空格 的 目录");
    temp.MakeFile(L"含空格 的 目录\\里面.txt", "4444");

    const ScanResult result = RunScan(temp.Path());
    EXPECT_EQ(cbk::Status::kOk, result.status);
    EXPECT_EQ(5u, result.entries.size());

    const auto index = ByPath(result.entries);
    EXPECT_EQ(1u, index.count(L"中文文件名.txt"));
    EXPECT_EQ(1u, index.count(L"with spaces.txt"));
    EXPECT_EQ(1u, index.count(L"a.b.c.tar.gz"));
    EXPECT_EQ(1u, index.count(L"含空格 的 目录\\里面.txt"));
}

TEST(Scanner, SkipsDotAndDotDot) {
    cbk_test::TempDir temp;
    temp.MakeDir(L"d");
    const ScanResult result = RunScan(temp.Path());
    for (const cbk::EntryMeta& entry : result.entries) {
        EXPECT_NE(L".", entry.relative_path);
        EXPECT_NE(L"..", entry.relative_path);
    }
    EXPECT_EQ(1u, result.entries.size());
}

TEST(Scanner, SurvivesDeepTreeWithoutStackOverflow) {
    // 显式栈迭代而不是递归，就是为了这个。改成递归的话这条会崩，
    // 而且崩在用户的深目录上、不是在测试里。
    cbk_test::TempDir temp;
    std::wstring relative;
    const int kDepth = 200;
    for (int i = 0; i < kDepth; ++i) {
        relative = relative.empty() ? L"d" : pf::JoinPath(relative, L"d");
        ASSERT_FALSE(temp.MakeDir(relative).empty());
    }
    temp.MakeFile(pf::JoinPath(relative, L"bottom.txt"), "deep");

    const ScanResult result = RunScan(temp.Path());
    EXPECT_EQ(cbk::Status::kOk, result.status);
    EXPECT_EQ(static_cast<size_t>(kDepth) + 1, result.entries.size());

    const auto index = ByPath(result.entries);
    const std::wstring deepest = pf::JoinPath(relative, L"bottom.txt");
    ASSERT_EQ(1u, index.count(deepest)) << "最深处那个文件没被找到";
    EXPECT_GT(temp.At(deepest).size(), 260u) << "路径没超过 MAX_PATH，这条用例白测了";
}

// ============================================================ 链接

TEST(Scanner, ClassifiesSymlinksWithoutFollowingThem) {
    cbk_test::TempDir temp;
    temp.MakeDir(L"real");
    temp.MakeFile(L"real\\inside.txt", "content");
    temp.MakeFile(L"target.txt", "t");

    const bool dir_link = TryCreateSymlink(temp.At(L"link_to_dir"), temp.At(L"real"), true);
    const bool file_link =
        TryCreateSymlink(temp.At(L"link_to_file"), temp.At(L"target.txt"), false);
    if (!dir_link && !file_link) {
        GTEST_SKIP() << "这台机器建不了符号链接（没开开发者模式且不是管理员）";
    }

    const ScanResult result = RunScan(temp.Path());
    const auto index = ByPath(result.entries);

    if (dir_link) {
        ASSERT_EQ(1u, index.count(L"link_to_dir"));
        // 目录链接和文件链接必须分开：还原时 CreateSymbolicLinkW 要靠
        // 这个决定加不加 SYMBOLIC_LINK_FLAG_DIRECTORY。
        EXPECT_EQ(cbk::FileType::kSymlinkDir, index.at(L"link_to_dir").type);
        EXPECT_EQ(static_cast<uint32_t>(IO_REPARSE_TAG_SYMLINK),
                  index.at(L"link_to_dir").reparse_tag);
        // 默认不跟随：链接底下的内容不能被当成第二份备份进来。
        EXPECT_EQ(0u, index.count(L"link_to_dir\\inside.txt"));
    }
    if (file_link) {
        ASSERT_EQ(1u, index.count(L"link_to_file"));
        EXPECT_EQ(cbk::FileType::kSymlinkFile, index.at(L"link_to_file").type);
    }
}

TEST(Scanner, FollowModeDescendsIntoDirectorySymlink) {
    cbk_test::TempDir temp;
    temp.MakeDir(L"real");
    temp.MakeFile(L"real\\inside.txt", "content");
    if (!TryCreateSymlink(temp.At(L"link"), temp.At(L"real"), true)) {
        GTEST_SKIP() << "这台机器建不了符号链接";
    }

    cbk::ScanOptions options;
    options.follow_symlinks = true;
    const ScanResult result = RunScan(temp.Path(), options, nullptr);
    const auto index = ByPath(result.entries);
    EXPECT_EQ(1u, index.count(L"link\\inside.txt")) << "跟随模式应该走进链接里";
}

TEST(Scanner, FollowModeBreaksSelfReferentialLoop) {
    // 链接指回它自己所在的树。不断环的话这里会无限转下去，
    // 测试超时——这正是要维护 (卷序列号, 文件索引) 集合的原因。
    cbk_test::TempDir temp;
    temp.MakeDir(L"inner");
    temp.MakeFile(L"inner\\file.txt", "x");
    if (!TryCreateSymlink(temp.At(L"inner\\loop"), temp.Path(), true)) {
        GTEST_SKIP() << "这台机器建不了符号链接";
    }

    cbk::ScanOptions options;
    options.follow_symlinks = true;
    const ScanResult result = RunScan(temp.Path(), options, nullptr);
    // 只要能返回就说明环断掉了。条目数不多做断言——具体几条取决于
    // 环在哪一层被拦下，那属于实现细节。
    EXPECT_LT(result.entries.size(), 50u) << "看起来在环里转了很多圈";
}

// ============================================================ 取消

TEST(Scanner, StopsWhenCancelled) {
    cbk_test::TempDir temp;
    for (int i = 0; i < 50; ++i) {
        temp.MakeFile(L"f" + std::to_wstring(i) + L".txt", "x");
    }

    CollectingObserver observer;
    observer.cancel_after = 3;
    const ScanResult result = RunScan(temp.Path(), cbk::ScanOptions{}, &observer);

    EXPECT_EQ(cbk::Status::kFailed, result.status);
    EXPECT_LT(result.entries.size(), 50u) << "取消之后不应该把整棵树走完";
}

TEST(Scanner, RunsToCompletionWhenObserverNeverCancels) {
    cbk_test::TempDir temp;
    temp.MakeFile(L"a.txt", "x");
    temp.MakeFile(L"b.txt", "y");

    CollectingObserver observer;  // cancel_after 为 0，永不取消
    const ScanResult result = RunScan(temp.Path(), cbk::ScanOptions{}, &observer);

    EXPECT_EQ(cbk::Status::kOk, result.status);
    EXPECT_EQ(2u, result.entries.size());
    EXPECT_TRUE(observer.warns.empty());
    EXPECT_GT(observer.cancel_checks, 0) << "引擎必须真的去查取消，而不是摆设";
}

}  // namespace
