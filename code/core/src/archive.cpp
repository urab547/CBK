// Copyright 2026 CBK Project. .cbk 容器读写的实现。
#include "src/archive.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "cbk/stage.h"
#include "src/byte_io.h"
#include "src/crc32.h"
#include "src/entry_codec.h"

namespace cbk {

namespace {

// ---- FileHeader 各字段的偏移，跟 docs/实现方案.md 3.2 节一一对应 ----
constexpr size_t kOffMagic = 0;
constexpr size_t kOffFormatVersion = 4;
constexpr size_t kOffFlags = 6;
constexpr size_t kOffHeaderSize = 8;
constexpr size_t kOffPipelineLen = 12;
constexpr size_t kOffCreatedAt = 16;
constexpr size_t kOffEntryCount = 24;
constexpr size_t kOffDataOffset = 32;
constexpr size_t kOffDataSize = 40;
constexpr size_t kOffIndexOffset = 48;
constexpr size_t kOffIndexSize = 56;
constexpr size_t kOffTotalOriginal = 64;
constexpr size_t kOffSourceRootLen = 72;
constexpr size_t kHeaderReservedLen = 54;

// ---- Footer ----
constexpr size_t kFooterReservedLen = 40;

// 循环写到写完为止。
//
// WriteFile 允许"部分写"——返回成功但 written < 要写的量。对本地磁盘
// 这几乎不发生，可一旦目标是网络盘或者管道就完全可能。漏了这个循环的
// bug 在开发机上永远复现不出来，只在别人的环境里偶发丢字节。
bool WriteAll(HANDLE handle, const uint8_t* data, size_t len) {
    size_t done = 0;
    while (done < len) {
        // 单次最多 1 GB，避免 size_t 到 DWORD 的截断。
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(len - done, 1u << 30));
        DWORD written = 0;
        if (!WriteFile(handle, data + done, chunk, &written, nullptr)) return false;
        if (written == 0) return false;
        done += written;
    }
    return true;
}

// 用 SetFilePointerEx 而不是 SetFilePointer。后者的偏移是 32 位的，
// 超过 4 GB 的包就定位不了了——而备份包超过 4 GB 是常态。
bool SeekTo(HANDLE handle, uint64_t offset) {
    LARGE_INTEGER position;
    position.QuadPart = static_cast<LONGLONG>(offset);
    return SetFilePointerEx(handle, position, nullptr, FILE_BEGIN) != 0;
}

bool FileSize(HANDLE handle, uint64_t* out) {
    LARGE_INTEGER size;
    if (!GetFileSizeEx(handle, &size)) return false;
    *out = static_cast<uint64_t>(size.QuadPart);
    return true;
}

// 只读文件的一个字节区间。数据区和索引区各用一个。
//
// 自己记 remaining_ 而不是靠读到文件尾来判断结束：数据区后面紧跟着
// 索引区和尾部，读过界的话会把索引的字节当成数据吐给打包器。
class FileRangeSource : public ISource {
public:
    FileRangeSource(platform::ScopedHandle handle, uint64_t size)
        : handle_(std::move(handle)), remaining_(size) {}

    size_t Read(uint8_t* buf, size_t len) override {
        if (remaining_ == 0 || len == 0) return 0;
        const DWORD want =
            static_cast<DWORD>(std::min<uint64_t>(static_cast<uint64_t>(len), remaining_));
        DWORD got = 0;
        if (!ReadFile(handle_.Get(), buf, want, &got, nullptr)) return 0;
        remaining_ -= got;
        return got;
    }

private:
    platform::ScopedHandle handle_;
    uint64_t remaining_;
};

/// 打开只读句柄并定位到 offset。
std::unique_ptr<ISource> OpenRange(const std::wstring& path, uint64_t offset, uint64_t size) {
    platform::ScopedHandle handle = platform::OpenForRead(path, true);
    if (!handle.IsValid()) return nullptr;
    if (!SeekTo(handle.Get(), offset)) return nullptr;
    return std::unique_ptr<ISource>(new FileRangeSource(std::move(handle), size));
}

// 把一个区间整个读一遍只为了算 CRC，内容不留。
//
// 固定 kIoBlockSize 缓冲，所以 verify 一个 100 GB 的包也只占 64 KB 内存。
uint32_t DrainCrc(ISource& source) {
    Crc32 crc;
    std::vector<uint8_t> buffer(kIoBlockSize);
    for (;;) {
        const size_t got = source.Read(buffer.data(), buffer.size());
        if (got == 0) break;
        crc.Update(buffer.data(), got);
    }
    return crc.Value();
}

// 算法名的字符集故意收得很紧：只允许小写字母、数字、连字符、点。
//
// 这样 PipelineDesc 的解析器永远不需要考虑转义——分隔符 ; = , 和大写
// 字母都进不来。源根路径不并进那一行，也正是因为路径里可能出现这些字符。
// 收紧字符集的代价是队友起名字受限，收益是解析器少一整类 bug。
bool IsValidAlgorithmName(const std::string& name) {
    if (name.empty()) return false;
    for (char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.';
        if (!ok) return false;
    }
    return true;
}

uint64_t NowAsFileTime() {
    FILETIME now = {};
    GetSystemTimeAsFileTime(&now);
    return (static_cast<uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
}

// 从 PipelineDesc 反推头部的 flags 位。
//
// flags 跟 PipelineDesc 是冗余的，但它在冻结的格式里，得如实填。
// 好处是外部工具不用解析那行 ASCII 就能一眼看出包有没有加密。
uint16_t ComputeFlags(const PipelineDesc& pipeline) {
    uint16_t flags = 0;
    if (!pipeline.packer.empty()) flags |= kFlagPacked;
    const StageRegistry& registry = StageRegistry::Instance();
    for (const std::string& name : pipeline.stages) {
        const IStageFactory* factory = registry.Find(name);
        if (factory == nullptr) continue;  // 名字合法性由调用方先校验
        if (factory->Kind() == StageKind::kCompress) flags |= kFlagCompressed;
        if (factory->Kind() == StageKind::kEncrypt) flags |= kFlagEncrypted;
    }
    return flags;
}

}  // namespace

// ============================================================ PipelineDesc

std::string PipelineDesc::Serialize() const {
    std::string text = "packer=" + packer + ";stages=";
    for (size_t i = 0; i < stages.size(); ++i) {
        if (i > 0) text.push_back(',');
        text += stages[i];
    }
    return text;
}

// 解析器故意写得很死板：必须严格是 "packer=X;stages=Y" 这个形状。
//
// 不做容错、不跳空白、不认大小写变体。宽松的解析器在这里是负资产——
// 它会把一个损坏的包"猜"成某个能解析的配置，然后拿错误的算法链去解数据，
// 产出一堆垃圾字节。直接拒绝、让调用方报错，才是对用户负责。
bool PipelineDesc::Parse(const std::string& text, PipelineDesc* out) {
    static const std::string kPackerKey = "packer=";
    static const std::string kStagesKey = ";stages=";

    if (text.compare(0, kPackerKey.size(), kPackerKey) != 0) return false;
    const size_t stages_at = text.find(kStagesKey, kPackerKey.size());
    if (stages_at == std::string::npos) return false;

    PipelineDesc parsed;
    parsed.packer = text.substr(kPackerKey.size(), stages_at - kPackerKey.size());
    if (!IsValidAlgorithmName(parsed.packer)) return false;

    const std::string joined = text.substr(stages_at + kStagesKey.size());
    if (!joined.empty()) {
        size_t start = 0;
        for (;;) {
            const size_t comma = joined.find(',', start);
            const size_t count = (comma == std::string::npos) ? std::string::npos : comma - start;
            const std::string name = joined.substr(start, count);
            if (!IsValidAlgorithmName(name)) return false;
            parsed.stages.push_back(name);
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
    }

    *out = std::move(parsed);
    return true;
}

// ============================================================ 索引区

void WriteIndex(const std::vector<EntryMeta>& entries, ISink& out) {
    for (const EntryMeta& entry : entries) {
        WriteEntryMeta(entry, out);
    }
}

// 读索引区。条数必须跟头部记的严格相等。
//
// 多一条少一条都说明包不对：少了是被截断，多了是头部被改过。这里不做
// 任何"能读多少算多少"的容错——索引是还原的地图，地图不完整就不该上路。
bool ReadIndex(ISource& source, uint64_t expected_count, std::vector<EntryMeta>* out) {
    std::vector<EntryMeta> entries;
    for (;;) {
        EntryMeta entry;
        bool end_of_stream = false;
        if (!ReadEntryMeta(source, &entry, &end_of_stream)) {
            if (end_of_stream) break;
            return false;  // 记录损坏
        }
        entries.push_back(std::move(entry));
        // 条数超了就别再读下去，防的是索引区被改坏成一个自增长的怪物。
        if (entries.size() > expected_count) return false;
    }
    if (entries.size() != expected_count) return false;
    *out = std::move(entries);
    return true;
}

// ============================================================ ArchiveWriter

/// 往文件顺序写，同时累计字节数和 CRC。数据区和索引区共用一个实例，
/// EndData() 时把计数快照下来再清零。
class ArchiveWriter::RegionSink : public ISink {
public:
    explicit RegionSink(HANDLE handle) : handle_(handle) {}

    void Write(const uint8_t* data, size_t len) override {
        if (len == 0) return;
        if (!WriteAll(handle_, data, len)) {
            failed_ = true;
            return;
        }
        crc_.Update(data, len);
        bytes_ += len;
    }

    void Restart() {
        crc_.Reset();
        bytes_ = 0;
    }

    bool Failed() const { return failed_; }
    uint64_t Bytes() const { return bytes_; }
    uint32_t Crc() const { return crc_.Value(); }

private:
    HANDLE handle_;
    Crc32 crc_;
    uint64_t bytes_ = 0;
    bool failed_ = false;
};

ArchiveWriter::ArchiveWriter() = default;

ArchiveWriter::~ArchiveWriter() {
    // 没成功 Close 就走到这里，说明中途出错或被取消了。
    // 留下残缺的 .cbk 比没有文件更糟——它看着像个能用的备份。
    if (phase_ != Phase::kDone) Abort();
}

Status ArchiveWriter::Fail(const std::wstring& what, std::wstring* error) {
    if (error != nullptr) {
        *error = what + L"：" + platform::FormatWinError(GetLastError());
    }
    Abort();
    return Status::kFailed;
}

Status ArchiveWriter::Open(const std::wstring& path, const std::wstring& source_root,
                           const PipelineDesc& pipeline, std::wstring* error) {
    source_root_utf8_ = ToUtf8(source_root);
    if (source_root_utf8_.size() > kMaxSourceRootUtf8) {
        if (error != nullptr) {
            *error = L"源根路径转成 UTF-8 后超过 65535 字节，容器头部存不下";
        }
        return Status::kBadArgs;
    }

    pipeline_text_ = pipeline.Serialize();

    file_ = platform::CreateForWrite(path);
    if (!file_.IsValid()) {
        path_.clear();  // 文件都没建出来，不该去删它
        if (error != nullptr) {
            *error = L"无法创建 " + path + L"：" + platform::FormatWinError(GetLastError());
        }
        return Status::kFailed;
    }
    path_ = path;

    header_ = ArchiveHeader();
    header_.created_at = NowAsFileTime();
    header_.source_root = source_root;
    header_.pipeline = pipeline;
    header_.flags = ComputeFlags(pipeline);
    header_.data_offset = kFileHeaderSize + pipeline_text_.size() + source_root_utf8_.size();

    // 先写一份占位头部，最后 Close 时再回填真实的偏移和长度。
    std::vector<uint8_t> prologue;
    prologue.reserve(header_.data_offset);
    ByteWriter writer(&prologue);
    writer.WriteZeros(kFileHeaderSize);
    writer.WriteBytes(pipeline_text_.data(), pipeline_text_.size());
    writer.WriteBytes(source_root_utf8_.data(), source_root_utf8_.size());

    if (!WriteAll(file_.Get(), prologue.data(), prologue.size())) {
        return Fail(L"写容器头部失败", error);
    }

    sink_.reset(new RegionSink(file_.Get()));
    phase_ = Phase::kData;
    return Status::kOk;
}

ISink& ArchiveWriter::DataSink() {
    return *sink_;
}

// 数据区写完，把计数快照下来再清零，同一个 RegionSink 接着服务索引区。
//
// 不新建一个 sink 是因为两个区在同一个文件里、位置连续，共用一个写入器
// 天然保证了"索引紧跟在数据后面"这个布局约束。
void ArchiveWriter::EndData() {
    if (phase_ != Phase::kData) return;
    data_bytes_ = sink_->Bytes();
    data_crc_ = sink_->Crc();
    sink_->Restart();
    phase_ = Phase::kIndex;
}

ISink& ArchiveWriter::IndexSink() {
    return *sink_;
}

// 收尾：算出各区的最终大小，写尾部，再回头把头部补完整。
//
// 为什么是"先写尾部再回填头部"这个顺序：头部回填要 seek 回文件开头，
// 而尾部必须落在文件末尾。反过来做的话，seek 到 0 之后文件指针就不在
// 末尾了，还得再 seek 一次回去——多一次定位就多一个出错的地方。
//
// 头部之所以要回填，是因为 dataSize / indexOffset / indexSize 这些值
// 只有全部写完才知道。这也是全流程唯一一次回头改已写出的字节。
Status ArchiveWriter::Close(uint64_t entry_count, uint64_t total_original_bytes,
                            std::wstring* error) {
    if (phase_ == Phase::kDone) return Status::kOk;
    if (phase_ == Phase::kClosed) {
        if (error != nullptr) *error = L"容器还没打开";
        return Status::kFailed;
    }
    if (phase_ == Phase::kData) EndData();

    if (sink_->Failed()) return Fail(L"写数据区失败", error);

    index_bytes_ = sink_->Bytes();
    index_crc_ = sink_->Crc();

    header_.entry_count = entry_count;
    header_.total_original_bytes = total_original_bytes;
    header_.data_size = data_bytes_;
    header_.index_offset = header_.data_offset + data_bytes_;
    header_.index_size = index_bytes_;

    // ---- 拼出最终的 128 字节头部 ----
    std::vector<uint8_t> head;
    head.reserve(kFileHeaderSize);
    ByteWriter writer(&head);
    writer.WriteBytes(kMagic, sizeof(kMagic));
    writer.WriteU16(header_.format_version);
    writer.WriteU16(header_.flags);
    writer.WriteU32(static_cast<uint32_t>(kFileHeaderSize));
    writer.WriteU32(static_cast<uint32_t>(pipeline_text_.size()));
    writer.WriteU64(header_.created_at);
    writer.WriteU64(header_.entry_count);
    writer.WriteU64(header_.data_offset);
    writer.WriteU64(header_.data_size);
    writer.WriteU64(header_.index_offset);
    writer.WriteU64(header_.index_size);
    writer.WriteU64(header_.total_original_bytes);
    writer.WriteU16(static_cast<uint16_t>(source_root_utf8_.size()));
    writer.WriteZeros(kHeaderReservedLen);
    if (head.size() != kFileHeaderSize) return Fail(L"头部长度不对", error);

    const uint32_t header_crc = ComputeCrc32(head.data(), head.size());

    // ---- 尾部 ----
    std::vector<uint8_t> footer;
    footer.reserve(kFooterSize);
    ByteWriter foot(&footer);
    foot.WriteBytes(kFooterMagic, sizeof(kFooterMagic));
    foot.WriteU64(header_.index_offset);  // 与头部冗余，头部损坏时抢救用
    foot.WriteU32(header_crc);
    foot.WriteU32(index_crc_);
    foot.WriteU32(data_crc_);
    foot.WriteZeros(kFooterReservedLen);
    if (footer.size() != kFooterSize) return Fail(L"尾部长度不对", error);

    if (!WriteAll(file_.Get(), footer.data(), footer.size())) {
        return Fail(L"写容器尾部失败", error);
    }
    // 头部是最后回填的：写的时候还不知道各区的大小。
    if (!SeekTo(file_.Get(), 0)) return Fail(L"回填头部时定位失败", error);
    if (!WriteAll(file_.Get(), head.data(), head.size())) {
        return Fail(L"回填头部失败", error);
    }
    if (!FlushFileBuffers(file_.Get())) return Fail(L"刷盘失败", error);

    sink_.reset();
    file_.Reset();
    phase_ = Phase::kDone;
    return Status::kOk;
}

// 关闭并删除半成品，可以重复调用。
//
// 先 reset sink_ 再 reset file_：sink_ 里存着裸 HANDLE，句柄关掉之后
// 它就成了悬垂引用。虽然 Abort 之后没人会再往里写，但顺序反了就是个
// 等着被将来某次改动踩到的地雷。
//
// path_ 清空之后重复调用就是空操作，所以析构里无脑调一次是安全的。
void ArchiveWriter::Abort() {
    sink_.reset();
    file_.Reset();
    if (!path_.empty()) {
        DeleteFileW(platform::ToExtendedPath(path_).c_str());
        path_.clear();
    }
    phase_ = Phase::kClosed;
}

// ============================================================ ArchiveReader

// 打开并校验一个容器。只读明文部分，数据区和索引区按需再流式读。
//
// 校验分三层，一层比一层贵，按顺序做，能早拒绝就早拒绝：
//   1. 文件长度、magic、格式版本 —— 只读 128 字节就能判
//   2. 布局不变式（各区偏移和大小自洽）—— 纯算术，零 IO
//   3. 三个 CRC32 —— 要把整个包读一遍，所以不在 Open 里做，
//      留给 Verify()，也就是 cbk verify 命令
//
// 这个分层的意义是：还原一个大包时，结构性损坏在毫秒级就被拒绝，
// 不用先花几分钟把整个包读一遍算 CRC。
Status ArchiveReader::Open(const std::wstring& path, std::wstring* error) {
    const auto fail = [error](const std::wstring& what) {
        if (error != nullptr) *error = what;
        return Status::kFailed;
    };

    platform::ScopedHandle file = platform::OpenForRead(path, true);
    if (!file.IsValid()) {
        return fail(L"无法打开 " + path + L"：" + platform::FormatWinError(GetLastError()));
    }

    uint64_t total_size = 0;
    if (!FileSize(file.Get(), &total_size)) return fail(L"读不到文件大小");
    if (total_size < kFileHeaderSize + kFooterSize) {
        return fail(L"文件太小，连头部加尾部都不够，不是一个 .cbk 容器");
    }

    // ---- 头部 ----
    std::vector<uint8_t> head(kFileHeaderSize);
    {
        DWORD got = 0;
        if (!ReadFile(file.Get(), head.data(), static_cast<DWORD>(head.size()), &got, nullptr) ||
            got != head.size()) {
            return fail(L"读头部失败");
        }
    }

    ByteReader reader(head);
    char magic[4] = {};
    reader.ReadBytes(magic, sizeof(magic));
    if (std::memcmp(magic, kMagic, sizeof(kMagic)) != 0) {
        return fail(L"magic 不是 CBKF，这不是一个 .cbk 容器");
    }

    ArchiveHeader parsed;
    parsed.format_version = reader.ReadU16();
    parsed.flags = reader.ReadU16();
    const uint32_t header_size = reader.ReadU32();
    const uint32_t pipeline_len = reader.ReadU32();
    parsed.created_at = reader.ReadU64();
    parsed.entry_count = reader.ReadU64();
    parsed.data_offset = reader.ReadU64();
    parsed.data_size = reader.ReadU64();
    parsed.index_offset = reader.ReadU64();
    parsed.index_size = reader.ReadU64();
    parsed.total_original_bytes = reader.ReadU64();
    const uint16_t source_root_len = reader.ReadU16();
    if (!reader.IsOk()) return fail(L"头部字段读取越界");

    if (parsed.format_version != kFormatVersion) {
        return fail(L"容器格式版本是 " + std::to_wstring(parsed.format_version) + L"，本程序只认 " +
                    std::to_wstring(kFormatVersion));
    }
    if (header_size != kFileHeaderSize) return fail(L"头部长度字段不是 128");

    // 布局不变式。对不上说明头部损坏，或者是别的工具生成的文件。
    // 这是白捡的一道完整性检查，比等到解数据区才发现好得多。
    const uint64_t expected_data_offset =
        kFileHeaderSize + static_cast<uint64_t>(pipeline_len) + source_root_len;
    if (parsed.data_offset != expected_data_offset) {
        return fail(L"头部自相矛盾：dataOffset 与 128 + pipelineDescLen + sourceRootLen 对不上");
    }
    if (parsed.index_offset != parsed.data_offset + parsed.data_size) {
        return fail(L"头部自相矛盾：indexOffset 不等于 dataOffset + dataSize");
    }
    if (parsed.index_offset + parsed.index_size + kFooterSize != total_size) {
        return fail(L"文件长度与头部记录的各区大小对不上，包可能被截断");
    }

    // ---- PipelineDesc 与源根 ----
    std::vector<uint8_t> prologue(static_cast<size_t>(pipeline_len) + source_root_len);
    if (!prologue.empty()) {
        DWORD got = 0;
        if (!ReadFile(file.Get(), prologue.data(), static_cast<DWORD>(prologue.size()), &got,
                      nullptr) ||
            got != prologue.size()) {
            return fail(L"读 PipelineDesc 或源根路径失败");
        }
    }
    const std::string pipeline_text(reinterpret_cast<const char*>(prologue.data()), pipeline_len);
    if (!PipelineDesc::Parse(pipeline_text, &parsed.pipeline)) {
        return fail(L"PipelineDesc 解析失败：" + FromUtf8(pipeline_text));
    }
    parsed.source_root = FromUtf8(std::string(
        reinterpret_cast<const char*>(prologue.data()) + pipeline_len, source_root_len));

    // ---- 尾部 ----
    if (!SeekTo(file.Get(), total_size - kFooterSize)) return fail(L"定位尾部失败");
    std::vector<uint8_t> footer(kFooterSize);
    {
        DWORD got = 0;
        if (!ReadFile(file.Get(), footer.data(), static_cast<DWORD>(footer.size()), &got,
                      nullptr) ||
            got != footer.size()) {
            return fail(L"读尾部失败");
        }
    }
    ByteReader foot(footer);
    char footer_magic[4] = {};
    foot.ReadBytes(footer_magic, sizeof(footer_magic));
    if (std::memcmp(footer_magic, kFooterMagic, sizeof(kFooterMagic)) != 0) {
        return fail(L"尾部 magic 不是 CBKE，包不完整");
    }
    const uint64_t footer_index_offset = foot.ReadU64();
    header_crc_ = foot.ReadU32();
    index_crc_ = foot.ReadU32();
    data_crc_ = foot.ReadU32();
    if (!foot.IsOk()) return fail(L"尾部字段读取越界");
    if (footer_index_offset != parsed.index_offset) {
        return fail(L"头尾记录的 indexOffset 不一致");
    }

    path_ = path;
    header_ = std::move(parsed);
    return Status::kOk;
}

std::unique_ptr<ISource> ArchiveReader::OpenData() const {
    return OpenRange(path_, header_.data_offset, header_.data_size);
}

std::unique_ptr<ISource> ArchiveReader::OpenIndex() const {
    return OpenRange(path_, header_.index_offset, header_.index_size);
}

Status ArchiveReader::Verify(std::wstring* error) const {
    const auto fail = [error](const std::wstring& what) {
        if (error != nullptr) *error = what;
        return Status::kFailed;
    };

    // 头部的 CRC 是对那 128 字节整体算的，重新读一遍原样比对。
    platform::ScopedHandle file = platform::OpenForRead(path_, true);
    if (!file.IsValid()) return fail(L"重新打开容器失败");
    std::vector<uint8_t> head(kFileHeaderSize);
    DWORD got = 0;
    if (!ReadFile(file.Get(), head.data(), static_cast<DWORD>(head.size()), &got, nullptr) ||
        got != head.size()) {
        return fail(L"重读头部失败");
    }
    if (ComputeCrc32(head.data(), head.size()) != header_crc_) {
        return fail(L"头部校验和不符，容器已损坏");
    }

    std::unique_ptr<ISource> data = OpenData();
    if (data == nullptr) return fail(L"打开数据区失败");
    if (DrainCrc(*data) != data_crc_) return fail(L"数据区校验和不符，容器已损坏");

    std::unique_ptr<ISource> index = OpenIndex();
    if (index == nullptr) return fail(L"打开索引区失败");
    if (DrainCrc(*index) != index_crc_) return fail(L"索引区校验和不符，容器已损坏");

    return Status::kOk;
}

}  // namespace cbk
