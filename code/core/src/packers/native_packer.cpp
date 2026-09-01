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

// 走一遍 ByteWriter 而不是自己拆四个字节。
// 小端拼装只该有一处实现——这里手写一遍的话，将来 ByteWriter 改了字节序
// 而这里没跟上，产出的包就自相矛盾了，而且极难查。
void WriteU32To(ISink& out, uint32_t value) {
    std::vector<uint8_t> bytes;
    bytes.reserve(4);
    ByteWriter writer(&bytes);
    writer.WriteU32(value);
    out.Write(bytes.data(), bytes.size());
}

// 容器损坏属于不可恢复的失败，抛异常由 CLI 层转成退出码 2。
//
// 不能返回错误码：Unpack 的两个回调都没有返回值，解析出错时没有别的
// 办法通知调用方停下来——继续读下去只会把垃圾数据当成条目吐出去。
[[noreturn]] void Corrupt(const char* what) {
    throw std::runtime_error(std::string("cbk-native 解包失败：") + what);
}

}  // namespace

void NativePacker::BeginEntry(const EntryMeta& meta, ISink& out) {
    WriteEntryMeta(meta, out);
}

// 大块输入要拆成多个不超过 kMaxNativeChunkSize 的块。
//
// 引擎按 64 KB 喂，正常永远走不到这个分支。但 IPacker 的契约没规定
// 上限，别的调用方（比如测试）完全可能一次塞进来几百 MB，而块长度
// 字段只有 32 位——不拆的话长度会被截断，解包时直接错位。
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
