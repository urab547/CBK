// Copyright 2026 CBK Project. 字节流的读写抽象。
#ifndef CBK_SINK_H_
#define CBK_SINK_H_

#include <cstddef>
#include <cstdint>

namespace cbk {

/// 字节输出端。
///
/// 流水线上每一段都往 ISink 写，而不关心下游是文件、是下一个 Stage、
/// 还是测试里的一个内存缓冲。这就是整条流水线能自由串联的原因。
class ISink {
public:
    virtual ~ISink() = default;

    /// 写出 len 字节。实现方必须写完，写不完要抛异常，不允许部分写。
    virtual void Write(const uint8_t* data, size_t len) = 0;
};

/// 字节输入端。
class ISource {
public:
    virtual ~ISource() = default;

    /// 读取至多 len 字节到 buf，返回实际读到的字节数。
    /// 返回 0 表示流已结束。返回值小于 len 不代表结束，调用方要继续读。
    virtual size_t Read(uint8_t* buf, size_t len) = 0;
};

}  // namespace cbk

#endif  // CBK_SINK_H_
