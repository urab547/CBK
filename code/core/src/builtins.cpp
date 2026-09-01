// Copyright 2026 CBK Project.
//
// 内置算法的唯一注册点。
//
// ★ 加了新算法，就在这个文件里加一行。★
//
// 为什么不用"每个 .cpp 里放个静态对象自动注册"那种写法：
// core 是静态库，链接器会把没有被任何符号引用到的目标文件整个丢掉，
// 那样注册代码根本不会执行，而且不报错，只是算法凭空消失。
// 显式列一遍虽然多一行，但永远不会出这种问题。

#include <memory>

#include "cbk/packer.h"
#include "cbk/stage.h"
#include "src/packers/native_packer.h"

// 新增打包算法时在这里加 include：
// #include "src/packers/tar_packer.h"      // 队友 A
// #include "src/packers/cpio_packer.h"     // 队友 A

// 新增 Stage 时在这里加 include：
// #include "stages/huffman_stage.h"    // 队友 A
// #include "stages/lz77_stage.h"       // 队友 A
// #include "stages/xor_stage.h"        // 队友 B
// #include "stages/aes_stage.h"        // 队友 B

namespace cbk {

void RegisterBuiltinPackers() {
    PackerRegistry& registry = PackerRegistry::Instance();

    registry.Register(kNativePackerName, [] { return std::make_unique<NativePacker>(); });
    // registry.Register("tar",  [] { return std::make_unique<TarPacker>(); });
    // registry.Register("cpio", [] { return std::make_unique<CpioPacker>(); });
}

void RegisterBuiltinStages() {
    StageRegistry& registry = StageRegistry::Instance();
    (void)registry;  // 同上

    // registry.Register(std::make_unique<HuffmanStageFactory>());
    // registry.Register(std::make_unique<Lz77StageFactory>());
    // registry.Register(std::make_unique<XorStageFactory>());
    // registry.Register(std::make_unique<AesStageFactory>());
}

}  // namespace cbk
