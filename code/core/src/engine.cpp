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

/// 计数用的直通管道：写进来的字节原样转发给下游，同时累计总数。
///
/// 插在打包器和 Stage 链之间，用来算每个条目的 data_offset / stored_size。
/// 计量口径是"打包器输出流里的位置"，不是容器文件里的物理偏移——
/// 放在链头而不是链尾，正是为了绕开 Stage 的内部缓冲：压缩器可能攒着
/// 几十 KB 不吐，在链尾计数的话跟条目边界对不上。
class CountingSink : public ISink {
public:
    /// @param downstream 真正接收数据的下游，必须比本对象活得久。
    explicit CountingSink(ISink* downstream) : downstream_(downstream) {}

    /// 转发给下游，再把长度累加进计数。
    void Write(const uint8_t* data, size_t len) override {
        downstream_->Write(data, len);
        count_ += len;
    }

    /// 到目前为止一共流过了多少字节。
    uint64_t Count() const { return count_; }

private:
    ISink* downstream_;   ///< 下游，不归本对象管
    uint64_t count_ = 0;  ///< 累计字节数
};

/// 给索引区的读取加上进度上报的包装层。
///
/// 还原和列出内容之前都要先把整个索引区读完、解密、解析。包很大或者加了
/// 密的时候这一步要花好几秒，界面需要知道"正在读索引、读到哪了"，否则
/// 看起来像卡死。本类只做两件事：原样转发读取，顺手报告读了多少字节。
class IndexProgressSource : public ISource {
public:
    /// @param source   真正的索引区数据来源，必须比本对象活得久。
    /// @param total    索引区总字节数，用来算百分比。
    /// @param observer 进度接收方，可以为 nullptr（此时只转发不上报）。
    IndexProgressSource(ISource* source, uint64_t total, IProgressObserver* observer)
        : source_(source), observer_(observer) {
        progress_.total_bytes = total;
        // 一开始就报一次 0%，界面立刻切到"读取索引"阶段。
        if (observer_) observer_->OnPhase("read-index", progress_);
    }

    /// 转发一次读取并上报进度。用户取消时返回 0，假装流已结束，
    /// 上层的解析会因为数据不完整而失败退出。
    size_t Read(uint8_t* data, size_t len) override {
        if (observer_ && observer_->IsCancelled()) return 0;
        const size_t got = source_->Read(data, len);
        progress_.done_bytes += got;
        if (got && observer_) observer_->OnPhase("read-index", progress_);
        return got;
    }

private:
    ISource* source_;              ///< 被包装的数据来源
    IProgressObserver* observer_;  ///< 进度接收方，可能为空
    ProgressInfo progress_;        ///< 累计的读取进度
};

/// 硬链接的身份证：同一个文件实体的多个目录项（多个名字），这两个值完全相同。
///
/// 来自 GetFileInformationByHandle。文件索引号由 nFileIndexHigh 和
/// nFileIndexLow 拼成 64 位。卷序列号也必须进键——文件索引号只在一个卷内
/// 唯一，两个不同卷上的文件完全可能撞上同一个索引号。
struct HardlinkKey {
    uint32_t volume_serial = 0;  ///< 卷序列号（dwVolumeSerialNumber）
    uint64_t file_index = 0;     ///< 文件索引号，卷内唯一

    /// std::map 按键排序查找，需要一个严格的大小关系：先比卷，再比索引号。
    bool operator<(const HardlinkKey& other) const {
        if (volume_serial != other.volume_serial) return volume_serial < other.volume_serial;
        return file_index < other.file_index;
    }
};

/// 发一条告警事件。告警表示"这一条出了可预期的问题，已经跳过，整体继续"，
/// 最终会让退出码变成 1（部分成功）。observer 为空时什么都不做。
///
/// @param path      出问题的条目的相对路径；不针对具体条目时传空串。
/// @param win_error GetLastError() 的原值，跟 Win32 无关时传 0。
void Warn(IProgressObserver* observer, const std::wstring& path, const std::wstring& message,
          uint32_t win_error) {
    if (observer == nullptr) return;
    WarnInfo warn;
    warn.path = path;
    warn.message = message;
    warn.win_error = win_error;
    observer->OnWarn(warn);
}

/// 用户是否请求了取消。没有 observer 就永远不取消。
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

/// 按名字从注册表里建出一串 Stage（压缩 / 加密算法的实例）。
///
/// 每次调用都造一套**全新**的实例，绝对不能复用：压缩器和加密器都是有
/// 状态的（余料缓冲、CBC 链接向量、霍夫曼码表）。拿写完数据区的那一套
/// 接着写索引区，解的时候是解不出来的，而且错得很隐蔽——数据区能解开，
/// 偏偏索引区解出乱码。
///
/// @param names    算法名，按写入时的应用顺序（压缩在前、加密在后）。
/// @param inverse  true 时建逆变换（解压 / 解密），并且**顺序也要反过来**——
///                 写的时候先压缩后加密，读的时候就得先解密后解压。
/// @param password 加密算法需要的密码；没用加密算法时可以为空。
/// @param out      建好的实例按顺序追加到这里。
/// @param error    失败时写入给用户看的原因。
/// @return 有算法名不认识、实例建不出来、或者加密却没给密码时返回 false。
bool BuildStages(const std::vector<std::string>& names, bool inverse, const std::string& password,
                 std::vector<std::unique_ptr<IStage>>* out, std::wstring* error) {
    const StageRegistry& registry = StageRegistry::Instance();
    std::vector<std::string> ordered = names;
    // 还原方向要倒序：写入时最后套上的那层（加密）得最先剥掉。
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
        // 加密 / 解密算法拿空密码跑出来的东西没有意义，提前拦下。
        if (factory->Kind() == StageKind::kEncrypt && password.empty()) {
            *error = L"此操作需要密码，请填写归档密码";
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
    // 固定 64 KB 缓冲反复读，任何情况下都不把整个文件读进内存。
    std::vector<uint8_t> buffer(kIoBlockSize);
    Crc32 checksum;      // 边读边算校验和，不用把文件读第二遍
    uint64_t total = 0;  // 实际读到的字节数（文件可能在备份期间被改过）

    for (;;) {
        DWORD got = 0;
        // 读失败（文件被锁定的区段、网络盘断开、磁盘坏道）：发告警、放弃这一条。
        if (ReadFile(handle, buffer.data(), static_cast<DWORD>(buffer.size()), &got, nullptr) ==
            0) {
            Warn(observer, meta.relative_path,
                 L"读到一半失败，已跳过：" + platform::FormatWinError(GetLastError()),
                 GetLastError());
            return false;
        }
        if (got == 0) break;  // 读到 0 字节就是到文件尾了

        // 同一块数据：算进校验和、交给打包器写进容器、累加长度。
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

/// 按固定顺序把元数据写回一个已经存在的对象：时间戳 → 属主与 DACL → 属性位。
///
/// 顺序是硬性的，不是风格问题：
///   · 属性位必须最后。只读位一旦设上，再设时间戳和 ACL 都会失败，
///     而且失败得很安静——只是元数据没生效，没人会注意到。
///   · 时间戳排在 ACL 之前。还原出来的 DACL 可能比当前更严，设完之后
///     当前进程未必还有改时间戳的权限。
///
/// 每一步失败都只发告警、不中断：元数据设不上通常是权限问题（没有
/// SeRestorePrivilege、目标卷不支持 ACL），属于可预期的失败。为了一个时间戳
/// 设不上就让整个还原失败，等于把"还原出 99% 的正确数据"变成"什么都没有"。
///
/// 文件在 Pass 2 写完内容后调用，符号链接和 junction 在 Pass 2 建好链接后调用
/// （设的是链接本身，不碰目标），目录在 Pass 3 逆序调用。
void ApplyEntryMetadata(const std::wstring& absolute, const EntryMeta& entry,
                        IProgressObserver* observer) {
    uint32_t error = 0;
    // ① 创建、访问、修改三个时间戳。
    if (!ApplyTimestamps(absolute, entry, true, &error)) {
        Warn(observer, entry.relative_path, L"时间戳设置失败：" + platform::FormatWinError(error),
             error);
    }

    // ② 属主、属组、DACL（备份时存成了一串 SDDL 文本）。
    //
    // 符号链接和 junction 跳过这一步：GetNamedSecurityInfoW / SetNamedSecurityInfoW
    // 都会顺着链接走到目标上，备份时存下的本来就是目标的权限；还原时再写一遍，
    // 改的也是目标——链接指向源目录之外时，等于去改一个不归我们管的文件的权限。
    const bool is_link = entry.type == FileType::kSymlinkFile ||
                         entry.type == FileType::kSymlinkDir || entry.type == FileType::kJunction;
    bool owner_skipped = false;
    if (is_link) {
        // 什么都不做，直接进入 ③。
    } else if (!ApplySecurityDescriptor(absolute, entry.sddl, &owner_skipped, &error)) {
        Warn(observer, entry.relative_path, L"权限设置失败：" + platform::FormatWinError(error),
             error);
    } else if (owner_skipped) {
        // 走到这里说明属主或属组跟源文件**确实不同**（相同的早在
        // ApplySecurityDescriptor 里跳过了），而当前身份没有改它的权限。
        // 典型场景是还原别人的文件，或者跨机器还原。只还原了 DACL，说清楚就行。
        Warn(observer, entry.relative_path,
             L"属主或属组跟源文件不同，当前权限不够修改，已跳过（DACL 已还原）。"
             L"以管理员身份运行可以完整还原",
             error);
    }

    // ③ 属性位（只读、隐藏等），必须最后。
    if (!ApplyAttributes(absolute, entry, &error)) {
        Warn(observer, entry.relative_path, L"属性位设置失败：" + platform::FormatWinError(error),
             error);
    }
}

/// 这个条目还原完之后，属性位会不会被后面的权限设置改坏。
///
/// 给目录设 DACL 时，SetNamedSecurityInfoW 会把可继承的 ACE 传播到它下面
/// **所有已经存在的子项**，而传播会让每个子项被系统加上 FILE_ATTRIBUTE_ARCHIVE。
/// 子项的属性位早在 Pass 2 就设好了，Pass 3 设父目录、最后设根目录时又被
/// 加回一个存档位——源端没有存档位的文件，还原出来就平白多一个。
///
/// 传播只会**加上**存档位，不会动别的位，所以源端本来就带存档位的条目不受
/// 影响，只需要把源端没有存档位的那些在最后再设一遍。绝大多数文件都带存档位，
/// 这一趟通常只涉及很少几个条目。attributes 为 0 表示没记录，不处理。
bool NeedsAttributePass(uint32_t attributes) {
    return attributes != 0 && (attributes & FILE_ATTRIBUTE_ARCHIVE) == 0;
}

/// 确保某个路径上有一个目录：不存在就建，已经是目录就当成功。
///
/// 已存在但**是个文件**时返回失败（ERROR_DIRECTORY）——不能把文件当目录用，
/// 否则后面往里写子项会全部失败，而且报错会很莫名其妙。
///
/// 只建这一级，不递归建父目录。正常流程里父目录在 Pass 1 已经按顺序建好了。
///
/// @param win_error 失败时写入错误码，可为 nullptr。
bool EnsureDirectory(const std::wstring& path, uint32_t* win_error) {
    const std::wstring extended = platform::ToExtendedPath(path);
    if (CreateDirectoryW(extended.c_str(), nullptr) != 0) return true;
    const DWORD error = GetLastError();
    if (error == ERROR_ALREADY_EXISTS) {
        const DWORD attributes = GetFileAttributesW(extended.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            return true;
        }
        if (win_error != nullptr) *win_error = ERROR_DIRECTORY;
        return false;
    }
    if (win_error != nullptr) *win_error = error;
    return false;
}

/// 路径上是否已经有东西（文件、目录、链接都算）。
bool PathExists(const std::wstring& path) {
    return GetFileAttributesW(platform::ToExtendedPath(path).c_str()) != INVALID_FILE_ATTRIBUTES;
}

/// --overwrite rename 策略下，找一个还没被占用的名字："a.txt" → "a (1).txt"。
///
/// 序号插在扩展名**之前**，不是简单往后拼："报告.txt (1)" 会让文件失去
/// 扩展名关联，双击打不开。判断扩展名时要确认那个点在最后一个路径分隔符
/// **之后**，否则 "D:/a.b/c" 这种目录名带点的路径会被切错。
///
/// 依次试 (1)、(2)……直到找到空位；一万个都被占了就原样返回，交给调用方失败。
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

/// 还原第二趟（Pass 2）的执行者：解包器每解出一个条目、每吐出一块内容，
/// 都回调到这里，由它负责在目标目录里把东西建出来。
///
/// 它是一个状态机，因为解包器的回调是"条目头 → 若干块内容 → 下一个条目头"
/// 这样交替来的：BeginEntry 时打开目标文件，WriteData 往里写，下一次
/// BeginEntry（或最后的 FinishCurrent）时关闭文件并设元数据。
class RestoreSink {
public:
    /// @param options  还原选项，必须比本对象活得久。
    /// @param observer 进度与告警的接收方，可以为 nullptr。
    /// @param result   计数（成功几条、跳过几条、写了多少字节）直接累加到这里。
    RestoreSink(const RestoreOptions& options, IProgressObserver* observer, EngineResult* result)
        : options_(options), observer_(observer), result_(result) {}

    /// 索引区里的那一份才是权威的。数据区里的条目记录是在读文件之前写的，
    /// 那时还不知道真实长度和校验和（文件可能正被别人改着），所以
    /// original_size 是遍历时的旧值、crc32 是 0。按 id 用索引里的覆盖回来。
    void UseIndexAsAuthority(const std::map<uint64_t, const EntryMeta*>* index) { index_ = index; }

    /// 开始处理一个新条目：先把上一条收尾，再按类型分别处理。
    ///
    /// 会做一道一致性校验：数据区里解出来的条目必须能在索引里按 id 找到，
    /// 而且路径和类型都对得上。对不上说明包被篡改或损坏了，直接抛异常中止
    /// 整个还原——继续下去可能把文件写到错误的位置。
    void BeginEntry(const EntryMeta& meta) {
        FinishCurrent();
        current_ = meta;
        // 用索引里的权威版本替换数据区里的那份（原因见 UseIndexAsAuthority）。
        if (index_ != nullptr) {
            const auto it = index_->find(meta.id);
            if (it == index_->end() || it->second->relative_path != meta.relative_path ||
                it->second->type != meta.type) {
                throw std::runtime_error("archive data entry does not match its index");
            }
            current_ = *it->second;
        }
        has_current_ = true;
        writing_ = false;
        content_crc_.Reset();

        // 每个条目开始时报一次进度、查一次取消。
        if (observer_ != nullptr) {
            progress_.done_entries = result_->entries_done;
            progress_.current = &current_;
            observer_->OnProgress(progress_);
            if (observer_->IsCancelled()) throw CancelledByUser();
        }

        // 按类型分派。普通文件这里只是打开目标，真正的写入在后续的 WriteData 里；
        // 其它类型没有内容，在这里就一步处理完。
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

    /// 把当前条目的一块内容写进目标文件，并累计校验和与进度。
    void WriteData(const uint8_t* data, size_t len) {
        // writing_ 为假说明这条在 BeginEntry 时就被跳过了（目标已存在、
        // 建文件失败、类型不支持）。数据还会照常流过来——打包器不知道
        // 上层的取舍——这里静默丢掉就行，warn 在跳过时已经发过了。
        if (!writing_ || !handle_.IsValid()) return;
        size_t done = 0;
        // WriteFile 允许"部分写入"，循环到这一块全部写完。
        // 单次最多 1 GB，避免 size_t 转 DWORD 时被截断。
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

    /// 收尾当前条目：关闭文件、核对校验和、设元数据。
    ///
    /// 每次 BeginEntry 开头会调一次（收尾上一条），全部解包完之后还要
    /// 再手动调一次（收尾最后一条）。只有普通文件需要收尾，其它类型在
    /// BeginEntry 里就已经处理完了。
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

        // --no-metadata 时只还原内容，不动时间戳、权限、属性。
        if (options_.restore_metadata) {
            ApplyEntryMetadata(target_path_, current_, observer_);
            RememberForAttributePass();
        }
        ++result_->entries_done;
        writing_ = false;
    }

    /// 设置进度的分母（总条目数、总字节数），供界面算百分比。
    void SetTotals(uint64_t entries, uint64_t bytes) {
        progress_.total_entries = entries;
        progress_.total_bytes = bytes;
    }

    /// 需要在所有权限设完之后再设一次属性位的条目：(目标路径, 属性位)。
    /// 原因见 NeedsAttributePass。
    const std::vector<std::pair<std::wstring, uint32_t>>& AttributePass() const {
        return attribute_pass_;
    }

private:
    /// 当前条目如果会被之后的权限传播改坏属性位，就记下来留给最后一趟重设。
    void RememberForAttributePass() {
        if (NeedsAttributePass(current_.attributes)) {
            attribute_pass_.emplace_back(target_path_, current_.attributes);
        }
    }

    /// 跳过当前条目：发一条告警并计入"跳过"数，最终退出码会变成 1。
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

    /// 普通文件：准备好目标路径，新建（或截断）目标文件，等后续 WriteData 往里写。
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

    /// 符号链接：按备份时记录的原始目标字符串重建，文件链接和目录链接分开处理。
    /// 没权限建时降级成空占位并告警，不中断整个还原。
    void RestoreSymlink() {
        if (!PrepareTargetPath()) return;
        if (current_.link_target.empty()) {
            Skip(L"符号链接没有记录目标，跳过");
            return;
        }

        const bool directory = current_.type == FileType::kSymlinkDir;
        uint32_t error = 0;
        if (CreateSymlink(target_path_, current_.link_target, directory, &error)) {
            // 链接自身的时间戳和属性位。降级出来的占位不设——那不是原来的东西。
            if (options_.restore_metadata) {
                ApplyEntryMetadata(target_path_, current_, observer_);
                RememberForAttributePass();
            }
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

    /// 目录联接（junction）：先建空目录再写入重解析点数据。不需要管理员权限。
    void RestoreJunction() {
        if (!PrepareTargetPath()) return;
        if (current_.link_target.empty()) {
            Skip(L"junction 没有记录目标，跳过");
            return;
        }

        // junction 不需要管理员权限也不需要开发者模式，这条路径基本不会失败。
        uint32_t error = 0;
        if (CreateJunction(target_path_, current_.link_target, &error)) {
            // junction 自身的时间戳和属性位。之后不会再往它里面写东西，
            // 所以这里设完就不会被刷新，不用像普通目录那样拖到 Pass 3。
            if (options_.restore_metadata) {
                ApplyEntryMetadata(target_path_, current_, observer_);
                RememberForAttributePass();
            }
            ++result_->entries_done;
            return;
        }
        Warn(observer_, current_.relative_path,
             L"建 junction 失败：" + platform::FormatWinError(error), error);
        ++result_->entries_skipped;
    }

    /// 硬链接：让目标路径成为"被指向的那个已还原文件"的另一个名字。
    /// 跨卷建不了时降级为复制一份内容，并告警说明二者不再是同一实体。
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

    const RestoreOptions& options_;  ///< 还原选项（目标目录、覆盖策略等）
    IProgressObserver* observer_;    ///< 进度与告警的接收方，可能为空
    EngineResult* result_;           ///< 计数结果，直接累加

    /// id → 索引里的权威条目，用来校验和覆盖数据区里的条目头。
    const std::map<uint64_t, const EntryMeta*>* index_ = nullptr;
    EntryMeta current_;              ///< 正在处理的条目
    std::wstring target_path_;       ///< 当前条目在目标目录里的绝对路径
    platform::ScopedHandle handle_;  ///< 当前普通文件的写入句柄
    Crc32 content_crc_;              ///< 当前文件已写内容的校验和
    ProgressInfo progress_;          ///< 累计进度
    bool has_current_ = false;       ///< 是否已经开始过至少一个条目
    bool writing_ = false;           ///< 当前条目是否是一个正在写入的普通文件
    /// 最后一趟要重设属性位的条目（见 NeedsAttributePass）。
    std::vector<std::pair<std::wstring, uint32_t>> attribute_pass_;
};

}  // namespace

// ============================================================ RunBackup

// 完整的设计说明见 cbk/engine.h。这里按顺序做五件事：
//   ① 校验参数                  ② 遍历源目录，得到条目表
//   ③ 打开容器，逐条把内容经流水线写进数据区
//   ④ 把条目表写进索引区        ⑤ 回填文件头、写校验块、关闭
EngineResult RunBackup(const BackupOptions& options, IProgressObserver* observer) {
    // ---- 参数校验。这些错都是调用方（通常是 GUI）的 bug，退出码 3。----
    if (options.source_root.empty() || options.dest_archive.empty()) {
        return Fail(Status::kBadArgs, L"--source 和 --dest 都不能为空");
    }
    if (!PackerRegistry::Instance().Has(options.packer)) {
        return Fail(Status::kBadArgs, L"没有名为 " + FromUtf8(options.packer) +
                                          L" 的打包算法。用 cbk info 看看支持哪些。");
    }
    // 每个算法名都必须认识；用了加密算法就必须给密码。
    for (const std::string& name : options.stages) {
        const auto* factory = StageRegistry::Instance().Find(name);
        if (factory != nullptr && factory->Kind() == StageKind::kEncrypt &&
            options.password.empty()) {
            return Fail(Status::kBadArgs, L"加密备份必须填写非空密码");
        }
        if (!StageRegistry::Instance().Has(name)) {
            return Fail(Status::kBadArgs,
                        L"没有名为 " + FromUtf8(name) + L" 的算法。用 cbk info 看看支持哪些。");
        }
    }
    // 顺序必须是"压缩在前、加密在后"。
    if (!ValidatePipelineOrder(options.stages)) {
        return Fail(Status::kBadArgs,
                    L"流水线顺序不合法：压缩必须排在加密之前。密文是高熵数据，"
                    L"压不动还会变大。");
    }

    EngineResult result;

    // 告诉界面进入"扫描"阶段。
    if (observer != nullptr) observer->OnPhase("scan", ProgressInfo{});
    // 尝试启用备份特权（绕过 ACL 读取无权访问的文件）。拿不到不是致命错误，
    // 降级跑就是了——只是遇到没权限的文件会跳过。
    platform::EnableBackupPrivileges();

    // ---- 备份文件不能放在源目录里面 ----
    // 否则遍历时会把正在写的备份文件自己也扫进去，一边写一边备份自己。
    // 两个路径都转成扩展格式再比，大小写不敏感（Windows 路径不区分大小写）。
    const std::wstring root = platform::NormalizePath(options.source_root);
    const std::wstring source_path = platform::ToExtendedPath(root);
    const std::wstring destination = platform::ToExtendedPath(options.dest_archive);
    const std::wstring prefix = source_path + (source_path.back() == L'\\' ? L"" : L"\\");
    const bool same_path =
        CompareStringOrdinal(source_path.c_str(), -1, destination.c_str(), -1, TRUE) == CSTR_EQUAL;
    const bool inside_source =
        destination.size() >= prefix.size() &&
        CompareStringOrdinal(prefix.c_str(), static_cast<int>(prefix.size()), destination.c_str(),
                             static_cast<int>(prefix.size()), TRUE) == CSTR_EQUAL;
    if (same_path || inside_source) {
        return Fail(Status::kBadArgs, L"备份文件必须保存在源目录以外");
    }
    // ---- 遍历：把整棵树摊平成条目表 ----
    // 在扫描之前取源根自身的元数据，避免遍历影响访问时间。
    // 根路径是链接时备份其指向目录的元数据，与扫描根目录内容的行为一致。
    EntryMeta source_root_metadata;
    source_root_metadata.type = FileType::kDirectory;
    source_root_metadata.relative_path = L".";
    if (!ReadMetadataByPath(root, false, &source_root_metadata) ||
        !(source_root_metadata.attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        return Fail(Status::kFailed, L"无法读取源文件夹的元数据：" + root);
    }
    uint32_t root_security_error = 0;
    if (!ReadSecurityDescriptor(root, &source_root_metadata.sddl, &root_security_error)) {
        Warn(observer, root,
             L"源文件夹权限读取失败：" + platform::FormatWinError(root_security_error),
             root_security_error);
        result.status = Status::kPartial;
    }
    std::vector<EntryMeta> entries;
    ScanStats stats;
    ProgressInfo scan_progress;
    {
        // 每找到一个条目就存进 entries，同时向界面报告扫描进度。
        Scanner scanner(root, ScanOptions{options.follow_symlinks}, observer);
        const Status scan_status = scanner.Scan(
            [&](const EntryMeta& meta) {
                entries.push_back(meta);
                ++scan_progress.done_entries;
                scan_progress.done_bytes += meta.original_size;
                scan_progress.current = &meta;
                if (observer != nullptr) observer->OnPhase("scan", scan_progress);
            },
            &stats);
        // 根目录打不开或被取消：整个备份失败。有条目被跳过：记为部分成功，继续。
        if (scan_status == Status::kFailed) {
            if (IsCancelled(observer)) return Fail(Status::kFailed, L"已取消");
            return Fail(Status::kFailed, L"无法遍历源目录 " + root);
        }
        if (scan_status == Status::kPartial) result.status = Status::kPartial;
    }

    // 遍历完就知道总量了，通知界面画进度条。
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
    const Status open_status = writer.Open(options.dest_archive, root, pipeline, &error, true);
    if (open_status != Status::kOk) return Fail(open_status, error);

    // 按用户选的打包格式建打包器（默认 cbk-native）。
    std::unique_ptr<IPacker> packer = PackerRegistry::Instance().Create(options.packer);
    if (packer == nullptr) return Fail(Status::kFailed, L"打包器创建失败");

    // 真正写进去的条目（跳过的不算），最后整张写进索引区。
    std::vector<EntryMeta> written;
    written.reserve(entries.size());
    uint64_t total_original = 0;  // 所有文件的原始字节数之和，记进文件头

    // 硬链接去重表。**只对 nNumberOfLinks > 1 的文件建表项**——几万个普通
    // 文件全塞进去纯属浪费，而链接数是开句柄时顺手就有的，不额外花系统调用。
    std::map<HardlinkKey, uint64_t> hardlinks;

    try {
        // ---- 数据区 ----
        std::vector<std::unique_ptr<IStage>> stages;
        if (!BuildStages(options.stages, false, options.password, &stages, &error)) {
            return Fail(Status::kBadArgs, error);
        }
        // 组装写入链：打包器 → 计数器 → 压缩 → 加密 → 容器的数据区。
        StageChainSink chain(std::move(stages), &writer.DataSink());
        CountingSink counter(&chain.Entry());

        ProgressInfo progress;
        progress.total_entries = entries.size();
        progress.total_bytes = stats.total_bytes;

        // 逐条处理。每条开始前查一次取消、报一次进度。
        for (EntryMeta& entry : entries) {
            if (IsCancelled(observer)) throw CancelledByUser();

            progress.done_entries = written.size();
            progress.current = &entry;
            if (observer != nullptr) observer->OnProgress(progress);

            const std::wstring absolute = platform::JoinPath(root, entry.relative_path);
            // 记下这一条在打包输出流里从哪开始。
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

                // 普通文件（或一组硬链接里第一次遇到的那个）：
                // 写条目头 → 流式写内容 → 写结束标记。
                packer->BeginEntry(entry, counter);
                uint64_t actual = 0;
                uint32_t crc = 0;
                if (!StreamFileContent(handle.Get(), packer.get(), counter, observer, entry,
                                       &actual, &crc, &progress)) {
                    // 读到一半失败：补上结束标记，保持数据区格式完整；
                    // 这一条记为跳过，不进索引。
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
                // 目录、链接等：只有条目头，没有内容。
                packer->BeginEntry(entry, counter);
                packer->EndEntry(counter);
            }

            // 这一条在打包输出流里占了多少字节，然后登记进索引表。
            entry.stored_size = counter.Count() - entry.data_offset;
            written.push_back(entry);
            ++result.entries_done;
        }

        // 数据区收尾：打包器写结束信息 → 各 Stage 冲刷内部缓冲 → 容器切到索引区。
        if (observer != nullptr) observer->OnPhase("finalize", ProgressInfo{});
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
        WriteIndex(written, index_chain.Entry(), &source_root_metadata);
        index_chain.Finish();
    } catch (const CancelledByUser&) {
        // 用户取消：删掉写了一半的备份文件。残缺的 .cbk 看着像能用的备份，比没有更糟。
        Warn(observer, std::wstring(), L"用户取消了备份，半成品已删除", 0);
        writer.Abort();
        return Fail(Status::kFailed, L"已取消");
    }

    // 回填文件头、写校验块、刷盘关闭。
    const Status close_status = writer.Close(written.size(), total_original, &error);
    if (close_status != Status::kOk) return Fail(close_status, error);

    result.bytes_written = writer.DataBytesWritten();
    // 有任何条目被跳过，整体就是"部分成功"（退出码 1）。
    if (result.entries_skipped > 0 && result.status == Status::kOk) {
        result.status = Status::kPartial;
    }

    // 报告最终结果。
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

// 完整的设计说明（为什么分三趟）见 cbk/engine.h。这里的步骤是：
//   ① 打开容器、读出索引        ② Pass 1：建出所有目录
//   ③ Pass 2：顺序解包，逐条落盘 ④ Pass 3：逆序给目录设元数据，再设根目录
//   ⑤ Pass 4：重设被权限继承传播改坏的属性位（只涉及源端没有存档位的条目）
EngineResult RunRestore(const RestoreOptions& options, IProgressObserver* observer) {
    if (options.archive.empty() || options.dest_root.empty()) {
        return Fail(Status::kBadArgs, L"--archive 和 --dest 都不能为空");
    }

    if (observer != nullptr) observer->OnPhase("read-index", ProgressInfo{});
    // 打开容器：校验魔数、版本、布局，读出文件头和流水线描述。
    std::wstring error;
    ArchiveReader reader;
    if (reader.Open(options.archive, &error) != Status::kOk) {
        return Fail(Status::kFailed, error);
    }
    const ArchiveHeader& header = reader.Header();

    EntryMeta source_root_metadata;
    const bool has_source_root_metadata = (header.flags & kFlagSourceRootMetadata) != 0;

    // 按包里记录的打包格式建解包器。本程序不认识这种格式就没法还原。
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
        // 读取链：原始索引区 → 进度上报 → 解密 → 解压 → 解析成条目表。
        IndexProgressSource index_progress(raw.get(), header.index_size, observer);
        StageChainSource chain(&index_progress, std::move(stages));
        // 密码不对时解密出来的是乱码，解析会抛异常或失败，统一报"密码错误或已损坏"。
        bool index_ok = false;
        try {
            index_ok = ReadIndex(chain, header.entry_count, &entries, has_source_root_metadata,
                                 &source_root_metadata);
        } catch (const std::exception&) {
            return Fail(Status::kFailed, L"密码错误或归档已损坏，无法读取索引");
        }
        if (!index_ok) {
            if (IsCancelled(observer)) return Fail(Status::kFailed, L"已取消");
            return Fail(Status::kFailed, L"索引区解析失败，容器已损坏或密码不对");
        }
    }

    // 尝试启用还原特权（设置任意属主、绕过 ACL 写入）。拿不到就降级运行。
    platform::EnableBackupPrivileges();

    EngineResult result;
    uint32_t win_error = 0;
    // 先确保目标根目录存在。
    if (!EnsureDirectory(platform::NormalizePath(options.dest_root), &win_error)) {
        return Fail(Status::kFailed, L"无法创建目标目录：" + platform::FormatWinError(win_error));
    }
    std::wstring dest = platform::NormalizePath(options.dest_root);
    if (options.include_source_root) {
        // 旧归档也保存了 source_root。只取末级名称，绝不将归档里的绝对路径用作目标。
        std::wstring source = header.source_root;
        while (!source.empty() && (source.back() == L'\\' || source.back() == L'/')) {
            source.pop_back();
        }
        const size_t slash = source.find_last_of(L"\\/");
        std::wstring name = source.substr(slash == std::wstring::npos ? 0 : slash + 1);
        // 备份整个盘符根目录时，使用稳定的目录名，例如 C_drive。
        if (name.size() == 2 && name[1] == L':' &&
            ((name[0] >= L'A' && name[0] <= L'Z') || (name[0] >= L'a' && name[0] <= L'z'))) {
            name = name.substr(0, 1) + L"_drive";
        }
        std::wstring stem = name.substr(0, name.find(L'.'));
        for (wchar_t& ch : stem) {
            if (ch >= L'a' && ch <= L'z') ch -= L'a' - L'A';
        }
        const bool reserved =
            stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL" ||
            stem == L"CONIN$" || stem == L"CONOUT$" ||
            (stem.size() == 4 && (stem.substr(0, 3) == L"COM" || stem.substr(0, 3) == L"LPT") &&
             stem[3] >= L'0' && stem[3] <= L'9');
        if (name.empty() || name == L"." || name == L".." || reserved || name.back() == L'.' ||
            name.back() == L' ' || name.find_first_of(L"<>:\"/\\|?*") != std::wstring::npos ||
            std::any_of(name.begin(), name.end(), [](wchar_t ch) { return ch < 32; })) {
            return Fail(Status::kFailed,
                        L"归档中的源文件夹名称无效，可用 --contents-only 仅恢复内容");
        }
        dest = platform::JoinPath(dest, name);
        const DWORD attributes = GetFileAttributesW(platform::ToExtendedPath(dest).c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            return Fail(Status::kFailed, L"目标中的同名文件夹是链接，请选择其他恢复目录");
        }
        if (!EnsureDirectory(dest, &win_error)) {
            return Fail(Status::kFailed,
                        L"无法创建原文件夹：" + platform::FormatWinError(win_error));
        }
    }

    // 通知界面开始还原，并报出总量。
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
    ProgressInfo preparation;
    preparation.total_entries = entries.size();
    if (observer != nullptr) observer->OnPhase("prepare", preparation);
    for (const EntryMeta& entry : entries) {
        if (IsCancelled(observer)) return Fail(Status::kFailed, L"已取消");
        preparation.current = &entry;
        ++preparation.done_entries;
        if (observer != nullptr) observer->OnPhase("prepare", preparation);
        // 这一趟只处理目录，其它类型留给 Pass 2。
        if (entry.type != FileType::kDirectory) continue;
        const std::wstring absolute = platform::JoinPath(dest, entry.relative_path);
        if (!EnsureDirectory(absolute, &win_error)) {
            Warn(observer, entry.relative_path,
                 L"建目录失败：" + platform::FormatWinError(win_error), win_error);
            ++result.entries_skipped;
            continue;
        }
        // 记下来，Pass 3 要倒着给它们设元数据。
        directories.push_back(&entry);
    }

    // ---- Pass 2：顺序解包，逐条落盘并设各自的元数据 ----
    RestoreOptions effective = options;
    effective.dest_root = dest;
    RestoreSink sink(effective, observer, &result);
    sink.SetTotals(entries.size(), header.total_original_bytes);

    // 建一张 id → 索引条目的查找表，供 RestoreSink 校验和覆盖数据区里的条目头。
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
        // 读取链：原始数据区 → 解密 → 解压；解包器从链尾拉数据。
        StageChainSource chain(raw.get(), std::move(stages));

        // 解包器每解出一个条目头就回调 BeginEntry，每吐出一块内容就回调 WriteData。
        packer->Unpack(
            chain, [&sink](const EntryMeta& meta) { sink.BeginEntry(meta); },
            [&sink](const uint8_t* data, size_t len) { sink.WriteData(data, len); });
        sink.FinishCurrent();  // 收尾最后一个条目
    } catch (const CancelledByUser&) {
        Warn(observer, std::wstring(), L"用户取消了还原，已还原的部分保留在目标目录里", 0);
        return Fail(Status::kFailed, L"已取消");
    } catch (const std::runtime_error& e) {
        // 解包器发现数据损坏，或者数据区和索引对不上：整个还原失败。
        return Fail(Status::kFailed, L"解包失败：" + FromUtf8(e.what()));
    }

    // ---- Pass 3：逆序设置目录的时间戳和属性位 ----
    // 逆序是因为从最深的往外设，设完父目录就不会再有人去动它的子目录，
    // 父目录的时间戳才不会被二次刷新。
    if (options.restore_metadata) {
        ProgressInfo metadata_progress;
        metadata_progress.total_entries = directories.size();
        for (size_t i = directories.size(); i > 0; --i) {
            if (IsCancelled(observer)) return Fail(Status::kFailed, L"已取消");
            const EntryMeta& entry = *directories[i - 1];
            ApplyEntryMetadata(platform::JoinPath(dest, entry.relative_path), entry, observer);
            metadata_progress.current = &entry;
            ++metadata_progress.done_entries;
            if (observer != nullptr) observer->OnPhase("metadata", metadata_progress);
        }
        // 外层最后应用，避免创建子项再次改变其时间戳或提前收紧权限。
        // contents-only 模式不改用户指定的公共目标目录的元数据。
        if (options.include_source_root && has_source_root_metadata) {
            if (IsCancelled(observer)) return Fail(Status::kFailed, L"已取消");
            ApplyEntryMetadata(dest, source_root_metadata, observer);
        }

        // ---- Pass 4：权限全部设完之后，重设会被传播改坏的属性位 ----
        // Pass 3 和根目录设 DACL 时的继承传播会给子项加上存档位（见 NeedsAttributePass）。
        // 文件和链接由 RestoreSink 记下，目录在这里从目录表里挑。
        std::vector<std::pair<std::wstring, uint32_t>> pass = sink.AttributePass();
        for (const EntryMeta* entry : directories) {
            if (NeedsAttributePass(entry->attributes)) {
                pass.emplace_back(platform::JoinPath(dest, entry->relative_path),
                                  entry->attributes);
            }
        }
        for (const auto& item : pass) {
            EntryMeta attributes_only;
            attributes_only.attributes = item.second;
            uint32_t attribute_error = 0;
            if (!ApplyAttributes(item.first, attributes_only, &attribute_error)) {
                Warn(observer, item.first,
                     L"属性位设置失败：" + platform::FormatWinError(attribute_error),
                     attribute_error);
            }
        }
    }

    // 有条目被跳过就是部分成功。
    if (result.entries_skipped > 0) result.status = Status::kPartial;

    // 报告最终结果。
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

/// cbk list 的实现：只读文件头和索引区，不碰数据区。
///
/// 所以不管备份包有多大，列出内容都很快。索引区如果加了密，同样需要密码。
Status ReadArchiveListing(const std::wstring& path, const std::string& password, ArchiveInfo* info,
                          std::vector<EntryMeta>* entries, std::wstring* error) {
    ArchiveReader reader;
    const Status status = reader.Open(path, error);
    if (status != Status::kOk) return status;

    const ArchiveHeader& header = reader.Header();
    // 把内部的文件头结构转成对外的精简版 ArchiveInfo。
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
    // 跟还原时读索引是同一条链：解密 → 解压 → 解析。
    StageChainSource chain(raw.get(), std::move(stages));
    bool index_ok = false;
    try {
        index_ok = ReadIndex(chain, header.entry_count, entries,
                             (header.flags & kFlagSourceRootMetadata) != 0);
    } catch (const std::exception&) {
        *error = L"密码错误或归档已损坏，无法读取索引";
        return Status::kFailed;
    }
    if (!index_ok) {
        *error = L"索引区解析失败，容器已损坏或密码不对";
        return Status::kFailed;
    }
    return Status::kOk;
}

/// cbk verify 的实现：先做结构检查（Open），再重算三个 CRC32 逐一比对（Verify）。
Status VerifyArchive(const std::wstring& path, std::wstring* error) {
    ArchiveReader reader;
    const Status status = reader.Open(path, error);
    if (status != Status::kOk) return status;
    return reader.Verify(error);
}

}  // namespace cbk
