// Copyright 2026 CBK Project. 进度事件与取消的契约。
#ifndef CBK_EVENT_H_
#define CBK_EVENT_H_

#include <cstdint>
#include <string>

#include "cbk/types.h"

namespace cbk {

/// 任务开始。
///
/// Scanner 是边遍历边产出的，开始时往往还不知道总量，
/// 所以 total_* 允许为 0，表示"未知"——GUI 收到 0 应该显示不确定进度条，
/// 而不是显示 0%。
struct StartInfo {
    uint64_t total_entries = 0;  ///< 预估条目总数，0 表示未知
    uint64_t total_bytes = 0;    ///< 预估字节总数，0 表示未知
};

/// 进度推进。
struct ProgressInfo {
    uint64_t done_entries = 0;
    uint64_t total_entries = 0;  ///< 0 表示未知
    uint64_t done_bytes = 0;
    uint64_t total_bytes = 0;  ///< 0 表示未知

    /// 当前正在处理的条目。可能为 nullptr（比如正在写索引区，不属于任何条目）。
    /// 指向的对象只在本次回调期间有效，实现方要用的话必须自己拷贝。
    const EntryMeta* current = nullptr;
};

/// 可预期的失败：文件被别的进程占用、没有读取权限、符号链接建不出来。
///
/// 发完这个事件之后流程继续往下跑，最终结果是 Status::kPartial。
/// 不可恢复的失败不走这里，直接抛异常。
struct WarnInfo {
    std::wstring path;       ///< 出问题的相对路径，可能为空（不针对具体条目时）
    std::wstring message;    ///< 人能看懂的说明
    uint32_t win_error = 0;  ///< GetLastError() 的原值，0 表示与 Win32 无关
};

/// 任务结束。
struct ResultInfo {
    Status status = Status::kOk;
    uint64_t entries_done = 0;
    uint64_t entries_skipped = 0;  ///< 发了 warn 并跳过的条目数
    uint64_t bytes_read = 0;
    uint64_t bytes_written = 0;
};

/// 引擎向外汇报进度，并询问是否该停下来。
///
/// ## 为什么是一个接口，而不是四个 std::function
///
/// 引擎只需要拿一个指针，调用点干净。测试里派生一个子类把收到的事件记进
/// vector 就能断言，CLI 派生一个子类把事件打成 JSON。四个 std::function
/// 要么打包成结构体，要么在每层函数签名里传四个参数，都更啰嗦。
///
/// 所有方法都有默认空实现，实现方只覆盖自己关心的那一个即可。
///
/// ## 取消为什么也放在这里
///
/// 需要查"是不是该停了"的地方，恰好就是上报进度的地方——遍历循环里、
/// 大文件的分块读写循环里。拆成两个对象要传两个指针，没有任何好处。
///
/// ## 上报频率：core 不节流，由消费方节流
///
/// 引擎每处理完一个 kIoBlockSize 块、以及每个条目结束时，各调一次
/// OnProgress。这是一次虚函数调用，代价可以忽略，好处是单个大文件也能
/// 让进度条动起来。
///
/// **节流是消费方的责任。** CLI 那一侧每行 JSON 都要 flush，一万个小文件
/// 全打出去会把 stdout 淹掉，GUI 忙着解析反而更卡——所以 CLI 里要做
/// "距上次输出不足 100ms 就不打"的判断。反过来把节流做进 core 是错的：
/// core 无从知道消费方一次输出有多贵。
class IProgressObserver {
public:
    virtual ~IProgressObserver() = default;

    virtual void OnStart(const StartInfo&) {}
    virtual void OnProgress(const ProgressInfo&) {}
    virtual void OnWarn(const WarnInfo&) {}
    virtual void OnResult(const ResultInfo&) {}

    /// 返回 true 时引擎应尽快停止。默认永不取消。
    ///
    /// 引擎收到取消后：删掉写了一半的产物，发一条 WarnInfo 说明是用户主动
    /// 取消（好跟真的出错区分开），返回 Status::kFailed。
    ///
    /// 不给 Status 加一个 kCancelled——那是冻结的契约，加一个值等于改动
    /// GUI 那边的退出码约定。取消产生的包本来就是残缺不可用的，
    /// 归到 kFailed（退出码 2）语义上也说得通。
    virtual bool IsCancelled() { return false; }
};

}  // namespace cbk

#endif  // CBK_EVENT_H_
