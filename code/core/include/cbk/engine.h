// Copyright 2026 CBK Project. 备份与还原的编排。
//
// 这一层把 Scanner、IPacker、Stage 链、Archive 串起来，是唯一知道
// 「整件事按什么顺序做」的地方。CLI 只负责解析参数和把事件打成 JSON。
#ifndef CBK_ENGINE_H_
#define CBK_ENGINE_H_

#include <cstdint>
#include <string>
#include <vector>

#include "cbk/event.h"
#include "cbk/types.h"

namespace cbk {

/// 还原时目标已经存在同名文件怎么办。
enum class OverwritePolicy {
    kSkip,    ///< 跳过并 warn（默认，最安全）
    kForce,   ///< 直接覆盖
    kRename,  ///< 改名成 "name (1).ext" 之类
};

struct BackupOptions {
    std::wstring source_root;
    std::wstring dest_archive;
    std::string packer = "cbk-native";
    std::vector<std::string> stages;  ///< 按应用顺序：压缩在前，加密在后
    std::string password;             ///< 需要密码的 Stage 从这里拿
    bool follow_symlinks = false;
};

struct RestoreOptions {
    std::wstring archive;
    std::wstring dest_root;
    std::string password;
    OverwritePolicy overwrite = OverwritePolicy::kSkip;
    bool restore_metadata = true;  ///< --no-metadata 时为 false
};

struct EngineResult {
    Status status = Status::kOk;
    uint64_t entries_done = 0;
    uint64_t entries_skipped = 0;
    uint64_t bytes_read = 0;
    uint64_t bytes_written = 0;
    std::wstring error;  ///< status 为 kFailed / kBadArgs 时的原因
};

/// 跑一次备份。
///
/// ## 顺序
///
/// 先整棵树遍历完拿到条目表，再开始写内容。看着像多走一遍，其实不是：
/// 索引区在容器尾部，本来就必须把所有 EntryMeta 攒在内存里等到最后写，
/// 所以这份表跑不掉。顺带的好处是 OnStart 能报出真实的总量，
/// 进度条不用一直悬着。
///
/// 文件内容始终是 64 KB 流式读的，任何情况下都不整个读进内存。
///
/// ## data_offset 与 stored_size 的口径
///
/// 记的是**打包器输出流**里的偏移和长度，也就是"过了 Packer、还没过
/// Stage"的位置。不是容器文件里的物理偏移。
///
/// 原因：压缩和加密是对整条流做的，一个条目在压缩后的字节流里根本没有
/// 独立的起止点。硬要填一个"物理偏移"只能填个近似值，而一个看着精确、
/// 实际上不能用来定位的字段比留空更害人。按现在这个口径，没有 Stage 时
/// 它就等于物理偏移，有 Stage 时它仍然是解包后数据流里的准确位置。
EngineResult RunBackup(const BackupOptions& options, IProgressObserver* observer);

/// 跑一次还原。
///
/// ## 为什么分三趟
///
/// 三个顺序陷阱，踩了都不会报错，只会静默出错：
///
///   1. 往目录里写文件会更新目录的 lastWriteTime，所以目录时间戳必须
///      **后序**设置——建目录、填内容、最后才设时间。
///   2. 先设了 FILE_ATTRIBUTE_READONLY，后面再去设时间戳和 ACL 都会失败。
///      所以属性位**最后**设。
///   3. 硬链接必须在目标条目还原之后才能建。按 id 升序还原就自然满足，
///      因为 hardlink_ref_id 一定指向更小的 id。
///
/// 于是：
///
///     Pass 1  建所有目录，不设任何元数据
///     Pass 2  顺序解包，逐条还原内容，每条紧接着设自己的元数据
///     Pass 3  **逆序**遍历目录表，从最深往外设时间戳和属性位
///
/// Pass 3 逆序是因为设置父目录之后不能再去动子目录，否则父目录的
/// 时间戳又被刷新了。
EngineResult RunRestore(const RestoreOptions& options, IProgressObserver* observer);

/// 容器的概况，`cbk list` / `cbk info` 用得到的那些。
///
/// 这是 ArchiveHeader 的对外精简版。头部里那些偏移和长度是容器实现的内部
/// 细节，摆到对外契约上只会让人以为可以拿去随机定位。
struct ArchiveInfo {
    uint16_t format_version = 0;
    std::wstring source_root;
    std::string packer;
    std::vector<std::string> stages;
    uint64_t entry_count = 0;
    uint64_t total_original_bytes = 0;
    uint64_t created_at = 0;  ///< FILETIME
};

/// 读出容器的概况和条目表，供 `cbk list` 用。不碰数据区。
Status ReadArchiveListing(const std::wstring& path, const std::string& password, ArchiveInfo* info,
                          std::vector<EntryMeta>* entries, std::wstring* error);

/// 校验容器完整性，供 `cbk verify` 用。
Status VerifyArchive(const std::wstring& path, std::wstring* error);

}  // namespace cbk

#endif  // CBK_ENGINE_H_
