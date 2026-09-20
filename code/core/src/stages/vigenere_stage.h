#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "cbk/stage.h"

namespace cbk {

class VigenereStage : public IStage {
public:
    explicit VigenereStage(bool inverse = false);
    std::string Name() const override;
    void Process(const uint8_t* data, size_t len, ISink& out) override;
    void Finish(ISink& out) override;
    void SetPassword(const std::string& password) override;

private:
    std::vector<uint8_t> key_;
    size_t pos_ = 0;
    bool inverse_ = false;
};

class VigenereStageFactory : public IStageFactory {
public:
    std::string Name() const override { return "vigenere"; }
    StageKind Kind() const override { return StageKind::kEncrypt; }
    std::unique_ptr<IStage> CreateForward() override {
        return std::make_unique<VigenereStage>(false);
    }
    std::unique_ptr<IStage> CreateInverse() override {
        return std::make_unique<VigenereStage>(true);
    }
};

}  // namespace cbk
