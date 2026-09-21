// Copyright 2026 CBK Project. 按 newc 字段布局自行编解码。
#include "src/packers/cpio_packer.h"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include "cbk/text.h"
#include "src/byte_io.h"
#include "src/entry_codec.h"

namespace cbk {
namespace {

constexpr size_t kHeaderSize = 110;
constexpr size_t kMaxNameSize = 128 * 1024;
constexpr uint32_t kMaxField = std::numeric_limits<uint32_t>::max();
constexpr uint32_t kRegular = 0100000;
constexpr uint32_t kDirectory = 0040000;
constexpr uint32_t kSymlink = 0120000;
constexpr uint32_t kTypeMask = 0170000;
constexpr uint64_t kEpoch = 116444736000000000ull;
constexpr uint64_t kTicks = 10000000;
constexpr char kMetaPrefix[] = ".__cbk_metadata__/";
using Header = std::array<uint8_t, kHeaderSize>;
using Fields = std::array<uint32_t, 13>;
using Identity = std::tuple<uint32_t, uint32_t, uint32_t>;

[[noreturn]] void Invalid(const char* reason) {
    throw std::runtime_error(std::string("cpio: ") + reason);
}

std::string Slashes(const std::wstring& text) {
    std::string result = ToUtf8(text);
    std::replace(result.begin(), result.end(), '\\', '/');
    return result;
}

std::wstring Utf8(const std::string& text) {
    const std::wstring wide = FromUtf8(text);
    if (ToUtf8(wide) != text || text.find('\0') != std::string::npos) {
        Invalid("非法 UTF-8 或嵌入 NUL");
    }
    return wide;
}

bool IsMetadataPath(const std::string& path) {
    std::string first = path.substr(0, path.find('/'));
    for (char& c : first) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return first == ".__cbk_metadata__";
}

// 消除外部归档常见的 ./ 前缀，拒绝会越出还原根目录的路径。
std::string NormalizeName(std::string name) {
    while (name.compare(0, 2, "./") == 0) name.erase(0, 2);
    while (name.size() > 1 && name.back() == '/') name.pop_back();
    if (name.empty() || name.front() == '/' || name.find(':') != std::string::npos ||
        name.find('\\') != std::string::npos) Invalid("条目名必须是相对路径");
    Utf8(name);
    size_t start = 0;
    while (start < name.size()) {
        const size_t end = name.find('/', start);
        const std::string part = name.substr(start, end - start);
        if (part.empty() || part == "." || part == "..") Invalid("条目名包含非法路径分量");
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return name;
}

void Pad(ISink& out, uint64_t size) {
    const uint8_t zeros[4] = {};
    const size_t padding = static_cast<size_t>((4 - size % 4) % 4);
    if (padding != 0) out.Write(zeros, padding);
}

void ReadPadding(ISource& src, uint64_t size) {
    uint8_t zeros[4] = {};
    const size_t padding = static_cast<size_t>((4 - size % 4) % 4);
    if (!ReadExact(src, zeros, padding)) Invalid("填充被截断");
    if (!std::all_of(zeros, zeros + padding, [](uint8_t b) { return b == 0; })) {
        Invalid("填充不是零");
    }
}

// newc 每个数字是固定 8 字节 ASCII 十六进制，不依赖宿主字节序。
void WriteHeader(ISink& out, const std::string& name, uint32_t inode, uint32_t mode,
                 uint32_t links, uint32_t size, uint32_t mtime, uint32_t device = 0) {
    if (name.size() + 1 > kMaxNameSize) Invalid("条目名过长");
    const Fields fields{inode, mode, 0, 0, links, mtime, size, device, 0, 0, 0,
                        static_cast<uint32_t>(name.size() + 1), 0};
    Header head{};
    std::copy_n("070701", 6, head.begin());
    static const char kHex[] = "0123456789abcdef";
    for (size_t i = 0; i < fields.size(); ++i) {
        for (size_t digit = 0; digit < 8; ++digit) {
            head[6 + i * 8 + digit] = kHex[(fields[i] >> ((7 - digit) * 4)) & 15];
        }
    }
    out.Write(head.data(), head.size());
    out.Write(reinterpret_cast<const uint8_t*>(name.c_str()), name.size() + 1);
    Pad(out, head.size() + name.size() + 1);
}

Fields ReadHeader(ISource& src) {
    Header head{};
    if (!ReadExact(src, head.data(), head.size())) Invalid("头部或 TRAILER!!! 被截断");
    if (!std::equal(head.begin(), head.begin() + 6, "070701")) Invalid("仅支持 newc 070701");
    Fields fields{};
    for (size_t i = 0; i < fields.size(); ++i) {
        for (size_t digit = 0; digit < 8; ++digit) {
            const uint8_t c = head[6 + i * 8 + digit];
            uint32_t value = 0;
            if (c >= '0' && c <= '9') value = c - '0';
            else if (c >= 'a' && c <= 'f') value = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') value = c - 'A' + 10;
            else Invalid("头部包含非十六进制字符");
            fields[i] = (fields[i] << 4) | value;
        }
    }
    if (fields[12] != 0) Invalid("newc 的 check 字段必须为零");
    return fields;
}

std::string ReadName(ISource& src, uint32_t size) {
    if (size < 2 || size > kMaxNameSize) Invalid("名字长度超出限制");
    std::vector<uint8_t> bytes(size);
    if (!ReadExact(src, bytes.data(), bytes.size()) || bytes.back() != 0) {
        Invalid("名字被截断或缺少 NUL");
    }
    ReadPadding(src, kHeaderSize + size);
    // newc 允许名字尾部包含多个 NUL，非尾部的 NUL 后不能再跟非零字节。
    const auto end = std::find(bytes.begin(), bytes.end(), uint8_t{0});
    if (!std::all_of(end, bytes.end(), [](uint8_t b) { return b == 0; })) {
        Invalid("名字包含嵌入 NUL");
    }
    return std::string(bytes.begin(), end);
}

std::vector<uint8_t> ReadSmall(ISource& src, uint32_t size, size_t limit) {
    if (size > limit) Invalid("元数据或链接内容超过限制");
    std::vector<uint8_t> bytes(size);
    if (!ReadExact(src, bytes.data(), bytes.size())) Invalid("元数据或链接内容被截断");
    ReadPadding(src, size);
    return bytes;
}

uint32_t Mode(FileType type) {
    switch (type) {
        case FileType::kRegular:
        case FileType::kHardlinkRef:
        case FileType::kUnsupported: return kRegular | 0644;
        case FileType::kDirectory: return kDirectory | 0755;
        case FileType::kSymlinkFile:
        case FileType::kSymlinkDir:
        case FileType::kJunction: return kSymlink | 0777;
    }
    Invalid("未知条目类型");
}

EntryMeta DecodeMetadata(const std::vector<uint8_t>& bytes) {
    ByteReader frame(bytes);
    const uint32_t size = frame.ReadU32();
    if (!frame.IsOk() || size != frame.Remaining()) Invalid("元数据长度不符");
    ByteReader body(bytes.data() + 4, size);
    EntryMeta meta;
    if (!DecodeEntryMeta(&body, &meta) || body.Remaining() != 0) Invalid("元数据损坏");
    return meta;
}

}  // namespace

void CpioPacker::BeginEntry(const EntryMeta& meta, ISink& out) {
    if (in_entry_ || finished_) Invalid("BeginEntry 调用顺序错误");
    const std::string name = NormalizeName(Slashes(meta.relative_path));
    if (IsMetadataPath(name)) Invalid(".__cbk_metadata__ 是保留命名空间");
    if (name.size() + 3 > kMaxNameSize || sequence_ >= kMaxField) Invalid("名字或条目数过大");
    if (ids_.count(meta.id) != 0) Invalid("重复的条目 id");
    const uint32_t mode = Mode(meta.type);
    uint32_t inode = static_cast<uint32_t>(sequence_ + 1);
    const std::string link = Slashes(meta.link_target);
    uint64_t size = meta.type == FileType::kRegular ? meta.original_size : 0;
    if ((mode & kTypeMask) == kSymlink) {
        if (link.empty() || link.size() > kMaxNameSize) Invalid("链接目标为空或过长");
        Utf8(link);
        size = link.size();
    }
    if (size > kMaxField) Invalid("newc 单文件最大为 4294967295 字节，请选择 tar");
    if (meta.type == FileType::kHardlinkRef) {
        const auto target = regular_inodes_.find(meta.hardlink_ref_id);
        if (target == regular_inodes_.end()) Invalid("硬链接目标尚未写入");
        inode = target->second;
    }
    VectorSink metadata;
    WriteEntryMeta(meta, metadata);
    if (metadata.Buffer().size() > kMaxEntryRecordSize + 4) Invalid("元数据过大");
    const std::string meta_name = std::string(kMetaPrefix) + std::to_string(sequence_) + ".bin";
    WriteHeader(out, meta_name, static_cast<uint32_t>(sequence_ + 1), kRegular | 0600, 1,
                 static_cast<uint32_t>(metadata.Buffer().size()), 0, 1);
    out.Write(metadata.Buffer().data(), metadata.Buffer().size());
    Pad(out, metadata.Buffer().size());
    const uint64_t seconds = meta.last_write_time >= kEpoch
                                 ? (meta.last_write_time - kEpoch) / kTicks : 0;
    // 接口没有最终链接总数，且 ISink 不可回填。用上限保持 inode 候选组，
    // 避免普通解包器在第二个名字后丢弃映射，第三个名字无法链接。
    // 此值是候选组上限，不是归档中的实际名字数；实际链接由相同 inode 决定。
    const bool linkable = meta.type == FileType::kRegular || meta.type == FileType::kHardlinkRef;
    const uint32_t mtime = seconds <= kMaxField ? static_cast<uint32_t>(seconds) : 0;
    WriteHeader(out, "./" + name, inode, mode, linkable ? kMaxField : 1,
                 static_cast<uint32_t>(size), mtime);
    if ((mode & kTypeMask) == kSymlink) {
        out.Write(reinterpret_cast<const uint8_t*>(link.data()), link.size());
    }
    ids_.insert(meta.id);
    if (meta.type == FileType::kRegular) regular_inodes_[meta.id] = inode;
    ++sequence_;
    expected_ = meta.type == FileType::kRegular ? size : 0;
    written_ = 0;
    wire_size_ = static_cast<uint32_t>(size);
    in_entry_ = true;
}

void CpioPacker::WriteData(const uint8_t* data, size_t len, ISink& out) {
    if (!in_entry_ || finished_) Invalid("WriteData 调用顺序错误");
    if (len > expected_ - written_) Invalid("内容超过声明长度");
    if (len == 0) return;
    if (data == nullptr) Invalid("非空内容使用空指针");
    out.Write(data, len);
    written_ += len;
}

void CpioPacker::EndEntry(ISink& out) {
    if (!in_entry_ || finished_) Invalid("EndEntry 调用顺序错误");
    if (written_ != expected_) Invalid("文件变小或读取失败，内容不足声明长度");
    Pad(out, wire_size_);
    in_entry_ = false;
}

void CpioPacker::Finish(ISink& out) {
    if (finished_) return;
    if (in_entry_) Invalid("条目尚未结束");
    WriteHeader(out, "TRAILER!!!", 0, 0, 1, 0, 0);
    finished_ = true;
}

void CpioPacker::Unpack(ISource& src, const std::function<void(const EntryMeta&)>& on_entry,
                        const std::function<void(const uint8_t*, size_t)>& on_data) {
    std::map<Identity, uint64_t> files;
    std::set<uint64_t> ids;
    EntryMeta pending;
    bool has_metadata = false;
    uint64_t sequence = 0;
    std::vector<uint8_t> buffer(kIoBlockSize);
    for (;;) {
        const Fields fields = ReadHeader(src);
        const std::string raw_name = ReadName(src, fields[11]);
        const uint32_t size = fields[6];
        const uint32_t type = fields[1] & kTypeMask;
        if (raw_name == "TRAILER!!!") {
            if (size != 0 || has_metadata) Invalid("结束标记非法或元数据缺少实际条目");
            size_t got = 0;
            while ((got = src.Read(buffer.data(), buffer.size())) != 0) {
                if (!std::all_of(buffer.begin(), buffer.begin() + got,
                                 [](uint8_t b) { return b == 0; })) Invalid("结束标记后有非零数据");
            }
            return;
        }
        const std::string name = NormalizeName(raw_name);
        if (IsMetadataPath(name)) {
            const std::string suffix = std::to_string(sequence) + ".bin";
            const std::string expected = std::string(kMetaPrefix) + suffix;
            if (has_metadata || raw_name != expected || type != kRegular || fields[4] != 1 ||
                fields[7] != 1 || fields[8] != 0) Invalid("元数据记录标识或顺序错误");
            pending = DecodeMetadata(ReadSmall(src, size, kMaxEntryRecordSize + 4));
            has_metadata = true;
            continue;
        }
        if (fields[4] == 0) Invalid("链接计数不能为零");
        if (type != kRegular && type != kDirectory && type != kSymlink) Invalid("不支持的文件类型");
        if (type == kDirectory && size != 0) Invalid("目录带有内容");
        std::string link;
        if (type == kSymlink) {
            if (size == 0) Invalid("链接目标为空");
            const auto bytes = ReadSmall(src, size, kMaxNameSize);
            link.assign(bytes.begin(), bytes.end());
            Utf8(link);
        }
        const Identity identity(fields[7], fields[8], fields[0]);
        const auto target = files.find(identity);
        const bool hardlink = type == kRegular && fields[4] > 1 && target != files.end();
        // 数据在最后一个链接的外部归档需要延迟输出；本实现明确拒绝，
        // 避免先恢复空文件后又把真正的数据误丢弃。
        if (hardlink && size != 0) Invalid("仅支持首个名字携带数据的硬链接组");
        EntryMeta meta;
        if (has_metadata) {
            meta = pending;
            if (NormalizeName(Slashes(meta.relative_path)) != name ||
                (Mode(meta.type) & kTypeMask) != type ||
                (meta.type == FileType::kRegular && (meta.original_size != size || hardlink)) ||
                (meta.type == FileType::kHardlinkRef &&
                 (!hardlink || meta.hardlink_ref_id != target->second)) ||
                (meta.type == FileType::kUnsupported && (size != 0 || hardlink)) ||
                (type == kSymlink && Slashes(meta.link_target) != link)) {
                Invalid("元数据与 newc 字段不一致");
            }
        } else {
            meta.id = sequence;
            meta.relative_path = Utf8(name);
            std::replace(meta.relative_path.begin(), meta.relative_path.end(), L'/', L'\\');
            meta.last_write_time = kEpoch + static_cast<uint64_t>(fields[5]) * kTicks;
            if (type == kDirectory) meta.type = FileType::kDirectory;
            else if (type == kSymlink) {
                meta.type = FileType::kSymlinkFile;
                meta.link_target = Utf8(link);
                meta.link_is_relative = link.front() != '/' && link.find(':') == std::string::npos;
            } else if (hardlink) {
                meta.type = FileType::kHardlinkRef;
                meta.hardlink_ref_id = target->second;
            } else meta.original_size = size;
        }
        if (!ids.insert(meta.id).second) Invalid("重复的条目 id");
        on_entry(meta);
        if (type == kRegular) {
            uint64_t remaining = size;
            while (remaining != 0) {
                const uint64_t count = std::min<uint64_t>(remaining, buffer.size());
                const size_t chunk = static_cast<size_t>(count);
                if (!ReadExact(src, buffer.data(), chunk)) Invalid("文件内容被截断");
                on_data(buffer.data(), chunk);
                remaining -= chunk;
            }
            ReadPadding(src, size);
            if (meta.type == FileType::kRegular && fields[4] > 1) files[identity] = meta.id;
        }
        ++sequence;
        has_metadata = false;
    }
}

}  // namespace cbk
