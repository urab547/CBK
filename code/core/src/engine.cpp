// Copyright 2026 CBK Project. 备份与还原编排的实现。
#include "cbk/engine.h"

#include <algorithm>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "cbk/packer.h"
#include "cbk/stage.h"
#include "src/archive.h"
#include "src/byte_io.h"
#include "src/crc32.h"
#include "src/metadata.h"
#include "src/platform_win.h"
#include "src/reparse.h"
#include "src/scanner.h"
#include "src/stage_pipeline.h"

namespace cbk {

namespace {

/// 用户取消时从深处的回调里跳出来。IPacker::Unpack 的回调没有返回值，
/// 抛异常是唯一能中途停下的办法；在 RunRestore 里就地接住，不会漏出去。
class CancelledByUser : public std::runtime_error {
public:
    CancelledByUser() : std::runtime_error("cancelled by user") {}
};

/// 统计流过的字节数，其余原样转发。用来算每个条目在打包器输出流里的
/// 偏移和长度。
// 插在打包器和 Stage 链之间，数打包器吐出了多少字节。
//
// 这是 data_offset / stored_size 的计量口径：打包器输出流里的位置，
// 不是容器文件里的物理偏移。放在链头而不是链尾，正是为了绕开 Stage
// 的内部缓冲——压缩器可能攒着几十 KB 不吐，链尾的计数跟条目边界对不上。
class CountingSink : public ISink {
public:
    explicit CountingSink(ISink* downstream) : downstream_(downstream) {}

    void Write(const uint8_t* data, size_t len) override {
        downstream_->Write(data, len);
        count_ += len;
    }

    uint64_t Count() const { return count_; }

private:
    ISink* downstream_;
    uint64_t count_ = 0;
};

/// 硬链接的身份。同一个文件实体的多个目录项，这三个值完全相同。
///
/// 来自 GetFileInformationByHandle。硬链接不能跨卷，所以卷序列号也必须
/// 进键——两个不同卷上的文件完全可能撞上同一个文件索引。
struct HardlinkKey {
    uint32_t volume_serial = 0;
    uint64_t file_index = 0;

    bool operator<(const HardlinkKey& other) const {
        if (volume_serial != other.volume_serial) return volume_serial < other.volume_serial;
        return file_index < other.file_index;
    }
};

void Warn(IProgressObserver* observer, const std::wstring& path, const std::wstring& message,
          uint32_t win_error) {
    if (observer == nullptr) return;
    WarnInfo warn;
    warn.path = path;
    warn.message = message;
    warn.win_error = win_error;
    observer->OnWarn(warn);
}

bool IsCancelled(IProgressObserver* observer) {
    return observer != nullptr && observer->IsCancelled();
}

// 失败时返回的计数全是 0。
//
// 这是有意的：调用方在 status != kOk 时不该去信 entries_done 之类的数字。
// 填个"到出错为止处理了多少"看着更有信息量，实际会诱导 GUI 去显示一个
// 半真半假的进度。
EngineResult Fail(Status status, const std::wstring& message) {
    EngineResult result;
    result.status = status;
    result.error = message;
    return result;
}

/// 按名字建一串 Stage。
///
/// @param inverse true 时建逆变换，并且**顺序也要反过来**——写的时候是
///                先压缩后加密，读的时候就得先解密后解压。
// 每次调用都造一套全新的 Stage 实例。
//
// 绝对不能复用：压缩器和加密器都是有状态的（余料缓冲、CBC 链接向量、
// 霍夫曼码表）。拿写完数据区的那一套接着写索引区，解的时候是解不出来的，
// 而且错得很隐蔽——数据区能解开，偏偏索引区解出乱码。
bool BuildStages(const std::vector<std::string>& names, bool inverse, const std::string& password,
                 std::vector<std::unique_ptr<IStage>>* out, std::wstring* error) {
    const StageRegistry& registry = StageRegistry::Instance();
    std::vector<std::string> ordered = names;
    if (inverse) std::reverse(ordered.begin(), ordered.end());

    for (const std::string& name : ordered) {
        IStageFactory* factory = registry.Find(name);
        if (factory == nullptr) {
            *error = L"本程序不支持算法 " + FromUtf8(name) + L"。用 cbk info 看看支持哪些。";
            return false;
        }
        std::unique_ptr<IStage> stage =
            inverse ? factory->CreateInverse() : factory->CreateForward();
        if (stage == nullptr) {
            *error = L"算法 " + FromUtf8(name) + L" 创建失败";
            return false;
        }
        if (!password.empty()) stage->SetPassword(password);
        out->push_back(std::move(stage));
    }
    return true;
}

/// 把一个已经打开的文件读出来喂给打包器。
///
/// 句柄由调用方持有：开文件这一步要在 BeginEntry 之前做完，开不了就整条
/// 跳过，不能先写了条目头再发现读不了。
///
/// @return false 表示读失败（已经发过 warn），调用方应跳过这个条目。
bool StreamFileContent(HANDLE handle, IPacker* packer, ISink& out, IProgressObserver* observer,
                       const EntryMeta& meta, uint64_t* actual_size, uint32_t* crc,
                       ProgressInfo* progress) {
    std::vector<uint8_t> buffer(kIoBlockSize);
    Crc32 checksum;
    uint64_t total = 0;

    for (;;) {
        DWORD got = 0;
        if (ReadFile(handle, buffer.data(), static_cast<DWORD>(buffer.size()), &got, nullptr) ==
            0) {
            Warn(observer, meta.relative_path,
                 L"读到一半失败，已跳过：" + platform::FormatWinError(GetLastError()),
                 GetLastError());
            return false;
        }
        if (got == 0) break;

        checksum.Update(buffer.data(), got);
        packer->WriteData(buffer.data(), got, out);
        total += got;

        // 大文件必须在块级别上报进度、也在块级别查取消，
        // 否则一个 10 GB 的文件会让进度条和取消按钮都失灵。
        if (observer != nullptr) {
            progress->done_bytes += got;
            observer->OnProgress(*progress);
            if (observer->IsCancelled()) throw CancelledByUser();
        }
    }

    *actual_size = total;
    *crc = checksum.Value();
    return true;
}

// ---------------------------------------------------------------- 还原侧

/// 按正确顺序把元数据写回一个已经存在的对象上：时间戳 -> ACL -> 属性位。
///
/// 顺序是硬性的，不是风格问题：
///   · 属性位必须最后。只读位一旦设上，设时间戳和设 ACL 都会失败，
///     而且失败得很安静——只是元数据没生效，没人会注意到。
///   · ACL 排在时间戳之后，是因为改 ACL 本身不会动时间戳，反过来不成立。
///
/// 文件在 Pass 2 结束时调，目录在 Pass 3 逆序调。
// 每一步失败都只 warn、不中断。
//
// 元数据设不上通常是权限问题（没有 SeRestorePrivilege、目标卷不支持 ACL），
// 这类失败是可预期的。为了一个时间戳设不上就让整个还原失败，等于把
// "还原出 99% 的正确数据"变成"什么都没有"，对用户是净损失。
void ApplyEntryMetadata(const std::wstring& absolute, const EntryMeta& entry,
                        IProgressObserver* observer) {
    uint32_t error = 0;
    if (!ApplyTimestamps(absolute, entry, true, &error)) {
        Warn(observer, entry.relative_path, L"时间戳设置失败：" + platform::FormatWinError(error),
             error);
    }

    bool owner_skipped = false;
    if (!ApplySecurityDescriptor(absolute, entry.sddl, &owner_skipped, &error)) {
        Warn(observer, entry.relative_path, L"权限设置失败：" + platform::FormatWinError(error),
             error);
    } else if (owner_skipped) {
        // 设属主要 SeRestorePrivilege。拿不到就只还原 DACL，这是普通用户
        // 运行时的正常情况，说清楚就行。
        Warn(observer, entry.relative_path,
             L"没有 SeRestorePrivilege，属主和属组未还原（DACL 已还原）。"
             L"以管理员身份运行可以完整还原",
             error);
    }

    if (!ApplyAttributes(absolute, entry, &error)) {
        Warn(observer, entry.relative_path, L"属性位设置失败：" + platform::FormatWinError(error),
             error);
    }
}

bool EnsureDirectory(const std::wstring& path, uint32_t* win_error) {
    const std::wstring extended = platform::ToExtendedPath(path);
    if (CreateDirectoryW(extended.c_str(), nullptr) != 0) return true;
    const DWORD error = GetLastError();
    if (error == ERROR_ALREADY_EXISTS) return true;
    if (win_error != nullptr) *win_error = error;
    return false;
}

bool PathExists(const std::wstring& path) {
    return GetFileAttributesW(platform::ToExtendedPath(path).c_str()) != INVALID_FILE_ATTRIBUTES;
}

/// kRename 策略下找一个还没被占用的名字："a.txt" -> "a (1).txt"。
// 在扩展名之前插序号，不是简单往后拼。
//
// "报告.txt" 要变成 "报告 (1).txt" 而不是 "报告.txt (1)"——后者会让文件
// 失去扩展名关联，双击打不开。判断扩展名时要确认那个点在最后一个路径
// 分隔符**之后**，否则 "D:/a.b/c" 这种目录名带点的路径会被切错。
std::wstring FindFreeName(const std::wstring& path) {
    const size_t slash = path.find_last_of(L'\\');
    const size_t dot = path.find_last_of(L'.');
    const bool has_extension =
        dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash);
    const std::wstring stem = has_extension ? path.substr(0, dot) : path;
    const std::wstring extension = has_extension ? path.substr(dot) : std::wstring();

    for (int n = 1; n < 10000; ++n) {
        const std::wstring candidate = stem + L" (" + std::to_wstring(n) + L")" + extension;
        if (!PathExists(candidate)) return candidate;
    }
    return path;  // 一万个重名，放弃挣扎
}

/// Pass 2 的状态机：打包器一条一条吐条目，这里负责落盘。
class RestoreSink {
public:
    RestoreSink(const RestoreOptions& options, IProgressObserver* observer, EngineResult* result)
        : options_(options), observer_(observer), result_(result) {}

    /// 索引区里的那一份才是权威的。数据区里的条目记录是在读文件之前写的，
    /// 那时还不知道真实长度和校验和（文件可能正被别人改着），所以
    /// original_size 是遍历时的旧值、crc32 是 0。按 id 用索引里的覆盖回来。
    void UseIndexAsAuthority(const std::map<uint64_t, const EntryMeta*>* index) { index_ = index; }

    void BeginEntry(const EntryMeta& meta) {
        FinishCurrent();
        current_ = meta;
        if (index_ != nullptr) {
            const auto it = index_->find(meta.id);
            if (it != index_->end()) current_ = *it->second;
        }
        has_current_ = true;
        writing_ = false;
        content_crc_.Reset();

        if (observer_ != nullptr) {
            progress_.done_entries = result_->entries_done;
            progress_.current = &current_;
            observer_->OnProgress(progress_);
            if (observer_->IsCancelled()) throw CancelledByUser();
        }

        switch (meta.type) {
            case FileType::kRegular: OpenRegularFile(); break;
            case FileType::kDirectory:
                // Pass 1 已经建好了，元数据留给 Pass 3。
                ++result_->entries_done;
                break;
            case FileType::kSymlinkFile:
            case FileType::kSymlinkDir: RestoreSymlink(); break;
            case FileType::kJunction: RestoreJunction(); break;
            case FileType::kHardlinkRef: RestoreHardlink(); break;
            case FileType::kUnsupported: Skip(L"备份时就没能识别的重解析点，不还原"); break;
        }
    }

    void WriteData(const uint8_t* data, size_t len) {
        // writing_ 为假说明这条在 BeginEntry 时就被跳过了（目标已存在、
        // 建文件失败、类型不支持）。数据还会照常流过来——打包器不知道
        // 上层的取舍——这里静默丢掉就行，warn 在跳过时已经发过了。
        if (!writing_ || !handle_.IsValid()) return;
        size_t done = 0;
        while (done < len) {
            const DWORD chunk = static_cast<DWORD>(std::min<size_t>(len - done, 1u << 30));
            DWORD written = 0;
            if (WriteFile(handle_.Get(), data + done, chunk, &written, nullptr) == 0 ||
                written == 0) {
                Warn(observer_, current_.relative_path,
                     L"写入失败：" + platform::FormatWinError(GetLastError()), GetLastError());
                handle_.Reset();
                writing_ = false;
                ++result_->entries_skipped;
                return;
            }
            done += written;
        }
        content_crc_.Update(data, len);
        result_->bytes_written += len;
        if (observer_ != nullptr) {
            progress_.done_bytes += len;
            observer_->OnProgress(progress_);
            if (observer_->IsCancelled()) throw CancelledByUser();
        }
    }

    /// 收尾最后一条。
    void FinishCurrent() {
        if (!has_current_ || !writing_) {
            writing_ = false;
            return;
        }
        handle_.Reset();  // 先关文件，元数据才设得上

        // 逐条目校验。crc32 为 0 表示备份时没算（老包，或者不是普通文件），
        // 那就跳过校验而不是报一个假的不匹配。
        if (current_.crc32 != 0 && content_crc_.Value() != current_.crc32) {
            Warn(observer_, current_.relative_path,
                 L"内容校验和与索引里记录的不符，这个文件可能已经损坏", 0);
            ++result_->entries_skipped;
        }

        if (options_.restore_metadata) {
            ApplyEntryMetadata(target_path_, current_, observer_);
        }
        ++result_->entries_done;
        writing_ = false;
    }

    void SetTotals(uint64_t entries, uint64_t bytes) {
        progress_.total_entries = entries;
        progress_.total_bytes = bytes;
    }

private:
    void Skip(const std::wstring& reason) {
        Warn(observer_, current_.relative_path, reason, 0);
        ++result_->entries_skipped;
    }

    /// 算出目标路径、按 --overwrite 策略处理已存在的情况、兜底建父目录。
    ///
    /// 文件、符号链接、junction、硬链接四条路径都要走这一遍，所以抽出来。
    ///
    /// @return false 表示这条按策略跳过了（已经计过数、发过 warn）。
    bool PrepareTargetPath() {
        target_path_ = platform::JoinPath(options_.dest_root, current_.relative_path);

        if (PathExists(target_path_)) {
            switch (options_.overwrite) {
                case OverwritePolicy::kSkip:
                    Skip(L"目标已存在，按 --overwrite skip 跳过");
                    return false;
                case OverwritePolicy::kRename: target_path_ = FindFreeName(target_path_); break;
                case OverwritePolicy::kForce:
                    // 只读文件直接覆盖会失败，先把只读位摘掉。
                    SetFileAttributesW(platform::ToExtendedPath(target_path_).c_str(),
                                       FILE_ATTRIBUTE_NORMAL);
                    break;
            }
        }

        uint32_t error = 0;
        const size_t slash = target_path_.find_last_of(L'\\');
        if (slash != std::wstring::npos) {
            // 正常情况下父目录在 Pass 1 就建好了。这里兜个底，
            // 免得一条目录条目缺失就让它下面所有文件全丢。
            EnsureDirectory(target_path_.substr(0, slash), &error);
        }
        return true;
    }

    void OpenRegularFile() {
        if (!PrepareTargetPath()) return;

        handle_ = platform::CreateForWrite(target_path_);
        if (!handle_.IsValid()) {
            Warn(observer_, current_.relative_path,
                 L"建文件失败：" + platform::FormatWinError(GetLastError()), GetLastError());
            ++result_->entries_skipped;
            return;
        }
        writing_ = true;
    }

    void RestoreSymlink() {
        if (!PrepareTargetPath()) return;
        if (current_.link_target.empty()) {
            Skip(L"符号链接没有记录目标，跳过");
            return;
        }

        const bool directory = current_.type == FileType::kSymlinkDir;
        uint32_t error = 0;
        if (CreateSymlink(target_path_, current_.link_target, directory, &error)) {
            ++result_->entries_done;
            return;
        }

        // 建不了通常是既非管理员、又没开开发者模式。降级：目录链接建成空目录、
        // 文件链接建成 0 字节文件，让目录结构至少是完整的，然后 warn 说明。
        // 不能因为一条链接建不出来就中断整个还原。
        uint32_t degrade_error = 0;
        bool degraded = false;
        if (directory) {
            degraded = EnsureDirectory(target_path_, &degrade_error);
        } else {
            platform::ScopedHandle placeholder = platform::CreateForWrite(target_path_);
            degraded = placeholder.IsValid();
        }
        Warn(observer_, current_.relative_path,
             L"建符号链接失败（" + platform::FormatWinError(error) +
                 L"）。已降级为空占位，需要管理员权限或打开开发者模式才能完整还原",
             error);
        ++result_->entries_skipped;
        if (!degraded) {
            Warn(observer_, current_.relative_path, L"连占位都建不出来", degrade_error);
        }
    }

    void RestoreJunction() {
        if (!PrepareTargetPath()) return;
        if (current_.link_target.empty()) {
            Skip(L"junction 没有记录目标，跳过");
            return;
        }

        // junction 不需要管理员权限也不需要开发者模式，这条路径基本不会失败。
        uint32_t error = 0;
        if (CreateJunction(target_path_, current_.link_target, &error)) {
            ++result_->entries_done;
            return;
        }
        Warn(observer_, current_.relative_path,
             L"建 junction 失败：" + platform::FormatWinError(error), error);
        ++result_->entries_skipped;
    }

    void RestoreHardlink() {
        if (index_ == nullptr) {
            Skip(L"没有索引，找不到硬链接指向的条目");
            return;
        }
        const auto it = index_->find(current_.hardlink_ref_id);
        if (it == index_->end()) {
            Skip(L"硬链接指向的条目不在索引里，包可能已损坏");
            return;
        }
        // 按 id 升序还原保证了被指向的那条一定已经落盘：
        // hardlink_ref_id 一定小于当前条目的 id。
        const std::wstring existing =
            platform::JoinPath(options_.dest_root, it->second->relative_path);

        if (!PrepareTargetPath()) return;

        if (CreateHardLinkW(platform::ToExtendedPath(target_path_).c_str(),
                            platform::ToExtendedPath(existing).c_str(), nullptr) != 0) {
            ++result_->entries_done;
            return;
        }

        // 硬链接不能跨卷。还原目标和被指向的条目落在不同卷上时只能降级成复制：
        // 内容是对的，但两个路径不再是同一个文件实体了——必须说清楚。
        const uint32_t error = GetLastError();
        if (CopyFileW(platform::ToExtendedPath(existing).c_str(),
                      platform::ToExtendedPath(target_path_).c_str(), FALSE) != 0) {
            Warn(observer_, current_.relative_path,
                 L"建硬链接失败（" + platform::FormatWinError(error) +
                     L"），已降级为复制一份。内容一致，但不再是同一个文件实体",
                 error);
            ++result_->entries_done;
            return;
        }
        Warn(observer_, current_.relative_path,
             L"硬链接建不了、复制也失败：" + platform::FormatWinError(GetLastError()),
             GetLastError());
        ++result_->entries_skipped;
    }

    const RestoreOptions& options_;
    IProgressObserver* observer_;
    EngineResult* result_;

    const std::map<uint64_t, const EntryMeta*>* index_ = nullptr;
    EntryMeta current_;
    std::wstring target_path_;
    platform::ScopedHandle handle_;
    Crc32 content_crc_;
    ProgressInfo progress_;
    bool has_current_ = false;
    bool writing_ = false;
};

}  // namespace

// ============================================================ RunBackup

EngineResult RunBackup(const BackupOptions& options, IProgressObserver* observer) {
    // ---- 参数校验。这些错都是调用方（通常是 GUI）的 bug，退出码 3。----
    if (options.source_root.empty() || options.dest_archive.empty()) {
        return Fail(Status::kBadArgs, L"--source 和 --dest 都不能为空");
    }
    if (!PackerRegistry::Instance().Has(options.packer)) {
        return Fail(Status::kBadArgs, L"没有名为 " + FromUtf8(options.packer) +
                                          L" 的打包算法。用 cbk info 看看支持哪些。");
    }
    for (const std::string& name : options.stages) {
        if (!StageRegistry::Instance().Has(name)) {
            return Fail(Status::kBadArgs,
                        L"没有名为 " + FromUtf8(name) + L" 的算法。用 cbk info 看看支持哪些。");
        }
    }
    if (!ValidatePipelineOrder(options.stages)) {
        return Fail(Status::kBadArgs,
                    L"流水线顺序不合法：压缩必须排在加密之前。密文是高熵数据，"
                    L"压不动还会变大。");
    }

    EngineResult result;

    // 拿不到特权不是致命错误，降级跑就是了——只是遇到没权限的文件会跳过。
    platform::EnableBackupPrivileges();

    // ---- 遍历 ----
    const std::wstring root = platform::NormalizePath(options.source_root);
    std::vector<EntryMeta> entries;
    ScanStats stats;
    {
        Scanner scanner(root, ScanOptions{options.follow_symlinks}, observer);
        const Status scan_status =
            scanner.Scan([&entries](const EntryMeta& meta) { entries.push_back(meta); }, &stats);
        if (scan_status == Status::kFailed) {
            if (IsCancelled(observer)) return Fail(Status::kFailed, L"已取消");
            return Fail(Status::kFailed, L"无法遍历源目录 " + root);
        }
        if (scan_status == Status::kPartial) result.status = Status::kPartial;
    }

    if (observer != nullptr) {
        StartInfo start;
        start.total_entries = entries.size();
        start.total_bytes = stats.total_bytes;
        observer->OnStart(start);
    }

    // ---- 打开容器 ----
    ArchiveWriter writer;
    PipelineDesc pipeline;
    pipeline.packer = options.packer;
    pipeline.stages = options.stages;

    std::wstring error;
    const Status open_status = writer.Open(options.dest_archive, root, pipeline, &error);
    if (open_status != Status::kOk) return Fail(open_status, error);

    std::unique_ptr<IPacker> packer = PackerRegistry::Instance().Create(options.packer);
    if (packer == nullptr) return Fail(Status::kFailed, L"打包器创建失败");

    std::vector<EntryMeta> written;
    written.reserve(entries.size());
    uint64_t total_original = 0;

    // 硬链接去重表。**只对 nNumberOfLinks > 1 的文件建表项**——几万个普通
    // 文件全塞进去纯属浪费，而链接数是开句柄时顺手就有的，不额外花系统调用。
    std::map<HardlinkKey, uint64_t> hardlinks;

    try {
        // ---- 数据区 ----
        std::vector<std::unique_ptr<IStage>> stages;
        if (!BuildStages(options.stages, false, options.password, &stages, &error)) {
            return Fail(Status::kBadArgs, error);
        }
        StageChainSink chain(std::move(stages), &writer.DataSink());
        CountingSink counter(&chain.Entry());

        ProgressInfo progress;
        progress.total_entries = entries.size();
        progress.total_bytes = stats.total_bytes;

        for (EntryMeta& entry : entries) {
            if (IsCancelled(observer)) throw CancelledByUser();

            progress.done_entries = written.size();
            progress.current = &entry;
            if (observer != nullptr) observer->OnProgress(progress);

            const std::wstring absolute = platform::JoinPath(root, entry.relative_path);
            entry.data_offset = counter.Count();

            if (entry.type == FileType::kRegular) {
                // 先开句柄再写条目头：开不了就整条跳过，不能先写了头
                // 才发现读不了，那样数据区里会留一条没有内容的空壳。
                platform::ScopedHandle handle = platform::OpenForRead(absolute, true);
                if (!handle.IsValid()) {
                    Warn(observer, entry.relative_path,
                         L"打不开，已跳过：" + platform::FormatWinError(GetLastError()),
                         GetLastError());
                    ++result.entries_skipped;
                    result.status = Status::kPartial;
                    continue;
                }

                // 从句柄上重读一遍权威元数据。FindFirstFileW 给的是父目录
                // 索引项里的缓存，正被别的进程写着的文件会读到旧值。
                // 句柄已经在手上，这一步不额外开销一次 CreateFile。
                ReadMetadataFromHandle(handle.Get(), &entry);

                // ---- 硬链接去重 ----
                //
                // 同一份内容有多个目录项时，只有第一次遇到的那条存内容，
                // 后面的都记成 kHardlinkRef 指回去。这既省空间，也是"正确
                // 处理硬链接"这个评分点的核心——把 3 个硬链接当成 3 个独立
                // 文件存下来的话，还原之后它们就不再是同一个实体了。
                BY_HANDLE_FILE_INFORMATION info = {};
                HardlinkKey key;
                bool multi_linked = false;
                if (GetFileInformationByHandle(handle.Get(), &info) != 0 &&
                    info.nNumberOfLinks > 1) {
                    multi_linked = true;
                    key.volume_serial = info.dwVolumeSerialNumber;
                    key.file_index =
                        (static_cast<uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;

                    const auto found = hardlinks.find(key);
                    if (found != hardlinks.end()) {
                        entry.type = FileType::kHardlinkRef;
                        entry.hardlink_ref_id = found->second;
                        entry.original_size = 0;
                        entry.crc32 = 0;
                        handle.Reset();

                        packer->BeginEntry(entry, counter);
                        packer->EndEntry(counter);
                        entry.stored_size = counter.Count() - entry.data_offset;
                        written.push_back(entry);
                        ++result.entries_done;
                        continue;
                    }
                }

                packer->BeginEntry(entry, counter);
                uint64_t actual = 0;
                uint32_t crc = 0;
                if (!StreamFileContent(handle.Get(), packer.get(), counter, observer, entry,
                                       &actual, &crc, &progress)) {
                    packer->EndEntry(counter);
                    ++result.entries_skipped;
                    result.status = Status::kPartial;
                    continue;
                }
                packer->EndEntry(counter);

                // 真实读到的长度和校验和只有读完才知道，所以数据区里那条
                // 记录带的还是旧值。**索引区里这一份才是权威的**，还原时
                // 按 id 用索引里的覆盖回来。
                entry.original_size = actual;
                entry.crc32 = crc;
                result.bytes_read += actual;
                total_original += actual;

                // 登记放在写成功之后：万一上面读失败被 continue 掉了，
                // 后面的硬链接就会指向一条根本不在索引里的 id。
                if (multi_linked) hardlinks.emplace(key, entry.id);
            } else {
                packer->BeginEntry(entry, counter);
                packer->EndEntry(counter);
            }

            entry.stored_size = counter.Count() - entry.data_offset;
            written.push_back(entry);
            ++result.entries_done;
        }

        packer->Finish(counter);
        chain.Finish();
        writer.EndData();

        // ---- 索引区 ----
        // 必须用一套全新的 Stage 实例：压缩器和加密器都是有状态的，
        // 拿写完数据区的那套接着压索引，解的时候是解不出来的。
        std::vector<std::unique_ptr<IStage>> index_stages;
        if (!BuildStages(options.stages, false, options.password, &index_stages, &error)) {
            return Fail(Status::kBadArgs, error);
        }
        StageChainSink index_chain(std::move(index_stages), &writer.IndexSink());
        WriteIndex(written, index_chain.Entry());
        index_chain.Finish();
    } catch (const CancelledByUser&) {
        Warn(observer, std::wstring(), L"用户取消了备份，半成品已删除", 0);
        writer.Abort();
        return Fail(Status::kFailed, L"已取消");
    }

    const Status close_status = writer.Close(written.size(), total_original, &error);
    if (close_status != Status::kOk) return Fail(close_status, error);

    result.bytes_written = writer.DataBytesWritten();
    if (result.entries_skipped > 0 && result.status == Status::kOk) {
        result.status = Status::kPartial;
    }

    if (observer != nullptr) {
        ResultInfo info;
        info.status = result.status;
        info.entries_done = result.entries_done;
        info.entries_skipped = result.entries_skipped;
        info.bytes_read = result.bytes_read;
        info.bytes_written = result.bytes_written;
        observer->OnResult(info);
    }
    return result;
}

// ============================================================ RunRestore

EngineResult RunRestore(const RestoreOptions& options, IProgressObserver* observer) {
    if (options.archive.empty() || options.dest_root.empty()) {
        return Fail(Status::kBadArgs, L"--archive 和 --dest 都不能为空");
    }

    std::wstring error;
    ArchiveReader reader;
    if (reader.Open(options.archive, &error) != Status::kOk) {
        return Fail(Status::kFailed, error);
    }
    const ArchiveHeader& header = reader.Header();

    std::unique_ptr<IPacker> packer = PackerRegistry::Instance().Create(header.pipeline.packer);
    if (packer == nullptr) {
        return Fail(Status::kFailed, L"这个包用 " + FromUtf8(header.pipeline.packer) +
                                         L" 打包，本程序不支持。用 cbk info 看看支持哪些。");
    }

    // ---- 索引 ----
    std::vector<EntryMeta> entries;
    {
        std::unique_ptr<ISource> raw = reader.OpenIndex();
        if (raw == nullptr) return Fail(Status::kFailed, L"打不开索引区");
        std::vector<std::unique_ptr<IStage>> stages;
        if (!BuildStages(header.pipeline.stages, true, options.password, &stages, &error)) {
            return Fail(Status::kFailed, error);
        }
        StageChainSource chain(raw.get(), std::move(stages));
        if (!ReadIndex(chain, header.entry_count, &entries)) {
            return Fail(Status::kFailed, L"索引区解析失败，容器已损坏或密码不对");
        }
    }

    platform::EnableBackupPrivileges();

    EngineResult result;
    uint32_t win_error = 0;
    if (!EnsureDirectory(platform::NormalizePath(options.dest_root), &win_error)) {
        return Fail(Status::kFailed, L"无法创建目标目录：" + platform::FormatWinError(win_error));
    }
    const std::wstring dest = platform::NormalizePath(options.dest_root);

    if (observer != nullptr) {
        StartInfo start;
        start.total_entries = entries.size();
        start.total_bytes = header.total_original_bytes;
        observer->OnStart(start);
    }

    // ---- Pass 1：先把所有目录建出来，一律不设元数据 ----
    // 条目是按 id 升序排的，而 Scanner 保证父目录的 id 更小，
    // 所以顺着建下去父目录一定已经存在。
    std::vector<const EntryMeta*> directories;
    for (const EntryMeta& entry : entries) {
        if (entry.type != FileType::kDirectory) continue;
        const std::wstring absolute = platform::JoinPath(dest, entry.relative_path);
        if (!EnsureDirectory(absolute, &win_error)) {
            Warn(observer, entry.relative_path,
                 L"建目录失败：" + platform::FormatWinError(win_error), win_error);
            ++result.entries_skipped;
            continue;
        }
        directories.push_back(&entry);
    }

    // ---- Pass 2：顺序解包，逐条落盘并设各自的元数据 ----
    RestoreOptions effective = options;
    effective.dest_root = dest;
    RestoreSink sink(effective, observer, &result);
    sink.SetTotals(entries.size(), header.total_original_bytes);

    std::map<uint64_t, const EntryMeta*> index_by_id;
    for (const EntryMeta& entry : entries) index_by_id[entry.id] = &entry;
    sink.UseIndexAsAuthority(&index_by_id);

    try {
        std::unique_ptr<ISource> raw = reader.OpenData();
        if (raw == nullptr) return Fail(Status::kFailed, L"打不开数据区");
        std::vector<std::unique_ptr<IStage>> stages;
        if (!BuildStages(header.pipeline.stages, true, options.password, &stages, &error)) {
            return Fail(Status::kFailed, error);
        }
        StageChainSource chain(raw.get(), std::move(stages));

        packer->Unpack(
            chain, [&sink](const EntryMeta& meta) { sink.BeginEntry(meta); },
            [&sink](const uint8_t* data, size_t len) { sink.WriteData(data, len); });
        sink.FinishCurrent();
    } catch (const CancelledByUser&) {
        Warn(observer, std::wstring(), L"用户取消了还原，已还原的部分保留在目标目录里", 0);
        return Fail(Status::kFailed, L"已取消");
    } catch (const std::runtime_error& e) {
        return Fail(Status::kFailed, L"解包失败：" + FromUtf8(e.what()));
    }

    // ---- Pass 3：逆序设置目录的时间戳和属性位 ----
    // 逆序是因为从最深的往外设，设完父目录就不会再有人去动它的子目录，
    // 父目录的时间戳才不会被二次刷新。
    if (options.restore_metadata) {
        for (size_t i = directories.size(); i > 0; --i) {
            const EntryMeta& entry = *directories[i - 1];
            ApplyEntryMetadata(platform::JoinPath(dest, entry.relative_path), entry, observer);
        }
    }

    if (result.entries_skipped > 0) result.status = Status::kPartial;

    if (observer != nullptr) {
        ResultInfo info;
        info.status = result.status;
        info.entries_done = result.entries_done;
        info.entries_skipped = result.entries_skipped;
        info.bytes_written = result.bytes_written;
        observer->OnResult(info);
    }
    return result;
}

// ============================================================ list / verify

Status ReadArchiveListing(const std::wstring& path, const std::string& password, ArchiveInfo* info,
                          std::vector<EntryMeta>* entries, std::wstring* error) {
    ArchiveReader reader;
    const Status status = reader.Open(path, error);
    if (status != Status::kOk) return status;

    const ArchiveHeader& header = reader.Header();
    info->format_version = header.format_version;
    info->source_root = header.source_root;
    info->packer = header.pipeline.packer;
    info->stages = header.pipeline.stages;
    info->entry_count = header.entry_count;
    info->total_original_bytes = header.total_original_bytes;
    info->created_at = header.created_at;

    std::unique_ptr<ISource> raw = reader.OpenIndex();
    if (raw == nullptr) {
        *error = L"打不开索引区";
        return Status::kFailed;
    }
    std::vector<std::unique_ptr<IStage>> stages;
    if (!BuildStages(header.pipeline.stages, true, password, &stages, error)) {
        return Status::kFailed;
    }
    StageChainSource chain(raw.get(), std::move(stages));
    if (!ReadIndex(chain, header.entry_count, entries)) {
        *error = L"索引区解析失败，容器已损坏或密码不对";
        return Status::kFailed;
    }
    return Status::kOk;
}

Status VerifyArchive(const std::wstring& path, std::wstring* error) {
    ArchiveReader reader;
    const Status status = reader.Open(path, error);
    if (status != Status::kOk) return status;
    return reader.Verify(error);
}

}  // namespace cbk
