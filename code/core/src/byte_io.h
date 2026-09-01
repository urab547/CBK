// Copyright 2026 CBK Project. 小端字节序的读写原语。
//
// 容器格式规定一律小端。直接 memcpy 结构体是不行的——那会把编译器的
// 结构体填充和本机字节序一起写进文件，换个编译器或架构就读不了了。
// 所以每个字段都显式按字节拼。
//
// 读的一侧全部做边界检查：容器文件可能被截断、被改坏、或者根本不是
// 我们生成的。读越界要变成"这个包坏了"，而不是崩溃或读到垃圾。
#ifndef CODE_CORE_SRC_BYTE_IO_H_
#define CODE_CORE_SRC_BYTE_IO_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "cbk/sink.h"

namespace cbk {

/// 按小端往一个 vector 尾部追加。
class ByteWriter {
public:
    explicit ByteWriter(std::vector<uint8_t>* out) : out_(out) {}

    void WriteU8(uint8_t value);
    void WriteU16(uint16_t value);
    void WriteU32(uint32_t value);
    void WriteU64(uint64_t value);
    void WriteBytes(const void* data, size_t len);

    /// U32 长度前缀 + 原始字节。空串写成一个 0 长度。
    void WriteLengthPrefixed(const std::string& bytes);

    /// 补 count 个 0，用于定长区块的 reserved 部分。
    void WriteZeros(size_t count);

    size_t Size() const { return out_->size(); }

private:
    std::vector<uint8_t>* out_;
};

/// 按小端从一段只读内存里取值，越界不抛异常，只置错误位。
///
/// 用法：连着读若干个字段，最后查一次 IsOk()。中途越界之后所有读取
/// 都返回 0 且不再前进，所以不必每读一个字段就判断一次。
class ByteReader {
public:
    ByteReader(const uint8_t* data, size_t len) : data_(data), len_(len) {}
    explicit ByteReader(const std::vector<uint8_t>& buffer)
        : data_(buffer.data()), len_(buffer.size()) {}

    uint8_t ReadU8();
    uint16_t ReadU16();
    uint32_t ReadU32();
    uint64_t ReadU64();

    /// 读 len 字节到 out。越界时返回 false 并置错误位。
    bool ReadBytes(void* out, size_t len);

    /// 读 U32 长度前缀 + 内容。
    bool ReadLengthPrefixed(std::string* out);

    /// 跳过 count 字节。
    void Skip(size_t count);

    /// 到目前为止有没有越界过。
    bool IsOk() const { return ok_; }
    size_t Offset() const { return offset_; }
    size_t Remaining() const { return ok_ ? len_ - offset_ : 0; }

private:
    /// 检查还能不能读 need 字节，不能就置错误位。
    bool Require(size_t need);

    const uint8_t* data_;
    size_t len_;
    size_t offset_ = 0;
    bool ok_ = true;
};

// ============================================================================
// 内存里的 ISink / ISource
// ============================================================================

/// 把写进来的字节全收进一个 vector。测试和索引区序列化都要用。
class VectorSink : public ISink {
public:
    void Write(const uint8_t* data, size_t len) override;

    const std::vector<uint8_t>& Buffer() const { return buffer_; }
    std::vector<uint8_t>& Buffer() { return buffer_; }

private:
    std::vector<uint8_t> buffer_;
};

/// 从一段内存里顺序读。不持有内存，调用方要保证生命周期。
class MemorySource : public ISource {
public:
    MemorySource(const uint8_t* data, size_t len) : data_(data), len_(len) {}
    explicit MemorySource(const std::vector<uint8_t>& buffer)
        : data_(buffer.data()), len_(buffer.size()) {}

    size_t Read(uint8_t* buf, size_t len) override;

private:
    const uint8_t* data_;
    size_t len_;
    size_t offset_ = 0;
};

/// 从 source 精确读满 len 字节。
///
/// ISource::Read 的契约是"返回值小于 len 不代表结束"，所以每个需要
/// 定长数据的地方都得自己循环。漏掉这个循环的 bug 在小文件上永远
/// 不会出现，一上大文件就随机解析失败。
///
/// @return 读满返回 true；流提前结束返回 false（此时 buffer 内容不完整）。
bool ReadExact(ISource& source, uint8_t* buffer, size_t len);

}  // namespace cbk

#endif  // CODE_CORE_SRC_BYTE_IO_H_
