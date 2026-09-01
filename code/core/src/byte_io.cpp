// Copyright 2026 CBK Project. 小端字节序读写的实现。
#include "src/byte_io.h"

#include <cstring>
#include <string>

namespace cbk {

// ------------------------------------------------------------------ ByteWriter

void ByteWriter::WriteU8(uint8_t value) {
    out_->push_back(value);
}

// 每个宽度都手动逐字节拆，不用 memcpy 整个整数。
// memcpy 写出去的是本机字节序，在小端机器上碰巧是对的，换到大端机器
// 生成的包就读不了了。容器格式规定小端，就得显式按小端拼。
void ByteWriter::WriteU16(uint16_t value) {
    out_->push_back(static_cast<uint8_t>(value & 0xFFu));
    out_->push_back(static_cast<uint8_t>((value >> 8) & 0xFFu));
}

void ByteWriter::WriteU32(uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out_->push_back(static_cast<uint8_t>((value >> shift) & 0xFFu));
    }
}

void ByteWriter::WriteU64(uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        out_->push_back(static_cast<uint8_t>((value >> shift) & 0xFFu));
    }
}

void ByteWriter::WriteBytes(const void* data, size_t len) {
    if (len == 0) return;
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    out_->insert(out_->end(), bytes, bytes + len);
}

// 长度前缀而不是 NUL 结尾。路径转成 UTF-8 之后理论上不含 NUL，
// 但格式的健壮性不该建立在"理论上"三个字上——长度前缀对内容零假设。
void ByteWriter::WriteLengthPrefixed(const std::string& bytes) {
    WriteU32(static_cast<uint32_t>(bytes.size()));
    WriteBytes(bytes.data(), bytes.size());
}

void ByteWriter::WriteZeros(size_t count) {
    out_->insert(out_->end(), count, uint8_t{0});
}

// ------------------------------------------------------------------ ByteReader

// 边界检查的唯一入口。
//
// 两个要点：
//   · 一旦置了错误位就永远返回 false，后续读取全部变成空操作。这样调用方
//     可以连读十几个字段最后只查一次 IsOk()，不用每行都判断。
//   · 用 len_ - offset_ 而不是 offset_ + need > len_。后者在 need 极大时
//     会整数溢出绕回去，把越界读判成合法——而 need 恰恰来自文件里的
//     长度字段，正是攻击者/损坏数据最容易做手脚的地方。
bool ByteReader::Require(size_t need) {
    if (!ok_) return false;
    if (len_ - offset_ < need) {
        ok_ = false;
        return false;
    }
    return true;
}

uint8_t ByteReader::ReadU8() {
    if (!Require(1)) return 0;
    return data_[offset_++];
}

uint16_t ByteReader::ReadU16() {
    if (!Require(2)) return 0;
    const uint16_t value =
        static_cast<uint16_t>(data_[offset_]) | (static_cast<uint16_t>(data_[offset_ + 1]) << 8);
    offset_ += 2;
    return value;
}

uint32_t ByteReader::ReadU32() {
    if (!Require(4)) return 0;
    uint32_t value = 0;
    for (int i = 3; i >= 0; --i) {
        value = (value << 8) | data_[offset_ + static_cast<size_t>(i)];
    }
    offset_ += 4;
    return value;
}

uint64_t ByteReader::ReadU64() {
    if (!Require(8)) return 0;
    uint64_t value = 0;
    for (int i = 7; i >= 0; --i) {
        value = (value << 8) | data_[offset_ + static_cast<size_t>(i)];
    }
    offset_ += 8;
    return value;
}

bool ByteReader::ReadBytes(void* out, size_t len) {
    if (!Require(len)) return false;
    if (len > 0) std::memcpy(out, data_ + offset_, len);
    offset_ += len;
    return true;
}

bool ByteReader::ReadLengthPrefixed(std::string* out) {
    const uint32_t length = ReadU32();
    if (!ok_) return false;
    if (!Require(length)) return false;
    out->assign(reinterpret_cast<const char*>(data_ + offset_), length);
    offset_ += length;
    return true;
}

void ByteReader::Skip(size_t count) {
    if (!Require(count)) return;
    offset_ += count;
}

// ------------------------------------------------------------ VectorSink 等

void VectorSink::Write(const uint8_t* data, size_t len) {
    if (data == nullptr || len == 0) return;
    buffer_.insert(buffer_.end(), data, data + len);
}

size_t MemorySource::Read(uint8_t* buf, size_t len) {
    const size_t available = len_ - offset_;
    const size_t count = (len < available) ? len : available;
    if (count > 0) {
        std::memcpy(buf, data_ + offset_, count);
        offset_ += count;
    }
    return count;
}

bool ReadExact(ISource& source, uint8_t* buffer, size_t len) {
    size_t filled = 0;
    while (filled < len) {
        const size_t got = source.Read(buffer + filled, len - filled);
        if (got == 0) return false;  // 流结束了，但还没读够
        filled += got;
    }
    return true;
}

}  // namespace cbk
