// Copyright 2026 CBK Project. 事件与取消契约的单元测试。
//
// 这个契约给三个人共用：引擎产出事件，CLI 把它序列化成 JSON，GUI 消费。
// 用例本身很轻，作用是钉死几条约定，免得以后有人顺手改坏：
// 默认实现必须是空操作、默认不取消、总量为 0 表示"未知"而不是"没有"。

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "cbk/event.h"
#include "cbk/types.h"

namespace {

/// 把收到的事件记下来，供断言检查。CLI 和测试都会这么派生一个子类，
/// 这正是把它做成接口而不是四个 std::function 的理由。
class RecordingObserver : public cbk::IProgressObserver {
public:
    void OnStart(const cbk::StartInfo& info) override { starts.push_back(info); }
    void OnProgress(const cbk::ProgressInfo& info) override { progresses.push_back(info); }
    void OnWarn(const cbk::WarnInfo& info) override { warns.push_back(info); }
    void OnResult(const cbk::ResultInfo& info) override { results.push_back(info); }

    std::vector<cbk::StartInfo> starts;
    std::vector<cbk::ProgressInfo> progresses;
    std::vector<cbk::WarnInfo> warns;
    std::vector<cbk::ResultInfo> results;
};

/// 只覆盖取消，其余全走默认实现——这是最常见的用法。
class AlwaysCancelledObserver : public cbk::IProgressObserver {
public:
    bool IsCancelled() override { return true; }
};

TEST(ProgressObserver, BaseImplementationSwallowsEverything) {
    // 不覆盖任何方法也必须能安全地被引擎调用。
    cbk::IProgressObserver observer;
    observer.OnStart(cbk::StartInfo{});
    observer.OnProgress(cbk::ProgressInfo{});
    observer.OnWarn(cbk::WarnInfo{});
    observer.OnResult(cbk::ResultInfo{});
    SUCCEED();
}

TEST(ProgressObserver, DefaultNeverCancels) {
    cbk::IProgressObserver observer;
    EXPECT_FALSE(observer.IsCancelled());
}

TEST(ProgressObserver, SubclassCanOverrideOnlyCancellation) {
    AlwaysCancelledObserver observer;
    EXPECT_TRUE(observer.IsCancelled());
    observer.OnProgress(cbk::ProgressInfo{});  // 没覆盖，走默认空实现
    SUCCEED();
}

TEST(ProgressObserver, RecordsEveryEventKind) {
    RecordingObserver observer;

    cbk::StartInfo start;
    start.total_entries = 3;
    start.total_bytes = 1024;
    observer.OnStart(start);

    cbk::EntryMeta entry;
    entry.id = 7;
    entry.relative_path = L"子目录\\文件.txt";
    cbk::ProgressInfo progress;
    progress.done_entries = 1;
    progress.total_entries = 3;
    progress.current = &entry;
    observer.OnProgress(progress);

    cbk::WarnInfo warn;
    warn.path = L"被占用.log";
    warn.message = L"文件被其它进程占用，已跳过";
    warn.win_error = 32;  // ERROR_SHARING_VIOLATION
    observer.OnWarn(warn);

    cbk::ResultInfo result;
    result.status = cbk::Status::kPartial;
    result.entries_done = 2;
    result.entries_skipped = 1;
    observer.OnResult(result);

    ASSERT_EQ(1u, observer.starts.size());
    EXPECT_EQ(3u, observer.starts[0].total_entries);

    ASSERT_EQ(1u, observer.progresses.size());
    ASSERT_NE(nullptr, observer.progresses[0].current);
    EXPECT_EQ(7u, observer.progresses[0].current->id);

    ASSERT_EQ(1u, observer.warns.size());
    EXPECT_EQ(32u, observer.warns[0].win_error);
    EXPECT_EQ(L"被占用.log", observer.warns[0].path);

    ASSERT_EQ(1u, observer.results.size());
    EXPECT_EQ(cbk::Status::kPartial, observer.results[0].status);
    EXPECT_EQ(1u, observer.results[0].entries_skipped);
}

TEST(ProgressEvents, ZeroTotalsMeanUnknownNotEmpty) {
    // Scanner 是边走边产出的，开始时通常还不知道总量。
    // GUI 收到 0 要显示不确定进度条，不能显示 0% 或者直接判定"没东西可备份"。
    const cbk::StartInfo start;
    EXPECT_EQ(0u, start.total_entries);
    EXPECT_EQ(0u, start.total_bytes);

    const cbk::ProgressInfo progress;
    EXPECT_EQ(0u, progress.total_entries);
    EXPECT_EQ(nullptr, progress.current);
}

TEST(ProgressEvents, ResultDefaultsToSuccess) {
    const cbk::ResultInfo result;
    EXPECT_EQ(cbk::Status::kOk, result.status);
    EXPECT_EQ(0, static_cast<int>(result.status));
}

TEST(WarnEvent, WinErrorZeroMeansNotAWin32Failure) {
    const cbk::WarnInfo warn;
    EXPECT_EQ(0u, warn.win_error);
    EXPECT_TRUE(warn.path.empty());
    EXPECT_TRUE(warn.message.empty());
}

}  // namespace
