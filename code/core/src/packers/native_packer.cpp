// Copyright 2026 CBK Project. 原生打包器的实现。
#include "src/packers/native_packer.h"

#include <stdexcept>
#include <string>
#include <vector>

#include "src/byte_io.h"
#include "src/entry_codec.h"

namespace cbk {

namespace {

/// 内容块的结束标记：一个长度为 0 的块。
constexpr uint32_t kContentEnd = 0;

void WriteU32To(ISink& out, uint32_t value) {
    std::vector<uint8_t> bytes;
    bytes.reserve(4);
    ByteWriter writer(&bytes);
    writer.WriteU32(value);
    out.Write(bytes.data(), bytes.size());
}

[[noreturn]] void Corrupt(const char* what) {
    throw std::runtime_error(std::string("cbk-native 解包失败：") + what);
}

}  // namespace

void NativePacker::BeginEntry(const EntryMeta& meta, ISink& out) {
    WriteEntryMeta(meta, out);
}

void NativePacker::WriteData(const uint8_t* data, size_t len, ISink& out) {
    // 长度为 0 的块是结束标记，不能被当成普通数据写出去。
    if (data == nullptr || len == 0) return;

    size_t done = 0;
    while (done < len) {
        const size_t chunk = (len - done < kMaxNativeChunkSize)
                                 ? (len - done)
                                 : static_cast<size_t>(kMaxNativeChunkSize);
        WriteU32To(out, static_cast<uint32_t>(chunk));
        out.Write(data + done, chunk);
        done += chunk;
    }
}

void NativePacker::EndEntry(ISink& out) {
    WriteU32To(out, kContentEnd);
}

void NativePacker::Finish(ISink&) {
    // 数据区的长度记在容器头里，读到流尾自然就结束了，不需要额外的标记。
}

void NativePacker::Unpack(ISource& src, const std::function<void(const EntryMeta&)>& on_entry,
                          const std::function<void(const uint8_t*, size_t)>& on_data) {
    std::vector<uint8_t> chunk;

    for (;;) {
        EntryMeta entry;
        bool end_of_stream = false;
        if (!ReadEntryMeta(src, &entry, &end_of_stream)) {
            if (end_of_stream) return;  // 干净地读完了
            Corrupt("条目记录不完整或字段非法");
        }
        on_entry(entry);

        for (;;) {
            uint8_t length_bytes[4] = {};
            if (!ReadExact(src, length_bytes, sizeof(length_bytes))) {
                Corrupt("内容块长度读不全，流被截断");
            }
            ByteReader reader(length_bytes, sizeof(length_bytes));
            const uint32_t chunk_size = reader.ReadU32();
            if (chunk_size == kContentEnd) break;
            if (chunk_size > kMaxNativeChunkSize) {
                Corrupt("内容块长度超出上限，字段已损坏");
            }

            chunk.resize(chunk_size);
            if (!ReadExact(src, chunk.data(), chunk.size())) {
                Corrupt("内容块读不全，流被截断");
            }
            on_data(chunk.data(), chunk.size());
        }
    }
}

}  // namespace cbk
