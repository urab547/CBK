// Copyright 2026 CBK Project. CRC32 校验和。
//
// 自己实现，不用 zlib 的 crc32()。core 这一层零第三方依赖是硬约束，
// 而且查表法 CRC32 本来就只有几十行。
//
// 用的是 IEEE 802.3 标准 CRC-32（反射多项式 0xEDB88320，初值全 1，
// 末了再取反）——跟 zip、png、zlib 用的是同一个，所以拿外部工具
// 交叉验证很方便。
#ifndef CODE_CORE_SRC_CRC32_H_
#define CODE_CORE_SRC_CRC32_H_

#include <cstddef>
#include <cstdint>

namespace cbk {

/// 流式 CRC32。内容是分块读进来的，不可能一次性算完，所以要能累积。
class Crc32 {
public:
    /// 喂一块数据。可以调任意多次，结果与一次性喂完整数据相同。
    void Update(const uint8_t* data, size_t len);

    /// 当前校验和。可以中途取，取完还能继续 Update。
    uint32_t Value() const;

    /// 复位，准备算下一个对象。
    void Reset();

private:
    /// 内部保持"未取反"的中间态，Value() 时才取反。
    uint32_t state_ = 0xFFFFFFFFu;
};

/// 一次性算完一段内存的 CRC32。
uint32_t ComputeCrc32(const uint8_t* data, size_t len);

}  // namespace cbk

#endif  // CODE_CORE_SRC_CRC32_H_
