// Copyright 2026 CBK Project. 注册表与流水线顺序校验的实现。
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "cbk/packer.h"
#include "cbk/stage.h"
#include "cbk/types.h"

namespace cbk {

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

PackerRegistry& PackerRegistry::Instance() {
    static PackerRegistry instance;
    return instance;
}

void PackerRegistry::Register(const std::string& name, Factory factory) {
    factories_[name] = std::move(factory);
}

bool PackerRegistry::Has(const std::string& name) const {
    return factories_.find(name) != factories_.end();
}

std::unique_ptr<IPacker> PackerRegistry::Create(const std::string& name) const {
    auto it = factories_.find(name);
    if (it == factories_.end()) return nullptr;
    return it->second();
}

std::vector<std::string> PackerRegistry::Names() const {
    std::vector<std::string> names;
    names.reserve(factories_.size());
    for (const auto& kv : factories_) names.push_back(kv.first);
    return names;
}

// ----------------------------------------------------------------- StageRegistry

StageRegistry& StageRegistry::Instance() {
    static StageRegistry instance;
    return instance;
}

void StageRegistry::Register(std::unique_ptr<IStageFactory> factory) {
    if (!factory) return;
    const std::string name = factory->Name();
    factories_[name] = std::move(factory);
}

bool StageRegistry::Has(const std::string& name) const {
    return factories_.find(name) != factories_.end();
}

IStageFactory* StageRegistry::Find(const std::string& name) const {
    auto it = factories_.find(name);
    if (it == factories_.end()) return nullptr;
    return it->second.get();
}

std::vector<std::string> StageRegistry::Names(StageKind kind) const {
    std::vector<std::string> names;
    for (const auto& kv : factories_) {
        if (kv.second->Kind() == kind) names.push_back(kv.first);
    }
    return names;
}

// ------------------------------------------------------- ValidatePipelineOrder

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
