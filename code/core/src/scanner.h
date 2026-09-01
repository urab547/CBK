// Copyright 2026 CBK Project. 目录树遍历。
//
// 把一棵目录树摊平成一串 EntryMeta，按发现顺序回调出去。
//
// 两个实现选择，都不是随便定的：
//
//   · 用 FindFirstFileW / FindNextFileW，不用 std::filesystem 的递归迭代器。
//     WIN32_FIND_DATAW 一次就把属性位、三个时间戳、文件大小全带回来了，
//     重解析点标签还直接放在 dwReserved0 里。换成 filesystem 的话，
//     每个文件都得再单独 CreateFile + DeviceIoControl 问一遍——
//     几万个文件的目录树上这是数量级的差别。
//
//   · **显式栈迭代，不用递归。** 深目录会把调用栈撑爆（我们的测试里就有
//     1000 层的用例），而且迭代版本才好在任意一步响应取消。
#ifndef CODE_CORE_SRC_SCANNER_H_
#define CODE_CORE_SRC_SCANNER_H_

#include <cstdint>
#include <functional>
#include <string>

#include "cbk/event.h"
#include "cbk/types.h"
#include "src/reparse.h"

namespace cbk {

struct ScanOptions {
    /// 是否跟随符号链接和 junction 递归进去。
    ///
    /// 默认不跟随。跟随会带来三个麻烦：备份体积暴涨、指向源树之外的链接
    /// 把不该备份的东西拖进来、循环链接直接转不出来。开了这个开关时，
    /// 遍历会用 (卷序列号, 文件索引) 集合断环。
    bool follow_symlinks = false;
};

struct ScanStats {
    uint64_t entries = 0;      ///< 成功产出的条目数
    uint64_t total_bytes = 0;  ///< 所有普通文件的原始字节数之和
    uint64_t skipped = 0;      ///< 打不开、发了 warn 之后跳过的数量
};

/// 遍历器。一个实例对应一次遍历。
///
/// ## 它填哪些字段，不填哪些
///
/// 填：id / relative_path / type / attributes / 三个时间戳 /
///     original_size / reparse_tag
///
///     link_target / link_is_relative（重解析点的目标）
///
/// 不填，留给后面的模块：
///   · hardlink_ref_id —— 硬链接去重在备份引擎里做，那儿本来就要开句柄
///   · sddl —— 属主与 DACL（#9）
///
/// ## 顺序保证
///
/// 父目录一定先于它的子项产出，id 也一定更小。还原时"先建所有目录、
/// 再按 id 升序还原内容"能成立，靠的就是这条。
///
/// 源根自身**不产出条目**。还原的目标是另一个目录（--dest），
/// 把源根的元数据往目标根上套没有意义。所以 relative_path 永远非空。
class Scanner {
public:
    Scanner(std::wstring root, ScanOptions options, IProgressObserver* observer);

    using EntryCallback = std::function<void(const EntryMeta&)>;

    /// 开始遍历。
    ///
    /// @return kOk     全部成功
    ///         kPartial 有条目打不开，已 warn 并跳过，其余照常
    ///         kFailed  根目录就打不开，或者被取消
    Status Scan(const EntryCallback& on_entry, ScanStats* stats);

private:
    /// 遍历栈上的一项：待展开的目录。
    struct PendingDir {
        std::wstring relative;  ///< 相对源根，空串表示根本身
    };

    void Warn(const std::wstring& relative, const std::wstring& message, uint32_t win_error);

    /// 跟随模式下，把一条链接改记成它指向的东西（目录或普通文件）。
    void MaterializeFollowedLink(const std::wstring& absolute, EntryMeta* meta, ScanStats* stats);

    std::wstring root_;
    ScanOptions options_;
    IProgressObserver* observer_;

    uint64_t next_id_ = 0;
    bool cancelled_ = false;
    bool had_warning_ = false;
};

}  // namespace cbk

#endif  // CODE_CORE_SRC_SCANNER_H_
