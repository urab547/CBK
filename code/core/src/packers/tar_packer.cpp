// Copyright 2026 CBK Project. 按 ustar/PAX 格式手工编解码，不调用第三方打包库。
#include "src/packers/tar_packer.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "cbk/text.h"
#include "src/byte_io.h"
#include "src/entry_codec.h"

namespace cbk {
namespace {

constexpr size_t kBlock = 512;
constexpr uint64_t kMaxOctalSize = 077777777777ull;
constexpr uint64_t kEpoch = 116444736000000000ull;
constexpr uint64_t kTicks = 10000000;
constexpr size_t kMaxPax = 4 * 1024 * 1024;
using Header = std::array<uint8_t, kBlock>;
using Pax = std::map<std::string, std::string>;

[[noreturn]] void Invalid(const char* reason) {
    throw std::runtime_error(std::string("tar: ") + reason);
}

std::string Slashes(std::wstring path) {
    std::replace(path.begin(), path.end(), L'\\', L'/');
    return ToUtf8(path);
}

// 条目名必须是相对路径。链接目标可以是绝对路径，因此不调用此检查。
void CheckPath(const std::string& path) {
    if (path.empty() || path.front() == '/' || path.find(':') != std::string::npos ||
        path.find('\0') != std::string::npos || path.find('\\') != std::string::npos) {
        Invalid("条目路径必须是非空相对路径");
    }
    size_t start = 0;
    while (start < path.size()) {
        const size_t end = path.find('/', start);
        const std::string part = path.substr(start, end - start);
        if (part.empty() || part == "..") Invalid("条目路径包含空分量或上级目录");
        if (end == std::string::npos) break;
        start = end + 1;
    }
}

void PutText(Header* head, size_t offset, size_t width, const std::string& text) {
    if (text.size() > width || text.find('\0') != std::string::npos) Invalid("头字段过长");
    std::copy(text.begin(), text.end(), head->begin() + offset);
}

std::string GetText(const Header& head, size_t offset, size_t width) {
    size_t count = 0;
    while (count < width && head[offset + count] != 0) ++count;
    return std::string(reinterpret_cast<const char*>(head.data() + offset), count);
}

void PutOctal(Header* head, size_t offset, size_t width, uint64_t value) {
    (*head)[offset + width - 1] = 0;
    for (size_t i = width - 1; i > 0; --i) {
        (*head)[offset + i - 1] = static_cast<uint8_t>('0' + (value & 7));
        value >>= 3;
    }
    if (value != 0) Invalid("八进制字段溢出");
}

uint64_t GetOctal(const Header& head, size_t offset, size_t width) {
    uint64_t value = 0;
    bool digits = false;
    bool ended = false;
    for (size_t i = offset; i < offset + width; ++i) {
        const uint8_t c = head[i];
        if (c == 0 || c == ' ') {
            if (digits || c == 0) ended = true;
        } else {
            if (ended || c < '0' || c > '7') Invalid("非法八进制字段");
            value = value * 8 + c - '0';
            digits = true;
        }
    }
    return value;
}

uint64_t Decimal(const std::string& text) {
    if (text.empty()) Invalid("空十进制字段");
    uint64_t value = 0;
    for (char c : text) {
        if (c < '0' || c > '9' ||
            value > (std::numeric_limits<uint64_t>::max() - (c - '0')) / 10) {
            Invalid("非法或溢出的十进制字段");
        }
        value = value * 10 + c - '0';
    }
    return value;
}

uint64_t Checksum(const Header& head) {
    uint64_t sum = 0;
    for (size_t i = 0; i < head.size(); ++i) sum += (i >= 148 && i < 156) ? ' ' : head[i];
    return sum;
}

Header MakeHeader(const std::string& name, char type, const std::string& link, uint64_t size,
                  uint64_t mtime) {
    Header head{};
    PutText(&head, 0, 100, name);
    PutOctal(&head, 100, 8, type == '5' ? 0755 : 0644);
    PutOctal(&head, 108, 8, 0);  // Windows SID 不能转换为 Unix uid/gid，原值存 PAX。
    PutOctal(&head, 116, 8, 0);
    PutOctal(&head, 124, 12, size);
    PutOctal(&head, 136, 12, mtime);
    head[156] = static_cast<uint8_t>(type);
    PutText(&head, 157, 100, link);
    PutText(&head, 257, 6, "ustar");
    PutText(&head, 263, 2, "00");
    PutOctal(&head, 148, 7, Checksum(head));
    head[155] = ' ';
    return head;
}

void Pad(ISink& out, uint64_t size) {
    const Header zeros{};
    const size_t padding = static_cast<size_t>((kBlock - size % kBlock) % kBlock);
    if (padding != 0) out.Write(zeros.data(), padding);
}

void ReadPadding(ISource& src, uint64_t size) {
    Header padding{};
    const size_t count = static_cast<size_t>((kBlock - size % kBlock) % kBlock);
    if (!ReadExact(src, padding.data(), count)) Invalid("填充块被截断");
}

// PAX 的长度包含十进制长度自身；跨越 99/100 等位数边界时需再算一轮。
void AddPax(std::string* pax, const std::string& key, const std::string& value) {
    const std::string body = " " + key + "=" + value + "\n";
    size_t length = body.size() + 1;
    while (length != body.size() + std::to_string(length).size()) {
        length = body.size() + std::to_string(length).size();
    }
    *pax += std::to_string(length) + body;
}

std::string EncodeMeta(const EntryMeta& meta) {
    VectorSink bytes;
    WriteEntryMeta(meta, bytes);
    if (bytes.Buffer().size() > kMaxEntryRecordSize + 4) Invalid("元数据超出上限");
    static const char kHex[] = "0123456789abcdef";
    std::string text;
    text.reserve(bytes.Buffer().size() * 2);
    for (uint8_t byte : bytes.Buffer()) {
        text.push_back(kHex[byte >> 4]);
        text.push_back(kHex[byte & 15]);
    }
    return text;
}

EntryMeta DecodeMeta(const std::string& text) {
    if (text.size() % 2 != 0 || text.size() / 2 > kMaxEntryRecordSize + 4) {
        Invalid("CBK.meta 长度非法");
    }
    std::vector<uint8_t> bytes;
    bytes.reserve(text.size() / 2);
    const auto digit = [](char c) -> uint8_t {
        if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
        Invalid("CBK.meta 不是十六进制");
    };
    for (size_t i = 0; i < text.size(); i += 2) {
        bytes.push_back(static_cast<uint8_t>((digit(text[i]) << 4) | digit(text[i + 1])));
    }
    ByteReader framed(bytes);
    const uint32_t size = framed.ReadU32();
    if (!framed.IsOk() || size != framed.Remaining()) Invalid("CBK.meta 记录长度不符");
    ByteReader body(bytes.data() + 4, size);
    EntryMeta meta;
    if (!DecodeEntryMeta(&body, &meta) || body.Remaining() != 0) Invalid("CBK.meta 记录损坏");
    return meta;
}

Pax ReadPax(ISource& src, uint64_t size) {
    if (size == 0 || size > kMaxPax) Invalid("PAX 头超出大小限制");
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if (!ReadExact(src, bytes.data(), bytes.size())) Invalid("PAX 头被截断");
    ReadPadding(src, size);
    const std::string text(bytes.begin(), bytes.end());
    Pax pax;
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t space = text.find(' ', pos);
        if (space == std::string::npos) Invalid("PAX 缺少长度分隔符");
        const uint64_t length = Decimal(text.substr(pos, space - pos));
        if (length > text.size() - pos || length <= space - pos + 3) Invalid("PAX 长度非法");
        const size_t end = pos + static_cast<size_t>(length);
        const size_t equal = text.find('=', space + 1);
        if (equal == std::string::npos || equal == space + 1 || equal >= end - 1 ||
            text[end - 1] != '\n') Invalid("PAX 键值记录非法");
        const std::string key = text.substr(space + 1, equal - space - 1);
        // 稀疏文件有另一套内容布局，不能忽略键后误当成普通文件。
        if (key.compare(0, 10, "GNU.sparse") == 0 || key == "SCHILY.filetype" ||
            key == "hdrcharset") Invalid("不支持此 PAX 内容编码扩展");
        if (!pax.emplace(key, text.substr(equal + 1, end - equal - 2)).second) {
            Invalid("重复的 PAX 键");
        }
        pos = end;
    }
    return pax;
}

char TarType(FileType type) {
    switch (type) {
        case FileType::kRegular: return '0';
        case FileType::kDirectory: return '5';
        case FileType::kHardlinkRef: return '1';
        case FileType::kSymlinkFile:
        case FileType::kSymlinkDir:
        case FileType::kJunction: return '2';
        case FileType::kUnsupported: return '0';  // 普通工具只会读出空占位。
    }
    Invalid("未知文件类型");
}

// 没有 CBK 扩展时，只能恢复标准 tar 能表达的字段。
EntryMeta StandardMeta(const Header& head, const std::string& path, const std::string& link,
                       uint64_t size, uint64_t id, const std::map<std::string, uint64_t>& files) {
    EntryMeta meta;
    meta.id = id;
    meta.relative_path = FromUtf8(path);
    std::replace(meta.relative_path.begin(), meta.relative_path.end(), L'/', L'\\');
    const uint64_t seconds = GetOctal(head, 136, 12);
    if (seconds > (std::numeric_limits<uint64_t>::max() - kEpoch) / kTicks) {
        Invalid("时间戳溢出");
    }
    meta.last_write_time = kEpoch + seconds * kTicks;
    switch (head[156]) {
        case 0:
        case '0': meta.original_size = size; break;
        case '5': meta.type = FileType::kDirectory; break;
        case '2':
            meta.type = FileType::kSymlinkFile;
            meta.link_target = FromUtf8(link);
            meta.link_is_relative = !link.empty() && link.front() != '/' &&
                                    link.find(':') == std::string::npos;
            break;
        case '1': {
            const auto target = files.find(link);
            if (target == files.end()) Invalid("硬链接必须指向前面的普通文件");
            meta.type = FileType::kHardlinkRef;
            meta.hardlink_ref_id = target->second;
            break;
        }
        default: Invalid("不支持的 tar 条目类型或格式扩展");
    }
    return meta;
}

}  // namespace

void TarPacker::BeginEntry(const EntryMeta& meta, ISink& out) {
    if (finished_ || in_entry_) Invalid("BeginEntry 调用顺序错误");
    const std::string path = Slashes(meta.relative_path);
    CheckPath(path);
    const char type = TarType(meta.type);
    const uint64_t size = meta.type == FileType::kRegular ? meta.original_size : 0;
    std::string link = Slashes(meta.link_target);
    if (link.find('\0') != std::string::npos) Invalid("链接目标包含 NUL");
    if (type == '1') {
        const auto target = regular_paths_.find(meta.hardlink_ref_id);
        if (target == regular_paths_.end()) Invalid("硬链接目标尚未打包");
        link = target->second;
    }
    if (type == '2' && link.empty()) Invalid("链接目标为空");

    std::string pax;
    AddPax(&pax, "path", path);
    if (!link.empty()) AddPax(&pax, "linkpath", link);
    if (size > kMaxOctalSize) AddPax(&pax, "size", std::to_string(size));
    AddPax(&pax, "CBK.meta", EncodeMeta(meta));
    if (pax.size() > kMaxPax) Invalid("PAX 元数据超出上限");
    const Header extension = MakeHeader("PaxHeaders/entry", 'x', "", pax.size(), 0);
    const uint64_t seconds = meta.last_write_time >= kEpoch
                                 ? (meta.last_write_time - kEpoch) / kTicks : 0;
    const Header head = MakeHeader(path.size() <= 100 ? path : "PaxEntry", type,
                                   link.size() <= 100 ? link : "", size <= kMaxOctalSize ? size : 0,
                                   seconds <= kMaxOctalSize ? seconds : 0);
    out.Write(extension.data(), extension.size());
    out.Write(reinterpret_cast<const uint8_t*>(pax.data()), pax.size());
    Pad(out, pax.size());
    out.Write(head.data(), head.size());
    if (meta.type == FileType::kRegular && !regular_paths_.emplace(meta.id, path).second) {
        Invalid("重复的普通文件 id");
    }
    expected_ = size;
    written_ = 0;
    in_entry_ = true;
}

void TarPacker::WriteData(const uint8_t* data, size_t len, ISink& out) {
    if (!in_entry_ || finished_) Invalid("WriteData 调用顺序错误");
    if (len > expected_ - written_) Invalid("文件变大，内容超过 tar 头声明的长度");
    if (len == 0) return;
    if (data == nullptr) Invalid("非空内容使用了空指针");
    out.Write(data, len);
    written_ += len;
}

void TarPacker::EndEntry(ISink& out) {
    if (!in_entry_ || finished_) Invalid("EndEntry 调用顺序错误");
    if (written_ != expected_) Invalid("文件变小或读取中断，内容不足 tar 头声明的长度");
    Pad(out, written_);
    in_entry_ = false;
}

void TarPacker::Finish(ISink& out) {
    if (finished_) return;
    if (in_entry_) Invalid("条目未结束，不能 Finish");
    const Header zeros{};
    out.Write(zeros.data(), zeros.size());
    out.Write(zeros.data(), zeros.size());
    finished_ = true;
}

void TarPacker::Unpack(ISource& src, const std::function<void(const EntryMeta&)>& on_entry,
                       const std::function<void(const uint8_t*, size_t)>& on_data) {
    Pax pax;
    bool has_pax = false;
    uint64_t next_id = 0;
    std::map<std::string, uint64_t> files;
    std::vector<uint8_t> buffer(kIoBlockSize);
    for (;;) {
        Header head{};
        if (!ReadExact(src, head.data(), head.size())) Invalid("头部或结束标记被截断");
        if (std::all_of(head.begin(), head.end(), [](uint8_t b) { return b == 0; })) {
            if (has_pax) Invalid("PAX 后缺少实际条目");
            if (!ReadExact(src, head.data(), head.size()) ||
                !std::all_of(head.begin(), head.end(), [](uint8_t b) { return b == 0; })) {
                Invalid("缺少第二个结束块");
            }
            // 允许普通 tar 工具为物理记录补的零，但拒绝隐藏的第二份归档。
            size_t got = 0;
            while ((got = src.Read(buffer.data(), buffer.size())) != 0) {
                if (!std::all_of(buffer.begin(), buffer.begin() + got,
                                 [](uint8_t b) { return b == 0; })) Invalid("结束块后有非零数据");
            }
            return;
        }
        if (GetOctal(head, 148, 8) != Checksum(head)) Invalid("头部校验和不符");
        if (GetText(head, 257, 6) != "ustar" || GetText(head, 263, 2) != "00") {
            Invalid("仅支持 ustar/PAX 格式");
        }
        uint64_t size = GetOctal(head, 124, 12);
        if (head[156] == 'x') {
            if (has_pax) Invalid("重复的逐条目 PAX 头");
            pax = ReadPax(src, size);
            has_pax = true;
            continue;
        }
        std::string path = GetText(head, 0, 100);
        const std::string prefix = GetText(head, 345, 155);
        if (!prefix.empty()) path = prefix + "/" + path;
        std::string link = GetText(head, 157, 100);
        if (pax.count("path") != 0) path = pax.at("path");
        if (pax.count("linkpath") != 0) link = pax.at("linkpath");
        if (pax.count("size") != 0) size = Decimal(pax.at("size"));
        CheckPath(path);
        if (link.find('\0') != std::string::npos) Invalid("链接目标包含 NUL");
        EntryMeta meta = StandardMeta(head, path, link, size, next_id, files);
        if (pax.count("CBK.meta") != 0) {
            meta = DecodeMeta(pax.at("CBK.meta"));
            if (Slashes(meta.relative_path) != path ||
                TarType(meta.type) != (head[156] == 0 ? '0' : head[156]) ||
                (meta.type == FileType::kRegular && meta.original_size != size) ||
                (head[156] == '2' && Slashes(meta.link_target) != link) ||
                (head[156] == '1' && meta.hardlink_ref_id != files.at(link))) {
                Invalid("CBK.meta 与 tar 标准字段不一致");
            }
        }
        if (meta.type != FileType::kRegular && size != 0) Invalid("非普通文件包含内容");
        on_entry(meta);
        uint64_t remaining = size;
        while (remaining != 0) {
            const size_t chunk = static_cast<size_t>(std::min<uint64_t>(remaining, buffer.size()));
            if (!ReadExact(src, buffer.data(), chunk)) Invalid("文件内容被截断");
            on_data(buffer.data(), chunk);
            remaining -= chunk;
        }
        ReadPadding(src, size);
        if (meta.type == FileType::kRegular) files[path] = meta.id;
        ++next_id;
        pax.clear();
        has_pax = false;
    }
}

}  // namespace cbk
