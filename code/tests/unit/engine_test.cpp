// Copyright 2026 CBK Project. 备份与还原引擎的端到端测试。
//
// 这是整个项目最有说服力的一组用例：备份一棵真实的树，再还原到另一个
// 目录，然后逐条比对内容、时间戳、属性位。前面每一层的单元测试都通过、
// 拼起来却对不上的情况，只有这种测试能抓住。

#include <windows.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "cbk/engine.h"
#include "cbk/event.h"
#include "cbk/packer.h"
#include "cbk/stage.h"
#include "cbk/text.h"
#include "cbk/types.h"
#include "src/metadata.h"
#include "src/platform_win.h"
#include "unit/temp_dir.h"

namespace {

namespace pf = cbk::platform;

/// 内置算法只需要注册一次，但每个用例都可能是第一个跑的。
void EnsureRegistered() {
    static bool done = [] {
        cbk::RegisterBuiltinPackers();
        cbk::RegisterBuiltinStages();
        return true;
    }();
    (void)done;
}

/// 记录事件，方便断言 warn 有没有发出来。
class Recorder : public cbk::IProgressObserver {
public:
    void OnStart(const cbk::StartInfo& info) override { starts.push_back(info); }
    void OnProgress(const cbk::ProgressInfo&) override { ++progress_count; }
    void OnWarn(const cbk::WarnInfo& info) override { warns.push_back(info); }
    void OnResult(const cbk::ResultInfo& info) override { results.push_back(info); }

    std::vector<cbk::StartInfo> starts;
    std::vector<cbk::WarnInfo> warns;
    std::vector<cbk::ResultInfo> results;
    int progress_count = 0;
};

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

struct Snapshot {
    uint32_t attributes = 0;
    uint64_t creation = 0;
    uint64_t last_write = 0;
};

Snapshot Peek(const std::wstring& path) {
    cbk::EntryMeta meta;
    EXPECT_TRUE(cbk::ReadMetadataByPath(path, true, &meta)) << cbk::ToUtf8(path);
    return Snapshot{meta.attributes, meta.creation_time, meta.last_write_time};
}

void ExpectMetadataMatches(const std::wstring& source, const std::wstring& restored) {
    const Snapshot a = Peek(source);
    const Snapshot b = Peek(restored);
    EXPECT_EQ(a.last_write, b.last_write) << "修改时间不一致：" << cbk::ToUtf8(restored);
    EXPECT_EQ(a.creation, b.creation) << "创建时间不一致：" << cbk::ToUtf8(restored);
    EXPECT_EQ(a.attributes, b.attributes) << "属性位不一致：" << cbk::ToUtf8(restored);
}

/// 造一棵有点内容的树。
void BuildSampleTree(const cbk_test::TempDir& temp) {
    temp.MakeDir(L"src");
    temp.MakeFile(L"src\\顶层.txt", "顶层文件的内容");
    temp.MakeDir(L"src\\子目录");
    temp.MakeFile(L"src\\子目录\\note.md", "# 标题\n正文");
    temp.MakeDir(L"src\\子目录\\更深");
    temp.MakeFile(L"src\\子目录\\更深\\deep.bin", std::string(1000, '\x7F'));
    temp.MakeFile(L"src\\空文件.dat", "");
    temp.MakeDir(L"src\\空目录");
}

// ============================================================ 往返

TEST(Engine, RoundTripsContentAndMetadata) {
    EnsureRegistered();
    cbk_test::TempDir temp;
    BuildSampleTree(temp);

    Recorder observer;
    const cbk::EngineResult backup =
        cbk::RunBackup(BackupTo(temp.At(L"src"), temp.At(L"out.cbk")), &observer);
    ASSERT_EQ(cbk::Status::kOk, backup.status) << cbk::ToUtf8(backup.error);
    EXPECT_EQ(7u, backup.entries_done);
    EXPECT_TRUE(observer.warns.empty());

    ASSERT_EQ(1u, observer.starts.size());
    EXPECT_EQ(7u, observer.starts[0].total_entries) << "OnStart 应该报出真实总量";
    ASSERT_EQ(1u, observer.results.size());
    EXPECT_EQ(cbk::Status::kOk, observer.results[0].status);

    Recorder restore_observer;
    const cbk::EngineResult restore =
        cbk::RunRestore(RestoreTo(temp.At(L"out.cbk"), temp.At(L"back")), &restore_observer);
    ASSERT_EQ(cbk::Status::kOk, restore.status) << cbk::ToUtf8(restore.error);

    // 内容
    EXPECT_EQ(temp.ReadFile(L"src\\顶层.txt"), temp.ReadFile(L"back\\顶层.txt"));
    EXPECT_EQ(temp.ReadFile(L"src\\子目录\\note.md"), temp.ReadFile(L"back\\子目录\\note.md"));
    EXPECT_EQ(temp.ReadFile(L"src\\子目录\\更深\\deep.bin"),
              temp.ReadFile(L"back\\子目录\\更深\\deep.bin"));
    EXPECT_TRUE(temp.ReadFile(L"back\\空文件.dat").empty());

    // 元数据，包括目录
    for (const std::wstring& rel : {L"顶层.txt", L"子目录", L"子目录\\note.md", L"子目录\\更深",
                                    L"子目录\\更深\\deep.bin", L"空文件.dat", L"空目录"}) {
        SCOPED_TRACE(cbk::ToUtf8(rel));
        ExpectMetadataMatches(temp.At(L"src\\" + rel), temp.At(L"back\\" + rel));
    }
}

TEST(Engine, DirectoryTimestampsSurviveTheThreePassOrder) {
    // Pass 3 的存在理由：往目录里写文件会刷新目录的 lastWriteTime，
    // 所以目录时间戳必须在所有内容都写完之后再设。
    // 如果把设时间戳挪进 Pass 1，这条会挂。
    EnsureRegistered();
    cbk_test::TempDir temp;
    temp.MakeDir(L"src");
    temp.MakeDir(L"src\\a");
    temp.MakeDir(L"src\\a\\b");
    for (int i = 0; i < 20; ++i) {
        temp.MakeFile(L"src\\a\\b\\f" + std::to_wstring(i) + L".txt", "内容");
    }

    ASSERT_EQ(cbk::Status::kOk,
              cbk::RunBackup(BackupTo(temp.At(L"src"), temp.At(L"o.cbk")), nullptr).status);
    ASSERT_EQ(cbk::Status::kOk,
              cbk::RunRestore(RestoreTo(temp.At(L"o.cbk"), temp.At(L"back")), nullptr).status);

    ExpectMetadataMatches(temp.At(L"src\\a"), temp.At(L"back\\a"));
    ExpectMetadataMatches(temp.At(L"src\\a\\b"), temp.At(L"back\\a\\b"));
}

TEST(Engine, ReadOnlyFilesRoundTrip) {
    // 属性位必须最后设：先设了 READONLY，后面设时间戳就会失败。
    // 把这个顺序改错的话，这条用例会在时间戳上挂。
    EnsureRegistered();
    cbk_test::TempDir temp;
    temp.MakeDir(L"src");
    const std::wstring file = temp.MakeFile(L"src\\readonly.txt", "只读内容");
    ASSERT_NE(0, SetFileAttributesW(pf::ToExtendedPath(file).c_str(),
                                    FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN));

    ASSERT_EQ(cbk::Status::kOk,
              cbk::RunBackup(BackupTo(temp.At(L"src"), temp.At(L"o.cbk")), nullptr).status);
    ASSERT_EQ(cbk::Status::kOk,
              cbk::RunRestore(RestoreTo(temp.At(L"o.cbk"), temp.At(L"back")), nullptr).status);

    EXPECT_EQ(temp.ReadFile(L"src\\readonly.txt"), temp.ReadFile(L"back\\readonly.txt"));
    ExpectMetadataMatches(file, temp.At(L"back\\readonly.txt"));
}

TEST(Engine, EmptyTreeRoundTrips) {
    EnsureRegistered();
    cbk_test::TempDir temp;
    temp.MakeDir(L"src");

    const cbk::EngineResult backup =
        cbk::RunBackup(BackupTo(temp.At(L"src"), temp.At(L"o.cbk")), nullptr);
    ASSERT_EQ(cbk::Status::kOk, backup.status) << cbk::ToUtf8(backup.error);
    EXPECT_EQ(0u, backup.entries_done);

    const cbk::EngineResult restore =
        cbk::RunRestore(RestoreTo(temp.At(L"o.cbk"), temp.At(L"back")), nullptr);
    EXPECT_EQ(cbk::Status::kOk, restore.status) << cbk::ToUtf8(restore.error);
    EXPECT_NE(INVALID_FILE_ATTRIBUTES,
              GetFileAttributesW(pf::ToExtendedPath(temp.At(L"back")).c_str()));
}

TEST(Engine, LargeFileStreamsThroughUnchanged) {
    // 跨好几个 64 KB 块，验证流式读写没有在块边界上出错。
    EnsureRegistered();
    cbk_test::TempDir temp;
    temp.MakeDir(L"src");
    std::string payload(3 * 64 * 1024 + 12345, '\0');
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<char>((i * 31 + 7) & 0xFF);
    }
    temp.MakeFile(L"src\\big.bin", payload);

    ASSERT_EQ(cbk::Status::kOk,
              cbk::RunBackup(BackupTo(temp.At(L"src"), temp.At(L"o.cbk")), nullptr).status);
    ASSERT_EQ(cbk::Status::kOk,
              cbk::RunRestore(RestoreTo(temp.At(L"o.cbk"), temp.At(L"back")), nullptr).status);

    EXPECT_EQ(payload, temp.ReadFile(L"back\\big.bin"));
}

TEST(Engine, DeepTreeRoundTripsBeyondMaxPath) {
    EnsureRegistered();
    cbk_test::TempDir temp;
    temp.MakeDir(L"src");
    std::wstring relative = L"src";
    for (int i = 0; i < 12; ++i) {
        relative = pf::JoinPath(relative, std::wstring(30, L'd'));
        ASSERT_FALSE(temp.MakeDir(relative).empty());
    }
    temp.MakeFile(pf::JoinPath(relative, L"bottom.txt"), "深处的内容");
    ASSERT_GT(temp.At(relative).size(), 260u) << "路径没超过 MAX_PATH，这条白测了";

    ASSERT_EQ(cbk::Status::kOk,
              cbk::RunBackup(BackupTo(temp.At(L"src"), temp.At(L"o.cbk")), nullptr).status);
    ASSERT_EQ(cbk::Status::kOk,
              cbk::RunRestore(RestoreTo(temp.At(L"o.cbk"), temp.At(L"back")), nullptr).status);

    const std::wstring restored = L"back" + relative.substr(3);
    EXPECT_EQ("深处的内容", temp.ReadFile(pf::JoinPath(restored, L"bottom.txt")));
}

// ============================================================ 参数错误

TEST(Engine, UnknownPackerIsBadArgs) {
    EnsureRegistered();
    cbk_test::TempDir temp;
    temp.MakeDir(L"src");
    cbk::BackupOptions options = BackupTo(temp.At(L"src"), temp.At(L"o.cbk"));
    options.packer = "没有这个打包器";

    const cbk::EngineResult result = cbk::RunBackup(options, nullptr);
    EXPECT_EQ(cbk::Status::kBadArgs, result.status);
    EXPECT_FALSE(result.error.empty());
}

TEST(Engine, UnknownStageIsBadArgs) {
    EnsureRegistered();
    cbk_test::TempDir temp;
    temp.MakeDir(L"src");
    cbk::BackupOptions options = BackupTo(temp.At(L"src"), temp.At(L"o.cbk"));
    options.stages = {"没有这个算法"};

    EXPECT_EQ(cbk::Status::kBadArgs, cbk::RunBackup(options, nullptr).status);
}

TEST(Engine, EmptyPathsAreBadArgs) {
    EnsureRegistered();
    EXPECT_EQ(cbk::Status::kBadArgs, cbk::RunBackup(BackupTo(L"", L"o.cbk"), nullptr).status);
    EXPECT_EQ(cbk::Status::kBadArgs, cbk::RunRestore(RestoreTo(L"o.cbk", L""), nullptr).status);
}

TEST(Engine, MissingSourceFails) {
    EnsureRegistered();
    cbk_test::TempDir temp;
    const cbk::EngineResult result =
        cbk::RunBackup(BackupTo(temp.At(L"不存在"), temp.At(L"o.cbk")), nullptr);
    EXPECT_EQ(cbk::Status::kFailed, result.status);
}

TEST(Engine, MissingArchiveFails) {
    EnsureRegistered();
    cbk_test::TempDir temp;
    EXPECT_EQ(cbk::Status::kFailed,
              cbk::RunRestore(RestoreTo(temp.At(L"没有.cbk"), temp.At(L"back")), nullptr).status);
}

// ============================================================ 部分成功

/// 在 OnStart 触发时删掉一个文件。
///
/// OnStart 的时机正好卡在"遍历已经做完、内容还没开始读"之间，所以能
/// 确定性地造出"条目在索引里、文件却打不开"的局面。
///
/// 为什么不用"另开一个句柄独占锁住"来造这个局面：那个办法**不跨环境**。
/// 实测本地非提权进程会撞上共享冲突、条目被跳过；而 CI 的运行器是提权的、
/// 拿到了 SeBackupPrivilege，同一个文件照样打得开，断言就全反过来了。
/// 顺带说，那对产品是好事——提权跑的时候连独占锁定的文件都能备份。
class DeleteOnStart : public cbk::IProgressObserver {
public:
    explicit DeleteOnStart(std::wstring victim) : victim_(std::move(victim)) {}

    void OnStart(const cbk::StartInfo&) override {
        DeleteFileW(pf::ToExtendedPath(victim_).c_str());
    }
    void OnWarn(const cbk::WarnInfo& info) override { warns.push_back(info); }

    std::vector<cbk::WarnInfo> warns;

private:
    std::wstring victim_;
};

TEST(Engine, UnreadableEntryIsSkippedAndReportedAsPartial) {
    // 备份软件必须能对付"某个文件读不了"——跳过、发 warn、继续，
    // 而不是整个备份失败。退出码 1 的含义就是"包可用，但有条目被跳过"。
    EnsureRegistered();
    cbk_test::TempDir temp;
    temp.MakeDir(L"src");
    temp.MakeFile(L"src\\正常.txt", "这个能备份");
    temp.MakeFile(L"src\\会消失.txt", "这个在遍历之后被删掉");

    DeleteOnStart observer(temp.At(L"src\\会消失.txt"));
    const cbk::EngineResult result =
        cbk::RunBackup(BackupTo(temp.At(L"src"), temp.At(L"o.cbk")), &observer);

    EXPECT_EQ(cbk::Status::kPartial, result.status);
    EXPECT_EQ(1u, result.entries_skipped);
    EXPECT_EQ(1u, result.entries_done);
    ASSERT_FALSE(observer.warns.empty());
    EXPECT_NE(0u, observer.warns[0].win_error) << "warn 应该带上 Win32 错误码";
    EXPECT_EQ(L"会消失.txt", observer.warns[0].path);

    // 关键：包本身仍然是完整可用的，能还原出没被跳过的那些。
    std::wstring error;
    ASSERT_EQ(cbk::Status::kOk, cbk::VerifyArchive(temp.At(L"o.cbk"), &error))
        << cbk::ToUtf8(error);
    ASSERT_EQ(cbk::Status::kOk,
              cbk::RunRestore(RestoreTo(temp.At(L"o.cbk"), temp.At(L"back")), nullptr).status);
    EXPECT_EQ("这个能备份", temp.ReadFile(L"back\\正常.txt"));
}

// ============================================================ 覆盖策略

TEST(Engine, OverwriteSkipLeavesExistingFileAlone) {
    EnsureRegistered();
    cbk_test::TempDir temp;
    temp.MakeDir(L"src");
    temp.MakeFile(L"src\\a.txt", "来自备份");
    ASSERT_EQ(cbk::Status::kOk,
              cbk::RunBackup(BackupTo(temp.At(L"src"), temp.At(L"o.cbk")), nullptr).status);

    temp.MakeDir(L"back");
    temp.MakeFile(L"back\\a.txt", "本来就在那儿");

    cbk::RestoreOptions options = RestoreTo(temp.At(L"o.cbk"), temp.At(L"back"));
    options.overwrite = cbk::OverwritePolicy::kSkip;
    const cbk::EngineResult result = cbk::RunRestore(options, nullptr);

    EXPECT_EQ(cbk::Status::kPartial, result.status);
    EXPECT_EQ("本来就在那儿", temp.ReadFile(L"back\\a.txt"));
}

TEST(Engine, OverwriteForceReplacesExistingFile) {
    EnsureRegistered();
    cbk_test::TempDir temp;
    temp.MakeDir(L"src");
    temp.MakeFile(L"src\\a.txt", "来自备份");
    ASSERT_EQ(cbk::Status::kOk,
              cbk::RunBackup(BackupTo(temp.At(L"src"), temp.At(L"o.cbk")), nullptr).status);

    temp.MakeDir(L"back");
    temp.MakeFile(L"back\\a.txt", "本来就在那儿");

    cbk::RestoreOptions options = RestoreTo(temp.At(L"o.cbk"), temp.At(L"back"));
    options.overwrite = cbk::OverwritePolicy::kForce;
    ASSERT_EQ(cbk::Status::kOk, cbk::RunRestore(options, nullptr).status);
    EXPECT_EQ("来自备份", temp.ReadFile(L"back\\a.txt"));
}

TEST(Engine, OverwriteRenameKeepsBoth) {
    EnsureRegistered();
    cbk_test::TempDir temp;
    temp.MakeDir(L"src");
    temp.MakeFile(L"src\\a.txt", "来自备份");
    ASSERT_EQ(cbk::Status::kOk,
              cbk::RunBackup(BackupTo(temp.At(L"src"), temp.At(L"o.cbk")), nullptr).status);

    temp.MakeDir(L"back");
    temp.MakeFile(L"back\\a.txt", "本来就在那儿");

    cbk::RestoreOptions options = RestoreTo(temp.At(L"o.cbk"), temp.At(L"back"));
    options.overwrite = cbk::OverwritePolicy::kRename;
    ASSERT_EQ(cbk::Status::kOk, cbk::RunRestore(options, nullptr).status);

    EXPECT_EQ("本来就在那儿", temp.ReadFile(L"back\\a.txt"));
    EXPECT_EQ("来自备份", temp.ReadFile(L"back\\a (1).txt"));
}

TEST(Engine, NoMetadataSkipsTimestampRestore) {
    EnsureRegistered();
    cbk_test::TempDir temp;
    temp.MakeDir(L"src");
    const std::wstring file = temp.MakeFile(L"src\\a.txt", "内容");
    // 把源文件的时间戳设成一个明显是过去的值，好和"还原时的当前时间"区分开。
    cbk::EntryMeta old_time;
    old_time.creation_time = 0x01D0000000000000ull;
    old_time.last_write_time = 0x01D0000000000000ull;
    uint32_t error = 0;
    ASSERT_TRUE(cbk::ApplyTimestamps(file, old_time, true, &error));

    ASSERT_EQ(cbk::Status::kOk,
              cbk::RunBackup(BackupTo(temp.At(L"src"), temp.At(L"o.cbk")), nullptr).status);

    cbk::RestoreOptions options = RestoreTo(temp.At(L"o.cbk"), temp.At(L"back"));
    options.restore_metadata = false;
    ASSERT_EQ(cbk::Status::kOk, cbk::RunRestore(options, nullptr).status);

    EXPECT_EQ("内容", temp.ReadFile(L"back\\a.txt")) << "内容照样要还原";
    EXPECT_NE(old_time.last_write_time, Peek(temp.At(L"back\\a.txt")).last_write)
        << "--no-metadata 时不该把时间戳写回去";
}

// ============================================================ list / verify

TEST(Engine, ListingReportsHeaderAndEntries) {
    EnsureRegistered();
    cbk_test::TempDir temp;
    BuildSampleTree(temp);
    ASSERT_EQ(cbk::Status::kOk,
              cbk::RunBackup(BackupTo(temp.At(L"src"), temp.At(L"o.cbk")), nullptr).status);

    cbk::ArchiveInfo info;
    std::vector<cbk::EntryMeta> entries;
    std::wstring error;
    ASSERT_EQ(cbk::Status::kOk,
              cbk::ReadArchiveListing(temp.At(L"o.cbk"), "", &info, &entries, &error))
        << cbk::ToUtf8(error);

    EXPECT_EQ(cbk::kFormatVersion, info.format_version);
    EXPECT_EQ("cbk-native", info.packer);
    EXPECT_TRUE(info.stages.empty());
    EXPECT_EQ(pf::NormalizePath(temp.At(L"src")), info.source_root);
    EXPECT_EQ(7u, info.entry_count);
    EXPECT_EQ(entries.size(), info.entry_count);
    EXPECT_NE(0u, info.created_at);

    std::map<std::wstring, cbk::FileType> by_path;
    for (const cbk::EntryMeta& entry : entries) by_path[entry.relative_path] = entry.type;
    EXPECT_EQ(cbk::FileType::kDirectory, by_path[L"子目录"]);
    EXPECT_EQ(cbk::FileType::kRegular, by_path[L"顶层.txt"]);
}

TEST(Engine, VerifyPassesOnFreshArchiveAndFailsOnCorruption) {
    EnsureRegistered();
    cbk_test::TempDir temp;
    BuildSampleTree(temp);
    const std::wstring archive = temp.At(L"o.cbk");
    ASSERT_EQ(cbk::Status::kOk, cbk::RunBackup(BackupTo(temp.At(L"src"), archive), nullptr).status);

    std::wstring error;
    EXPECT_EQ(cbk::Status::kOk, cbk::VerifyArchive(archive, &error)) << cbk::ToUtf8(error);

    // 往数据区中间改一个字节。
    pf::ScopedHandle handle =
        pf::OpenPath(archive, GENERIC_READ | GENERIC_WRITE, OPEN_EXISTING, true);
    ASSERT_TRUE(handle.IsValid());
    LARGE_INTEGER position;
    position.QuadPart = 400;
    ASSERT_NE(0, SetFilePointerEx(handle.Get(), position, nullptr, FILE_BEGIN));
    const uint8_t junk = 0x5A;
    DWORD written = 0;
    ASSERT_NE(0, WriteFile(handle.Get(), &junk, 1, &written, nullptr));
    handle.Reset();

    EXPECT_EQ(cbk::Status::kFailed, cbk::VerifyArchive(archive, &error));
}

// ============================================================ 取消

TEST(Engine, CancellationStopsAndDeletesHalfWrittenArchive) {
    EnsureRegistered();
    cbk_test::TempDir temp;
    temp.MakeDir(L"src");
    for (int i = 0; i < 50; ++i) {
        temp.MakeFile(L"src\\f" + std::to_wstring(i) + L".txt", std::string(500, 'x'));
    }

    class CancelAfter : public cbk::IProgressObserver {
    public:
        explicit CancelAfter(int n) : limit_(n) {}
        bool IsCancelled() override { return ++checks_ > limit_; }

    private:
        int limit_;
        int checks_ = 0;
    };

    CancelAfter observer(5);
    const std::wstring archive = temp.At(L"o.cbk");
    const cbk::EngineResult result = cbk::RunBackup(BackupTo(temp.At(L"src"), archive), &observer);

    EXPECT_EQ(cbk::Status::kFailed, result.status);
    // 半成品必须删掉：一个残缺的 .cbk 比没有文件更糟，它看着像个能用的备份。
    EXPECT_EQ(INVALID_FILE_ATTRIBUTES, GetFileAttributesW(pf::ToExtendedPath(archive).c_str()))
        << "取消之后半成品还留在盘上";
}

}  // namespace
