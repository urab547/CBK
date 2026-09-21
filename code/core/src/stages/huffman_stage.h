// Copyright 2026 CBK Project.
#ifndef CODE_CORE_SRC_STAGES_HUFFMAN_STAGE_H_
#define CODE_CORE_SRC_STAGES_HUFFMAN_STAGE_H_

#include <memory>
#include <string>

#include "cbk/stage.h"

namespace cbk {

/// 分块 Huffman 工厂。每个实例独立维护最多一个块的输入余料。
/// 自定义流格式、确定性建树规则与限制见 docs/huffman格式说明.md。
class HuffmanStageFactory : public IStageFactory {
public:
    std::string Name() const override { return "huffman"; }
    StageKind Kind() const override { return StageKind::kCompress; }
    std::unique_ptr<IStage> CreateForward() override;
    std::unique_ptr<IStage> CreateInverse() override;
};

}  // namespace cbk

#endif  // CODE_CORE_SRC_STAGES_HUFFMAN_STAGE_H_
