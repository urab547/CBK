// Copyright 2026 CBK Project.
#include "src/stages/vigenere_stage.h"

#include <algorithm>
#include <string>
#include <vector>

#include "cbk/sink.h"

namespace cbk {

VigenereStage::VigenereStage(bool inverse) : inverse_(inverse) {}

std::string VigenereStage::Name() const {
    return "vigenere";
}

void VigenereStage::SetPassword(const std::string& password) {
    key_.clear();
    if (password.empty()) {
        key_.push_back(0);
        return;
    }
    key_.assign(password.begin(), password.end());
    pos_ = 0;
}

void VigenereStage::Process(const uint8_t* data, size_t len, ISink& out) {
    if (len == 0) return;
    std::vector<uint8_t> buf(len);
    const size_t k = key_.empty() ? 1 : key_.size();
    for (size_t i = 0; i < len; ++i) {
        uint8_t kbyte = key_[pos_ % k];
        if (!inverse_) {
            buf[i] = static_cast<uint8_t>((data[i] + kbyte) & 0xFF);
        } else {
            buf[i] = static_cast<uint8_t>((data[i] - kbyte) & 0xFF);
        }
        pos_ = (pos_ + 1) % k;
    }
    out.Write(buf.data(), buf.size());
}

void VigenereStage::Finish(ISink& /*out*/) {
    // Vigenere has no tail state.
}

}  // namespace cbk
