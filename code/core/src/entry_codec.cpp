// Copyright 2026 CBK Project. EntryMeta 编解码的实现。
#include "src/entry_codec.h"

#include <string>
#include <utility>
#include <vector>

#include "src/platform_win.h"

namespace cbk {

namespace {

/// 记录体里定长部分的字节数，用来预留 buffer，写错了也只是多一次扩容。
constexpr size_t kFixedPartSize = 8 + 1 + 4 + 8 * 3 + 8 * 3 + 4 + 1 + 4 + 8;

/// FileType 只占一个字节，读回来时要挡住越界的值。
bool ToFileType(uint8_t raw, FileType* out) {
    if (raw > static_cast<uint8_t>(FileType::kUnsupported)) return false;
    *out = static_cast<FileType>(raw);
    return true;
}

}  // namespace

void WriteEntryMeta(const EntryMeta& meta, ISink& out) {
    std::vector<uint8_t> record;
    record.reserve(kFixedPartSize + meta.relative_path.size() * 3 + meta.sddl.size() + 32);

    ByteWriter body(&record);
    // 定长字段在前，变长字段在后。这样定长部分的偏移是固定的，
    // 将来要加"只读某几个字段"的快路径也方便。
    body.WriteU64(meta.id);
    body.WriteU8(static_cast<uint8_t>(meta.type));
    body.WriteU32(meta.attributes);
    body.WriteU64(meta.creation_time);
    body.WriteU64(meta.last_access_time);
    body.WriteU64(meta.last_write_time);
    body.WriteU64(meta.original_size);
    body.WriteU64(meta.stored_size);
    body.WriteU64(meta.data_offset);
    body.WriteU32(meta.crc32);
    body.WriteU8(meta.link_is_relative ? 1u : 0u);
    body.WriteU32(meta.reparse_tag);
    body.WriteU64(meta.hardlink_ref_id);

    body.WriteLengthPrefixed(ToUtf8(meta.relative_path));
    body.WriteLengthPrefixed(ToUtf8(meta.link_target));
    body.WriteLengthPrefixed(meta.sddl);

    std::vector<uint8_t> framed;
    framed.reserve(record.size() + 4);
    ByteWriter frame(&framed);
    frame.WriteU32(static_cast<uint32_t>(record.size()));
    frame.WriteBytes(record.data(), record.size());

    out.Write(framed.data(), framed.size());
}

bool DecodeEntryMeta(ByteReader* reader, EntryMeta* out) {
    EntryMeta meta;

    meta.id = reader->ReadU64();
    const uint8_t raw_type = reader->ReadU8();
    meta.attributes = reader->ReadU32();
    meta.creation_time = reader->ReadU64();
    meta.last_access_time = reader->ReadU64();
    meta.last_write_time = reader->ReadU64();
    meta.original_size = reader->ReadU64();
    meta.stored_size = reader->ReadU64();
    meta.data_offset = reader->ReadU64();
    meta.crc32 = reader->ReadU32();
    meta.link_is_relative = reader->ReadU8() != 0;
    meta.reparse_tag = reader->ReadU32();
    meta.hardlink_ref_id = reader->ReadU64();

    std::string relative_path;
    std::string link_target;
    std::string sddl;
    if (!reader->ReadLengthPrefixed(&relative_path)) return false;
    if (!reader->ReadLengthPrefixed(&link_target)) return false;
    if (!reader->ReadLengthPrefixed(&sddl)) return false;

    // 前面的定长读取即使越界也只是返回 0，所以要在这里统一查一次。
    if (!reader->IsOk()) return false;
    if (!ToFileType(raw_type, &meta.type)) return false;

    meta.relative_path = FromUtf8(relative_path);
    meta.link_target = FromUtf8(link_target);
    meta.sddl = sddl;

    *out = std::move(meta);
    return true;
}

bool ReadEntryMeta(ISource& source, EntryMeta* out, bool* end_of_stream) {
    *end_of_stream = false;

    uint8_t length_bytes[4] = {};
    // 长度前缀一个字节都读不到，说明流干净地结束了——这是正常情况。
    const size_t first = source.Read(length_bytes, 1);
    if (first == 0) {
        *end_of_stream = true;
        return false;
    }
    if (!ReadExact(source, length_bytes + 1, 3)) return false;

    ByteReader length_reader(length_bytes, sizeof(length_bytes));
    const uint32_t record_size = length_reader.ReadU32();
    if (record_size == 0 || record_size > kMaxEntryRecordSize) return false;

    std::vector<uint8_t> record(record_size);
    if (!ReadExact(source, record.data(), record.size())) return false;

    ByteReader body(record);
    return DecodeEntryMeta(&body, out);
}

}  // namespace cbk
