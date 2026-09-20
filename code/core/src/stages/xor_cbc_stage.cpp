#include "src/stages/xor_cbc_stage.h"

#include <algorithm>
#include <cstring>
#include <random>

#include "cbk/sink.h"

namespace cbk {

XorCbcStage::XorCbcStage(bool inverse) : inverse_(inverse) {
    key_block_.assign(kBlockSize, 0);
    prev_block_.assign(kBlockSize, 0);
}

std::string XorCbcStage::Name() const {
    return "xor-cbc";
}

void XorCbcStage::SetPassword(const std::string& password) {
    if (password.empty()) {
        std::fill(key_block_.begin(), key_block_.end(), 0);
    } else {
        for (size_t i = 0; i < kBlockSize; ++i) {
            key_block_[i] = static_cast<uint8_t>(password[i % password.size()]);
        }
    }
}

void XorCbcStage::XorBlock(uint8_t* out, const uint8_t* a, const uint8_t* b) {
    for (size_t i = 0; i < kBlockSize; ++i) out[i] = static_cast<uint8_t>(a[i] ^ b[i]);
}

void XorCbcStage::Process(const uint8_t* data, size_t len, ISink& out) {
    // if forward and IV not written, generate and write IV
    if (!wrote_iv_ && !inverse_) {
        std::vector<uint8_t> iv(kBlockSize);
        std::random_device rd;
        for (size_t i = 0; i < kBlockSize; ++i) iv[i] = static_cast<uint8_t>(rd() & 0xFF);
        prev_block_ = iv;
        out.Write(iv.data(), iv.size());
        wrote_iv_ = true;
    }

    // if inverse and IV not read yet, buffer until we have IV
    if (!wrote_iv_ && inverse_) {
        buffer_.insert(buffer_.end(), data, data + len);
        if (buffer_.size() >= kBlockSize) {
            std::copy(buffer_.begin(), buffer_.begin() + kBlockSize, prev_block_.begin());
            std::vector<uint8_t> tail(buffer_.begin() + kBlockSize, buffer_.end());
            buffer_.swap(tail);
            wrote_iv_ = true;
        }
        return;
    }

    if (len > 0) buffer_.insert(buffer_.end(), data, data + len);

    while (buffer_.size() >= kBlockSize) {
        uint8_t in_block[kBlockSize];
        uint8_t out_block[kBlockSize];
        std::memcpy(in_block, buffer_.data(), kBlockSize);

        if (!inverse_) {
            uint8_t temp[kBlockSize];
            for (size_t i = 0; i < kBlockSize; ++i)
                temp[i] = static_cast<uint8_t>(in_block[i] ^ key_block_[i]);
            XorBlock(out_block, temp, prev_block_.data());
            out.Write(out_block, kBlockSize);
            std::copy(out_block, out_block + kBlockSize, prev_block_.begin());
        } else {
            uint8_t temp[kBlockSize];
            XorBlock(temp, in_block, prev_block_.data());
            for (size_t i = 0; i < kBlockSize; ++i)
                out_block[i] = static_cast<uint8_t>(temp[i] ^ key_block_[i]);
            // append decrypted plaintext to plain_out_buffer_ for padding removal later
            plain_out_buffer_.insert(plain_out_buffer_.end(), out_block, out_block + kBlockSize);
            std::copy(in_block, in_block + kBlockSize, prev_block_.begin());
        }

        buffer_.erase(buffer_.begin(), buffer_.begin() + kBlockSize);
    }
}

void XorCbcStage::Finish(ISink& out) {
    if (!wrote_iv_ && inverse_) return;  // nothing to do

    if (!inverse_) {
        size_t pad = kBlockSize - buffer_.size();
        if (pad == 0) pad = kBlockSize;
        std::vector<uint8_t> block(buffer_);
        block.resize(buffer_.size() + pad, static_cast<uint8_t>(pad));
        size_t total = block.size();
        size_t offset = 0;
        while (offset < total) {
            uint8_t out_block[kBlockSize];
            uint8_t temp[kBlockSize];
            std::memcpy(temp, block.data() + offset, kBlockSize);
            for (size_t i = 0; i < kBlockSize; ++i)
                temp[i] = static_cast<uint8_t>(temp[i] ^ key_block_[i]);
            XorBlock(out_block, temp, prev_block_.data());
            out.Write(out_block, kBlockSize);
            std::copy(out_block, out_block + kBlockSize, prev_block_.begin());
            offset += kBlockSize;
        }
        buffer_.clear();
    } else {
        // inverse: remove PKCS#7 padding from plain_out_buffer_ and write trimmed plaintext
        if (plain_out_buffer_.empty()) return;
        uint8_t pad = plain_out_buffer_.back();
        if (pad == 0 || pad > kBlockSize) {
            // invalid padding, write all
            out.Write(plain_out_buffer_.data(), plain_out_buffer_.size());
            plain_out_buffer_.clear();
            return;
        }
        size_t plain_size = plain_out_buffer_.size();
        bool ok = true;
        for (size_t i = 0; i < pad; ++i) {
            if (plain_out_buffer_[plain_size - 1 - i] != pad) {
                ok = false;
                break;
            }
        }
        if (!ok) {
            out.Write(plain_out_buffer_.data(), plain_out_buffer_.size());
            plain_out_buffer_.clear();
            return;
        }
        size_t write_len = plain_size - pad;
        if (write_len > 0) out.Write(plain_out_buffer_.data(), write_len);
        plain_out_buffer_.clear();
    }
}

}  // namespace cbk
