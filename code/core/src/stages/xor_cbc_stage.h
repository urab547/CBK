// Copyright 2026 CBK Project.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "cbk/stage.h"

namespace cbk {

class XorCbcStage : public IStage {
public:
    explicit XorCbcStage(bool inverse = false);
    std::string Name() const override;
    void Process(const uint8_t* data, size_t len, ISink& out) override;
    void Finish(ISink& out) override;
    void SetPassword(const std::string& password) override;

private:
    static constexpr size_t kBlockSize = 16;
    std::vector<uint8_t> key_block_;   // kBlockSize bytes
    std::vector<uint8_t> buffer_;      // cache for partial input (cipher for inverse, plaintext for
                                       // forward before padding)
    std::vector<uint8_t> prev_block_;  // previous cipher block (IV or last cipher)
    std::vector<uint8_t>
        plain_out_buffer_;  // stores decrypted plaintext blocks until Finish for padding removal
    bool wrote_iv_ = false;
    bool inverse_ = false;

    void XorBlock(uint8_t* out, const uint8_t* a, const uint8_t* b);
};

class XorCbcStageFactory : public IStageFactory {
public:
    std::string Name() const override { return "xor-cbc"; }
    StageKind Kind() const override { return StageKind::kEncrypt; }
    std::unique_ptr<IStage> CreateForward() override {
        return std::make_unique<XorCbcStage>(false);
    }
    std::unique_ptr<IStage> CreateInverse() override { return std::make_unique<XorCbcStage>(true); }
};

}  // namespace cbk
