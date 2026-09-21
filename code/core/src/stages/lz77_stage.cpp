// Copyright 2026 CBK Project.
#include "src/stages/lz77_stage.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "cbk/types.h"
#include "src/crc32.h"

namespace cbk {
namespace {

constexpr size_t kBlockSize = 65536;
constexpr size_t kWindowSize = 32768;
constexpr size_t kMaxMatch = 258;
constexpr size_t kHashSize = 4096;
constexpr size_t kSearchLimit = 64;
constexpr size_t kHeaderSize = 13;
constexpr std::array<uint8_t, 8> kMagic = {'C', 'B', 'K', 'L', 'Z', '0', '0', '1'};
static_assert(kBlockSize == kIoBlockSize, "LZ77 block must match the I/O budget");

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

uint32_t Read32(const uint8_t* data) {
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value |= static_cast<uint32_t>(data[i]) << (8 * i);
    return value;
}

void Append32(uint32_t value, std::vector<uint8_t>* bytes) {
    for (unsigned i = 0; i < 4; ++i) bytes->push_back(static_cast<uint8_t>(value >> (8 * i)));
}

void WriteHeader(uint8_t mode, size_t original, size_t stored, uint32_t crc, ISink& out) {
    std::vector<uint8_t> header{mode};
    Append32(static_cast<uint32_t>(original), &header);
    Append32(static_cast<uint32_t>(stored), &header);
    Append32(crc, &header);
    out.Write(header.data(), header.size());
}

size_t Hash(const std::vector<uint8_t>& bytes, size_t offset) {
    return ((bytes[offset] * 251u + bytes[offset + 1]) * 251u + bytes[offset + 2]) % kHashSize;
}

// 哈希链索引块内已编码位置；每次最多比较 64 个候选，避免重复前缀导致二次扫描。
// 匹配引用可重叠，解码器必须逐字节复制，不能使用不支持重叠扩展的 memcpy。
std::vector<uint8_t> EncodeBlock(const std::vector<uint8_t>& input) {
    std::array<int, kHashSize> heads;
    heads.fill(-1);
    std::vector<int> previous(input.size(), -1);
    std::vector<uint8_t> encoded;
    encoded.reserve(input.size() + 4);
    size_t position = 0;
    while (position < input.size()) {
        const size_t flags_offset = encoded.size();
        encoded.push_back(0);
        for (unsigned token = 0; token < 8 && position < input.size(); ++token) {
            size_t best_length = 0;
            size_t best_distance = 0;
            const size_t limit = std::min(kMaxMatch, input.size() - position);
            if (limit >= 3) {
                int candidate = heads[Hash(input, position)];
                for (size_t count = 0; candidate >= 0 && count < kSearchLimit; ++count) {
                    const size_t source = static_cast<size_t>(candidate);
                    if (position - source > kWindowSize) break;
                    size_t length = 0;
                    while (length < limit && input[source + length] == input[position + length]) {
                        ++length;
                    }
                    if (length > best_length) {
                        best_length = length;
                        best_distance = position - source;
                    }
                    if (length == limit) break;
                    candidate = previous[source];
                }
            }
            const size_t consumed = best_length >= 3 ? best_length : 1;
            if (best_length >= 3) {
                encoded[flags_offset] |= static_cast<uint8_t>(1u << token);
                encoded.push_back(static_cast<uint8_t>(best_distance));
                encoded.push_back(static_cast<uint8_t>(best_distance >> 8));
                encoded.push_back(static_cast<uint8_t>(best_length - 3));
            } else {
                encoded.push_back(input[position]);
            }
            // 编码只会继续增长，达到原始大小即可停止搜索并选择原样模式。
            if (encoded.size() >= input.size()) return {};
            const size_t end = position + consumed;
            for (; position < end; ++position) {
                if (position + 2 >= input.size()) continue;
                const size_t hash = Hash(input, position);
                previous[position] = heads[hash];
                heads[hash] = static_cast<int>(position);
            }
        }
    }
    return encoded;
}

std::vector<uint8_t> DecodeBlock(const std::vector<uint8_t>& input, size_t original) {
    std::vector<uint8_t> decoded;
    decoded.reserve(original);
    size_t position = 0;
    while (decoded.size() < original) {
        Require(position < input.size(), "lz77: missing token flags");
        const unsigned flags = input[position++];
        unsigned token = 0;
        for (; token < 8 && decoded.size() < original; ++token) {
            if ((flags & (1u << token)) == 0) {
                Require(position < input.size(), "lz77: truncated literal");
                decoded.push_back(input[position++]);
            } else {
                Require(input.size() - position >= 3, "lz77: truncated match");
                const size_t high = static_cast<size_t>(input[position + 1]) << 8;
                const size_t distance = input[position] | high;
                const size_t length = static_cast<size_t>(input[position + 2]) + 3;
                position += 3;
                Require(distance > 0 && distance <= kWindowSize && distance <= decoded.size(),
                        "lz77: invalid match distance");
                Require(length <= original - decoded.size(), "lz77: match exceeds block length");
                for (size_t i = 0; i < length; ++i) {
                    const uint8_t byte = decoded[decoded.size() - distance];
                    decoded.push_back(byte);
                }
            }
        }
        Require((flags >> token) == 0, "lz77: nonzero unused flags");
    }
    Require(position == input.size(), "lz77: unused payload bytes");
    return decoded;
}

class Encoder : public IStage {
public:
    std::string Name() const override { return "lz77"; }

    void Process(const uint8_t* data, size_t len, ISink& out) override {
        Require(!finished_, "lz77: input after Finish");
        Start(out);
        while (len != 0) {
            const size_t count = std::min(len, kBlockSize - pending_.size());
            pending_.insert(pending_.end(), data, data + count);
            data += count;
            len -= count;
            if (pending_.size() == kBlockSize) Flush(out);
        }
    }

    void Finish(ISink& out) override {
        Require(!finished_, "lz77: duplicate Finish");
        Start(out);
        if (!pending_.empty()) Flush(out);
        WriteHeader(0, 0, 0, 0, out);
        finished_ = true;
    }

private:
    void Start(ISink& out) {
        if (started_) return;
        pending_.reserve(kBlockSize);
        out.Write(kMagic.data(), kMagic.size());
        started_ = true;
    }

    void Flush(ISink& out) {
        const auto encoded = EncodeBlock(pending_);
        const bool raw = encoded.empty();
        const auto& payload = raw ? pending_ : encoded;
        const auto crc = ComputeCrc32(pending_.data(), pending_.size());
        WriteHeader(raw ? 1 : 2, pending_.size(), payload.size(), crc, out);
        out.Write(payload.data(), payload.size());
        pending_.clear();
    }

    std::vector<uint8_t> pending_;
    bool started_ = false;
    bool finished_ = false;
};

class Decoder : public IStage {
public:
    std::string Name() const override { return "lz77"; }

    void Process(const uint8_t* data, size_t len, ISink& out) override {
        Require(!finished_, "lz77: input after Finish");
        // 逐字段积攒输入；块头完成校验后才接收有界的负载。
        while (len != 0) {
            Require(state_ != State::kEnd, "lz77: trailing data");
            const size_t count = std::min(len, needed_ - pending_.size());
            pending_.insert(pending_.end(), data, data + count);
            data += count;
            len -= count;
            if (pending_.size() != needed_) continue;
            Consume(out);
            pending_.clear();
        }
    }

    void Finish(ISink&) override {
        Require(!finished_, "lz77: duplicate Finish");
        Require(state_ == State::kEnd, "lz77: truncated stream");
        finished_ = true;
    }

private:
    enum class State { kMagic, kHeader, kPayload, kEnd };

    void Consume(ISink& out) {
        if (state_ == State::kMagic) {
            Require(std::equal(pending_.begin(), pending_.end(), kMagic.begin()),
                    "lz77: invalid magic or version");
            state_ = State::kHeader;
            needed_ = kHeaderSize;
        } else if (state_ == State::kHeader) {
            mode_ = pending_[0];
            original_ = Read32(pending_.data() + 1);
            const size_t stored = Read32(pending_.data() + 5);
            crc_ = Read32(pending_.data() + 9);
            if (mode_ == 0) {
                Require(original_ == 0 && stored == 0 && crc_ == 0, "lz77: invalid end marker");
                state_ = State::kEnd;
                return;
            }
            Require(original_ > 0 && original_ <= kBlockSize, "lz77: invalid block length");
            Require((mode_ == 1 && stored == original_) ||
                        (mode_ == 2 && stored > 0 && stored < original_),
                    "lz77: invalid mode or payload length");
            state_ = State::kPayload;
            needed_ = stored;
        } else {
            std::vector<uint8_t> decoded;
            const std::vector<uint8_t>* bytes = &pending_;
            if (mode_ == 2) {
                decoded = DecodeBlock(pending_, original_);
                bytes = &decoded;
            }
            const auto actual_crc = ComputeCrc32(bytes->data(), bytes->size());
            Require(actual_crc == crc_, "lz77: block checksum mismatch");
            // 损坏块不交付下游；已输出的前序块由调用方负责清理。
            out.Write(bytes->data(), bytes->size());
            state_ = State::kHeader;
            needed_ = kHeaderSize;
        }
    }

    State state_ = State::kMagic;
    size_t needed_ = kMagic.size();
    size_t original_ = 0;
    uint32_t crc_ = 0;
    uint8_t mode_ = 0;
    std::vector<uint8_t> pending_;
    bool finished_ = false;
};

}  // namespace

std::unique_ptr<IStage> Lz77StageFactory::CreateForward() {
    return std::make_unique<Encoder>();
}

std::unique_ptr<IStage> Lz77StageFactory::CreateInverse() {
    return std::make_unique<Decoder>();
}

}  // namespace cbk
