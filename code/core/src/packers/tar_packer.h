// Copyright 2026 CBK Project. ustar/PAX 打包器。
#ifndef CODE_CORE_SRC_PACKERS_TAR_PACKER_H_
#define CODE_CORE_SRC_PACKERS_TAR_PACKER_H_

#include <cstdint>
#include <map>
#include <string>

#include "cbk/packer.h"

namespace cbk {

/// 标准 ustar 头 + 每条目的 PAX 扩展，文件内容按 512 字节补齐。
/// PAX 的 CBK.meta 保存完整 EntryMeta，普通 tar 工具可忽略此键读取内容。
///
/// 内容直接转发，不缓存整个文件。tar 必须预先声明文件长度，因此实际
/// WriteData 总量必须等于 original_size；不一致时抛异常，不能生成错位的包。
/// 一个实例写一份归档；Finish 幂等。Unpack 使用独立的局部读取状态。
class TarPacker : public IPacker {
public:
    std::string Name() const override { return "tar"; }
    void BeginEntry(const EntryMeta& meta, ISink& out) override;
    void WriteData(const uint8_t* data, size_t len, ISink& out) override;
    void EndEntry(ISink& out) override;
    void Finish(ISink& out) override;
    void Unpack(ISource& src, const std::function<void(const EntryMeta&)>& on_entry,
                const std::function<void(const uint8_t*, size_t)>& on_data) override;

private:
    std::map<uint64_t, std::string> regular_paths_;
    uint64_t expected_ = 0;
    uint64_t written_ = 0;
    bool in_entry_ = false;
    bool finished_ = false;
};

}  // namespace cbk

#endif  // CODE_CORE_SRC_PACKERS_TAR_PACKER_H_
