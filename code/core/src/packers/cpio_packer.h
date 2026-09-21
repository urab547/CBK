// Copyright 2026 CBK Project. SVR4 newc 打包解包。
#ifndef CODE_CORE_SRC_PACKERS_CPIO_PACKER_H_
#define CODE_CORE_SRC_PACKERS_CPIO_PACKER_H_

#include <cstdint>
#include <map>
#include <set>
#include <string>

#include "cbk/packer.h"

namespace cbk {

/// newc（070701），110 字节头，名字和内容分别补齐至 4 字节。
/// 每个实际条目前写一条 .__cbk_metadata__/N.bin 保存完整 EntryMeta。
/// 此命名空间保留；外部工具会看到这些辅助文件，CBK 不向回调暴露它们。
///
/// 内容流式处理。单条数据不得超过 UINT32_MAX，实际输入长度必须等于
/// original_size。newc 不提供回填接口，大小变化时抛 std::runtime_error。
class CpioPacker : public IPacker {
public:
    std::string Name() const override { return "cpio"; }
    void BeginEntry(const EntryMeta& meta, ISink& out) override;
    void WriteData(const uint8_t* data, size_t len, ISink& out) override;
    void EndEntry(ISink& out) override;
    void Finish(ISink& out) override;
    void Unpack(ISource& src, const std::function<void(const EntryMeta&)>& on_entry,
                const std::function<void(const uint8_t*, size_t)>& on_data) override;

private:
    std::map<uint64_t, uint32_t> regular_inodes_;
    std::set<uint64_t> ids_;
    uint64_t sequence_ = 0;
    uint64_t expected_ = 0;
    uint64_t written_ = 0;
    uint32_t wire_size_ = 0;
    bool in_entry_ = false;
    bool finished_ = false;
};

}  // namespace cbk

#endif  // CODE_CORE_SRC_PACKERS_CPIO_PACKER_H_
