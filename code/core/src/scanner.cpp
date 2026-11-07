// Copyright 2026 CBK Project. 目录树遍历的实现。
#include "src/scanner.h"

#include <set>
#include <string>
#include <utility>
#include <vector>

#include "src/metadata.h"
#include "src/platform_win.h"
#include "src/reparse.h"

namespace cbk {

namespace {

/// 把重解析点标签格式化成 0xA000000C 这种形式。
/// std::to_wstring 只会给十进制，而重解析点标签按惯例都写成十六进制，
/// 用十进制报出来没人能一眼认出是哪种链接。
std::wstring ToHex(uint32_t value) {
    static const wchar_t kDigits[] = L"0123456789ABCDEF";
    std::wstring text = L"0x";
    for (int shift = 28; shift >= 0; shift -= 4) {
        text.push_back(kDigits[(value >> shift) & 0xFu]);
    }
    return text;
}

/// 把 FILETIME 拼成 uint64。存原值不转 time_t——转成秒会丢掉 100ns 精度，
/// 往返测试立刻就挂。
uint64_t ToUint64(const FILETIME& time) {
    return (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

/// 把高 32 位和低 32 位两个半截拼成一个 64 位整数。
/// 文件大小、文件索引号在 Win32 结构体里都是这样拆成两半存的。
uint64_t ToUint64(DWORD high, DWORD low) {
    return (static_cast<uint64_t>(high) << 32) | low;
}

/// 是不是 "." 或 ".."。FindFirstFileW 在每个目录里都会列出这两项
/// （分别代表当前目录和上级目录），必须跳过——否则会原地打转，
/// 或者顺着 ".." 跑到源目录外面去。
bool IsDotOrDotDot(const wchar_t* name) {
    if (name[0] != L'.') return false;
    if (name[1] == L'\0') return true;
    return name[1] == L'.' && name[2] == L'\0';
}

/// 从 WIN32_FIND_DATAW 判定条目类型。
///
/// 重解析点标签直接取 dwReserved0——这个字段只有在 FILE_ATTRIBUTE_REPARSE_POINT
/// 置位时才有意义，所以必须先判属性位。
FileType ClassifyEntry(const WIN32_FIND_DATAW& find_data, uint32_t* reparse_tag) {
    *reparse_tag = 0;
    const DWORD attributes = find_data.dwFileAttributes;

    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        *reparse_tag = find_data.dwReserved0;
        switch (find_data.dwReserved0) {
            case IO_REPARSE_TAG_SYMLINK:
                // 符号链接必须区分文件和目录两种：还原时 CreateSymbolicLinkW
                // 要靠这个决定加不加 SYMBOLIC_LINK_FLAG_DIRECTORY，判错了
                // 建出来的链接就是坏的。
                return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ? FileType::kSymlinkDir
                                                                    : FileType::kSymlinkFile;
            case IO_REPARSE_TAG_MOUNT_POINT: return FileType::kJunction;
            default:
                // OneDrive 占位符、AppExecLink 之类。记下标签、告警、不递归进去。
                return FileType::kUnsupported;
        }
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return FileType::kDirectory;
    return FileType::kRegular;
}

/// 是不是重解析点类型（含识别不出来的那些）。
bool IsReparse(FileType type) {
    return type == FileType::kSymlinkFile || type == FileType::kSymlinkDir ||
           type == FileType::kJunction || type == FileType::kUnsupported;
}

/// 是不是我们能完整还原的链接类型。
bool IsLink(FileType type) {
    return type == FileType::kSymlinkFile || type == FileType::kSymlinkDir ||
           type == FileType::kJunction;
}

/// 跟随模式下用来断环的身份：同一个目录无论从哪条链接走到，
/// (卷序列号, 文件索引) 都一样。
struct FileIdentity {
    uint32_t volume_serial = 0;  ///< 卷序列号
    uint64_t file_index = 0;     ///< 文件索引号，卷内唯一

    /// std::set 需要排序关系：先比卷，再比索引号。
    bool operator<(const FileIdentity& other) const {
        if (volume_serial != other.volume_serial) return volume_serial < other.volume_serial;
        return file_index < other.file_index;
    }
};

/// 打开路径（会跟随链接）取它的 (卷序列号, 文件索引号)。打不开返回 false。
bool QueryIdentity(const std::wstring& path, FileIdentity* out) {
    platform::ScopedHandle handle = platform::OpenForRead(path, false);
    if (!handle.IsValid()) return false;
    BY_HANDLE_FILE_INFORMATION info = {};
    if (!GetFileInformationByHandle(handle.Get(), &info)) return false;
    out->volume_serial = info.dwVolumeSerialNumber;
    out->file_index = ToUint64(info.nFileIndexHigh, info.nFileIndexLow);
    return true;
}

}  // namespace

/// 跟随模式下，把一条链接改记成它指向的东西（目录或普通文件），
/// 时间戳、属性、大小也都换成目标的。
void Scanner::MaterializeFollowedLink(const std::wstring& absolute, EntryMeta* meta,
                                      ScanStats* stats) {
    // 跟着链接走一次，拿目标的真实身份。打不开（悬空链接）就退回原样，
    // 当成普通链接备份——总比整条丢掉强。
    platform::ScopedHandle handle =
        platform::OpenPath(absolute, FILE_READ_ATTRIBUTES, OPEN_EXISTING, /*no_follow=*/false);
    if (!handle.IsValid()) {
        Warn(meta->relative_path,
             L"链接指向的目标打不开，按链接本身备份：" + platform::FormatWinError(GetLastError()),
             GetLastError());
        return;
    }

    BY_HANDLE_FILE_INFORMATION info = {};
    if (GetFileInformationByHandle(handle.Get(), &info) == 0) return;

    const bool target_is_directory = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    meta->type = target_is_directory ? FileType::kDirectory : FileType::kRegular;
    // 链接本身没有内容也没有属性可言了，现在记的是目标的。
    meta->attributes = info.dwFileAttributes & ~static_cast<DWORD>(FILE_ATTRIBUTE_REPARSE_POINT);
    meta->creation_time = ToUint64(info.ftCreationTime);
    meta->last_access_time = ToUint64(info.ftLastAccessTime);
    meta->last_write_time = ToUint64(info.ftLastWriteTime);
    meta->link_target.clear();
    meta->link_is_relative = false;
    meta->reparse_tag = 0;

    if (!target_is_directory) {
        meta->original_size = ToUint64(info.nFileSizeHigh, info.nFileSizeLow);
        stats->total_bytes += meta->original_size;
    }
}

/// 源根先规范化成绝对路径，之后所有条目的路径都相对它来拼。
Scanner::Scanner(std::wstring root, ScanOptions options, IProgressObserver* observer)
    : root_(platform::NormalizePath(root)), options_(options), observer_(observer) {}

/// 发一条告警，并记下"这次遍历出过问题"，Scan 结束时据此返回 kPartial。
void Scanner::Warn(const std::wstring& relative, const std::wstring& message, uint32_t win_error) {
    had_warning_ = true;
    if (observer_ == nullptr) return;
    WarnInfo warn;
    warn.path = relative;
    warn.message = message;
    warn.win_error = win_error;
    observer_->OnWarn(warn);
}

/// 遍历主体。用显式栈而不是递归，原因见 scanner.h 开头。
Status Scanner::Scan(const EntryCallback& on_entry, ScanStats* stats) {
    // 每次 Scan 都从头开始：计数清零、id 从 0 编起。
    ScanStats local;
    next_id_ = 0;
    cancelled_ = false;
    had_warning_ = false;

    // 显式栈：存"还没展开的目录"。一开始只有源根自己（用空串表示）。
    std::vector<PendingDir> stack;
    stack.push_back(PendingDir{std::wstring()});

    // 只有跟随模式才维护这个集合。不跟随时目录树本来就是棵树，转不出环，
    // 给几万个目录白建一个集合纯属浪费。
    std::set<FileIdentity> visited;
    if (options_.follow_symlinks) {
        // 把源根本身也放进去，挡住"树里有个链接指回根"这种最短的环。
        FileIdentity root_identity;
        if (QueryIdentity(root_, &root_identity)) visited.insert(root_identity);
    }

    bool root_ok = false;

    // 主循环：每轮从栈里弹出一个目录，列出它的全部子项。栈空了就遍历完了。
    while (!stack.empty()) {
        if (observer_ != nullptr && observer_->IsCancelled()) {
            cancelled_ = true;
            break;
        }

        // 从栈尾取出一个待展开的目录。
        const PendingDir current = stack.back();
        stack.pop_back();

        // 拼出「目录路径 + 通配符 *」交给 FindFirstFileW，一次列出这一层的全部子项。
        const std::wstring absolute_dir =
            current.relative.empty() ? root_ : platform::JoinPath(root_, current.relative);
        const std::wstring pattern =
            platform::JoinPath(platform::ToExtendedPath(absolute_dir), L"*");

        WIN32_FIND_DATAW find_data = {};
        HANDLE find = FindFirstFileW(pattern.c_str(), &find_data);
        if (find == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            if (current.relative.empty()) {
                // 根目录都打不开，没什么可继续的。
                Warn(current.relative, L"无法打开源根目录", error);
                if (stats != nullptr) *stats = local;
                return Status::kFailed;
            }
            Warn(current.relative, L"无法列出目录内容，已跳过", error);
            ++local.skipped;
            continue;
        }
        if (current.relative.empty()) root_ok = true;

        // 本层发现的子目录，先攒着，遍历完这一层再压栈。
        std::vector<std::wstring> subdirectories;

        // 逐个处理这一层的子项。第一项 FindFirstFileW 已经取到了，之后靠 FindNextFileW。
        do {
            if (IsDotOrDotDot(find_data.cFileName)) continue;
            if (observer_ != nullptr && observer_->IsCancelled()) {
                cancelled_ = true;
                break;
            }

            // 先用 WIN32_FIND_DATAW 里现成的信息填一条记录：
            // 编号、相对路径、类型、属性、三个时间戳、大小。
            EntryMeta meta;
            meta.id = next_id_++;
            meta.relative_path = current.relative.empty()
                                     ? std::wstring(find_data.cFileName)
                                     : platform::JoinPath(current.relative, find_data.cFileName);
            meta.type = ClassifyEntry(find_data, &meta.reparse_tag);
            meta.attributes = find_data.dwFileAttributes;
            meta.creation_time = ToUint64(find_data.ftCreationTime);
            meta.last_access_time = ToUint64(find_data.ftLastAccessTime);
            meta.last_write_time = ToUint64(find_data.ftLastWriteTime);
            if (meta.type == FileType::kRegular) {
                meta.original_size = ToUint64(find_data.nFileSizeHigh, find_data.nFileSizeLow);
                local.total_bytes += meta.original_size;
            }

            // 目录和重解析点的元数据要从对象本身再读一遍。
            // WIN32_FIND_DATAW 给的是父目录索引项里的缓存，NTFS 更新它是
            // 惰性的：刚往目录里写完文件就遍历，读到的 lastWriteTime 能差
            // 好几毫秒，往返测试就挂在这儿。
            //
            // 普通文件不在这里补——引擎读内容时本来就要开句柄，在那儿顺手
            // 刷一次更划算，也更准（拿到的是读到的那份内容对应的时间）。
            const std::wstring absolute = platform::JoinPath(root_, meta.relative_path);
            if (meta.type != FileType::kRegular) {
                // 读不到就沿用 find data 那份——陈旧的值也比没有值强。
                ReadMetadataByPath(absolute, true, &meta);
            }

            // 重解析点：把它指向哪儿读出来。这一步不能省，否则还原时
            // 只知道"这里有个链接"却不知道它指向谁。
            if (IsReparse(meta.type)) {
                ReparseInfo info;
                uint32_t error = 0;
                if (ReadReparsePoint(absolute, &info, &error)) {
                    meta.link_target = info.target;
                    meta.link_is_relative = info.is_relative;
                    // 以重解析点数据里的判断为准：ClassifyEntry 只看了属性位
                    // 和标签，这里读到的是真正的内容。
                    meta.type = info.type;
                    meta.reparse_tag = info.tag;
                } else {
                    Warn(
                        meta.relative_path,
                        L"读不出重解析点数据，降级为不支持类型：" + platform::FormatWinError(error),
                        error);
                    meta.type = FileType::kUnsupported;
                }
            }

            // 跟随模式下，链接要按它指向的东西来记，而不是记成链接。
            //
            // 不这么做的话会得到一个自相矛盾的包：类型写着"这是个目录链接"，
            // 底下却还挂着一堆子项。还原时先建出链接、再往链接里写子项，
            // 等于把文件写进了链接指向的真实目录——直接污染源数据。
            if (options_.follow_symlinks && IsLink(meta.type)) {
                MaterializeFollowedLink(absolute, &meta, &local);
            }

            // 属主与 DACL。放在最后读，因为它跟类型判定无关，而且是这一圈里
            // 最贵的一次系统调用——前面任何一步把这条判成不用备份，都不用
            // 白花这个开销。
            uint32_t sddl_error = 0;
            if (!ReadSecurityDescriptor(absolute, &meta.sddl, &sddl_error)) {
                meta.sddl.clear();
                Warn(meta.relative_path,
                     L"读不到属主与权限，这一条的 ACL 不会被备份：" +
                         platform::FormatWinError(sddl_error),
                     sddl_error);
            }

            if (meta.type == FileType::kUnsupported) {
                Warn(meta.relative_path,
                     L"无法识别的重解析点（标签 " + ToHex(meta.reparse_tag) + L"），只记录不还原",
                     0);
            }

            // 交给调用方（备份引擎会把它存进条目表）。
            on_entry(meta);
            ++local.entries;

            // 决定要不要递归进去。
            const bool is_plain_dir = meta.type == FileType::kDirectory;
            const bool is_link_dir =
                meta.type == FileType::kSymlinkDir || meta.type == FileType::kJunction;
            if (is_plain_dir || (is_link_dir && options_.follow_symlinks)) {
                subdirectories.push_back(meta.relative_path);
            }
        } while (FindNextFileW(find, &find_data) != 0);

        // FindNextFileW 返回 0 有两种可能：正常列完了（ERROR_NO_MORE_FILES），
        // 或者中途出错。后者要告警，因为这一层可能有条目被漏掉了。
        const DWORD stop_reason = GetLastError();
        FindClose(find);
        if (stop_reason != ERROR_NO_MORE_FILES && stop_reason != ERROR_SUCCESS) {
            Warn(current.relative, L"目录枚举中途失败，可能有条目被漏掉", stop_reason);
            ++local.skipped;
        }
        if (cancelled_) break;

        // 把这一层发现的子目录压栈，后面的轮次再逐个展开。
        for (const std::wstring& child : subdirectories) {
            if (options_.follow_symlinks) {
                // 断环：同一个目录第二次被走到就不再展开。
                FileIdentity identity;
                if (QueryIdentity(platform::JoinPath(root_, child), &identity)) {
                    if (!visited.insert(identity).second) continue;
                }
            }
            stack.push_back(PendingDir{child});
        }
    }

    if (stats != nullptr) *stats = local;

    // 结果：被取消或根目录打不开 → 失败；出过告警 → 部分成功；否则 → 成功。
    if (cancelled_) return Status::kFailed;
    if (!root_ok) return Status::kFailed;
    return had_warning_ ? Status::kPartial : Status::kOk;
}

}  // namespace cbk
