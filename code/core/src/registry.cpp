// Copyright 2026 CBK Project. 注册表与流水线顺序校验的实现。
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "cbk/packer.h"
#include "cbk/stage.h"
#include "cbk/types.h"

namespace cbk {

/// 文件类型 → 英文短名，cbk list 输出和日志里用。
const char* ToString(FileType type) {
    switch (type) {
        case FileType::kRegular: return "regular";
        case FileType::kDirectory: return "directory";
        case FileType::kSymlinkFile: return "symlink-file";
        case FileType::kSymlinkDir: return "symlink-dir";
        case FileType::kJunction: return "junction";
        case FileType::kHardlinkRef: return "hardlink-ref";
        case FileType::kUnsupported: return "unsupported";
    }
    return "unknown";
}

// ---------------------------------------------------------------- PackerRegistry

/// 取全局唯一的打包器注册表（单例）。
///
/// 用函数内静态变量而不是全局对象：
/// 全局对象之间的初始化顺序在 C++ 里是不确定的（跨编译单元），
/// 而注册表必须在 RegisterBuiltinPackers() 跑之前就构造好。
/// 函数内静态变量保证"第一次用到时才构造"，从根上避开这个问题，
/// C++11 起还保证线程安全。
PackerRegistry& PackerRegistry::Instance() {
    static PackerRegistry instance;
    return instance;
}

/// 登记一个打包器：名字 → 创建它的工厂函数。同名后登记的覆盖先登记的。
void PackerRegistry::Register(const std::string& name, Factory factory) {
    factories_[name] = std::move(factory);
}

/// 这个名字有没有登记过。
bool PackerRegistry::Has(const std::string& name) const {
    return factories_.find(name) != factories_.end();
}

/// 按名字新建一个打包器实例。
///
/// 返回 nullptr 而不是抛异常。名字不认识属于"调用方传错了参数"，
/// 由 CLI 层转成退出码 3；抛异常会被当成不可恢复失败转成退出码 2，
/// 那样 GUI 就分不清是自己传错了还是程序真的崩了。
std::unique_ptr<IPacker> PackerRegistry::Create(const std::string& name) const {
    auto it = factories_.find(name);
    if (it == factories_.end()) return nullptr;
    return it->second();
}

/// 列出所有已登记的打包器名字。
///
/// 用 std::map 而不是 unordered_map，图的就是这里：遍历出来天然按名字
/// 有序，cbk info 给 GUI 的下拉框顺序才是稳定的。算法总共不会超过十几个，
/// 查找性能差异可以忽略。
std::vector<std::string> PackerRegistry::Names() const {
    std::vector<std::string> names;
    names.reserve(factories_.size());
    for (const auto& kv : factories_) names.push_back(kv.first);
    return names;
}

// ----------------------------------------------------------------- StageRegistry

/// 取全局唯一的变换阶段（压缩/加密）注册表，原理同 PackerRegistry::Instance。
StageRegistry& StageRegistry::Instance() {
    static StageRegistry instance;
    return instance;
}

/// 登记一个阶段工厂，名字取自工厂自己的 Name()。空指针直接忽略。
void StageRegistry::Register(std::unique_ptr<IStageFactory> factory) {
    if (!factory) return;
    const std::string name = factory->Name();
    factories_[name] = std::move(factory);
}

/// 这个名字有没有登记过。
bool StageRegistry::Has(const std::string& name) const {
    return factories_.find(name) != factories_.end();
}

/// 按名字找工厂，找不到返回 nullptr。返回的指针归注册表所有，调用方不要 delete。
IStageFactory* StageRegistry::Find(const std::string& name) const {
    auto it = factories_.find(name);
    if (it == factories_.end()) return nullptr;
    return it->second.get();
}

/// 列出某一类（压缩或加密）的全部名字，cbk info 分两组输出。
std::vector<std::string> StageRegistry::Names(StageKind kind) const {
    std::vector<std::string> names;
    for (const auto& kv : factories_) {
        if (kv.second->Kind() == kind) names.push_back(kv.first);
    }
    return names;
}

// ------------------------------------------------------- ValidatePipelineOrder

/// 检查阶段顺序：所有名字都必须认识，且一旦出现过加密，后面就不许再有压缩。
bool ValidatePipelineOrder(const std::vector<std::string>& stage_names) {
    const StageRegistry& registry = StageRegistry::Instance();
    bool seen_encrypt = false;
    for (const std::string& name : stage_names) {
        IStageFactory* factory = registry.Find(name);
        if (factory == nullptr) return false;  // 未知算法
        if (factory->Kind() == StageKind::kEncrypt) {
            seen_encrypt = true;
        } else if (factory->Kind() == StageKind::kCompress && seen_encrypt) {
            return false;  // 压缩排在加密之后，压缩率会归零
        }
    }
    return true;
}

}  // namespace cbk
