#include "src/stages/xor_stage.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "cbk/sink.h"
#include "cbk/text.h"

namespace cbk {

XorStage::XorStage() = default;

std::string XorStage::Name() const {
    return "xor";
}

void XorStage::SetPassword(const std::string& password) {
    key_.clear();
    if (password.empty()) {
        // 空密码意味着无加密：用 single zero byte 做 keystream（相当于不变）
        key_.push_back(0);
        return;
    }
    key_.assign(password.begin(), password.end());
    // 强度提升（可选）：简单地在 key 后追加长度字节，避免短密码总是同一字节
    key_.push_back(static_cast<uint8_t>(key_.size() & 0xFF));
    pos_ = 0;
}

void XorStage::Process(const uint8_t* data, size_t len, ISink& out) {
    if (len == 0) return;
    std::vector<uint8_t> buf;
    buf.resize(len);
    const size_t k = key_.empty() ? 1 : key_.size();
    for (size_t i = 0; i < len; ++i) {
        uint8_t kbyte = key_[pos_ % k];
        buf[i] = static_cast<uint8_t>(data[i] ^ kbyte);
        pos_ = (pos_ + 1) % k;
    }
    out.Write(buf.data(), buf.size());
}

void XorStage::Finish(ISink& /*out*/) {
    // XOR 无尾部数据，nothing to flush.
}

}  // namespace cbk
