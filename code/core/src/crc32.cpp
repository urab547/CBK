// Copyright 2026 CBK Project. CRC32 的实现。
#include "src/crc32.h"

#include <array>

namespace cbk {

namespace {

/// 反射形式的 CRC-32 生成多项式。
///
/// 0xEDB88320 是 0x04C11DB7 按位反转的结果。用反射形式配合
/// "右移 + 查表"，比按位左移的写法少一次反转，是最常见的实现方式。
constexpr uint32_t kPolynomial = 0xEDB88320u;

/// 编译期把 256 项的查表算出来，运行时零初始化开销。
constexpr std::array<uint32_t, 256> MakeTable() {
    std::array<uint32_t, 256> table = {};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t remainder = i;
        for (int bit = 0; bit < 8; ++bit) {
            remainder = (remainder & 1u) ? (kPolynomial ^ (remainder >> 1)) : (remainder >> 1);
        }
        table[i] = remainder;
    }
    return table;
}

constexpr std::array<uint32_t, 256> kTable = MakeTable();

}  // namespace

void Crc32::Update(const uint8_t* data, size_t len) {
    if (data == nullptr) return;
    uint32_t state = state_;
    for (size_t i = 0; i < len; ++i) {
        state = kTable[(state ^ data[i]) & 0xFFu] ^ (state >> 8);
    }
    state_ = state;
}

uint32_t Crc32::Value() const {
    // 标准要求最后对结果取反。中间态不取反，是为了能继续累积。
    return state_ ^ 0xFFFFFFFFu;
}

void Crc32::Reset() {
    state_ = 0xFFFFFFFFu;
}

uint32_t ComputeCrc32(const uint8_t* data, size_t len) {
    Crc32 crc;
    crc.Update(data, len);
    return crc.Value();
}

}  // namespace cbk
