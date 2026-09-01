// Copyright 2026 CBK Project. 本项目自带的原生打包器。
//
// 这个打包器的价值是**当基准**，不是当卖点。有它才能在队友的 tar / cpio
// 落地之前把整条流水线跑通、把引擎的端到端测试写出来；真出了问题，
// 也能立刻分清是打包器的锅还是引擎的锅。
//
// 所以格式怎么简单怎么来：
//
//     每个条目：
//       [U32 记录长度][EntryMeta 记录体]        ← 与容器索引区同一套编码
//       [U32 块长度][块内容] × N
//       [U32 0]                                 ← 内容结束标记
//     全部条目写完就是流的结尾，没有额外的结束标记。
//
// 两个刻意的设计：
//
//   · **内容分块自带长度**，不依赖 EntryMeta::original_size。备份期间
//     文件被别人改大改小是常事，如果解包时按 original_size 去读，
//     一旦对不上，后面所有条目全部错位。分块自描述就不会串。
//
//   · **每个条目都写内容段**，哪怕是目录、链接这种一定没内容的。
//     多花 4 个字节，换来打包和解包两边都不需要按类型分支——
//     少一处分支就少一处两边判断不一致导致流错位的可能。
#ifndef CODE_CORE_SRC_PACKERS_NATIVE_PACKER_H_
#define CODE_CORE_SRC_PACKERS_NATIVE_PACKER_H_

#include <cstdint>
#include <functional>
#include <string>

#include "cbk/packer.h"

namespace cbk {

/// 注册名。
inline constexpr char kNativePackerName[] = "cbk-native";

/// 单个内容块的字节数上限，防的是长度字段被改坏后去申请天文数字的内存。
/// 引擎按 kIoBlockSize（64 KB）分块，64 MB 留了三个数量级的余量。
inline constexpr uint32_t kMaxNativeChunkSize = 64u * 1024 * 1024;

class NativePacker : public IPacker {
public:
    std::string Name() const override { return kNativePackerName; }

    void BeginEntry(const EntryMeta& meta, ISink& out) override;
    void WriteData(const uint8_t* data, size_t len, ISink& out) override;
    void EndEntry(ISink& out) override;
    void Finish(ISink& out) override;

    /// 解析失败会抛 std::runtime_error——容器损坏属于不可恢复的失败，
    /// 由 CLI 层捕获转成退出码 2。
    void Unpack(ISource& src, const std::function<void(const EntryMeta&)>& on_entry,
                const std::function<void(const uint8_t*, size_t)>& on_data) override;
};

}  // namespace cbk

#endif  // CODE_CORE_SRC_PACKERS_NATIVE_PACKER_H_
