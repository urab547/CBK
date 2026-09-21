// Copyright 2026 CBK Project.
#ifndef CODE_CORE_SRC_STAGES_LZ77_STAGE_H_
#define CODE_CORE_SRC_STAGES_LZ77_STAGE_H_

#include <memory>
#include <string>

#include "cbk/stage.h"

namespace cbk {

/// 分块 LZ77/LZSS 工厂：32 KB 窗口，块间独立，支持任意分段输入。
/// 格式、匹配搜索上限及错误处理见 docs/lz77格式说明.md。
class Lz77StageFactory : public IStageFactory {
public:
    std::string Name() const override { return "lz77"; }
    StageKind Kind() const override { return StageKind::kCompress; }
    std::unique_ptr<IStage> CreateForward() override;
    std::unique_ptr<IStage> CreateInverse() override;
};

}  // namespace cbk

#endif  // CODE_CORE_SRC_STAGES_LZ77_STAGE_H_
