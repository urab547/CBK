// Copyright 2026 CBK Project.
#include "src/stages/huffman_stage.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <queue>
#include <stdexcept>
#include <string>
#include <vector>

#include "cbk/types.h"
#include "src/crc32.h"

namespace cbk {
namespace {

constexpr size_t kBlockSize = 65536;
constexpr size_t kHeaderSize = 13;
constexpr size_t kTableSize = 256 * 4;
constexpr std::array<uint8_t, 8> kMagic = {'C', 'B', 'K', 'H', 'F', '0', '0', '1'};
static_assert(kBlockSize == kIoBlockSize, "Huffman block must match the I/O budget");
using Frequencies = std::array<uint32_t, 256>;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// 长度字段仅描述一个有界块；流总长度不使用 32 位计数。
uint32_t Read32(const uint8_t* data) {
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value |= static_cast<uint32_t>(data[i]) << (8 * i);
    return value;
}

void Append32(uint32_t value, std::vector<uint8_t>* bytes) {
    for (unsigned i = 0; i < 4; ++i) bytes->push_back(static_cast<uint8_t>(value >> (8 * i)));
}

struct Node {
    uint32_t weight;
    int left = -1;
    int right = -1;
    int parent = -1;
    int symbol = -1;
};

// 叶节点按字节值入队，内部节点依创建顺序编号。
// 权重相同时按节点编号排序，保证不同编译器上码表完全一致。
struct Tree {
    std::vector<Node> nodes;
    std::array<std::vector<uint8_t>, 256> codes;
    int root = -1;

    explicit Tree(const Frequencies& frequencies) {
        nodes.reserve(511);
        const auto later = [this](int a, int b) {
            if (nodes[a].weight != nodes[b].weight) return nodes[a].weight > nodes[b].weight;
            return a > b;
        };
        std::priority_queue<int, std::vector<int>, decltype(later)> queue(later);
        for (int symbol = 0; symbol < 256; ++symbol) {
            if (frequencies[symbol] == 0) continue;
            nodes.push_back({frequencies[symbol], -1, -1, -1, symbol});
            queue.push(static_cast<int>(nodes.size()) - 1);
        }
        const size_t leaves = nodes.size();
        while (queue.size() > 1) {
            const int left = queue.top();
            queue.pop();
            const int right = queue.top();
            queue.pop();
            const int parent = static_cast<int>(nodes.size());
            nodes.push_back({nodes[left].weight + nodes[right].weight, left, right, -1, -1});
            nodes[left].parent = nodes[right].parent = parent;
            queue.push(parent);
        }
        Require(!queue.empty(), "huffman: empty frequency table");
        root = queue.top();
        // 从叶到根生成码字，避免将长码字塞入固定宽度整数。
        for (size_t leaf = 0; leaf < leaves; ++leaf) {
            auto& code = codes[nodes[leaf].symbol];
            int current = static_cast<int>(leaf);
            while (nodes[current].parent != -1) {
                const int parent = nodes[current].parent;
                code.push_back(nodes[parent].right == current ? 1 : 0);
                current = parent;
            }
            std::reverse(code.begin(), code.end());
        }
    }

    size_t BitCount(const Frequencies& frequencies) const {
        size_t bits = 0;
        for (size_t i = 0; i < frequencies.size(); ++i) bits += frequencies[i] * codes[i].size();
        return bits;
    }
};

void WriteHeader(uint8_t mode, size_t original, size_t stored, uint32_t crc, ISink& out) {
    std::vector<uint8_t> header{mode};
    Append32(static_cast<uint32_t>(original), &header);
    Append32(static_cast<uint32_t>(stored), &header);
    Append32(crc, &header);
    out.Write(header.data(), header.size());
}

class Encoder : public IStage {
public:
    std::string Name() const override { return "huffman"; }

    void Process(const uint8_t* data, size_t len, ISink& out) override {
        Require(!finished_, "huffman: input after Finish");
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
        Require(!finished_, "huffman: duplicate Finish");
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
        Frequencies frequencies{};
        for (uint8_t byte : pending_) ++frequencies[byte];
        const Tree tree(frequencies);
        const size_t bits = tree.BitCount(frequencies);
        const size_t stored = kTableSize + (bits + 7) / 8;
        const uint32_t crc = ComputeCrc32(pending_.data(), pending_.size());
        if (stored >= pending_.size()) {
            WriteHeader(1, pending_.size(), pending_.size(), crc, out);
            out.Write(pending_.data(), pending_.size());
        } else {
            std::vector<uint8_t> payload;
            payload.reserve(stored);
            for (uint32_t frequency : frequencies) Append32(frequency, &payload);
            payload.resize(stored, 0);
            size_t position = 0;
            for (uint8_t byte : pending_) {
                for (uint8_t bit : tree.codes[byte]) {
                    payload[kTableSize + position / 8] |= bit << (7 - position % 8);
                    ++position;
                }
            }
            WriteHeader(2, pending_.size(), stored, crc, out);
            out.Write(payload.data(), payload.size());
        }
        pending_.clear();
    }

    std::vector<uint8_t> pending_;
    bool started_ = false;
    bool finished_ = false;
};

class Decoder : public IStage {
public:
    std::string Name() const override { return "huffman"; }

    void Process(const uint8_t* data, size_t len, ISink& out) override {
        Require(!finished_, "huffman: input after Finish");
        // 只接收当前字段所需的字节，恶意长度不会触发无界分配。
        while (len != 0) {
            Require(state_ != State::kEnd, "huffman: trailing data");
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
        Require(!finished_, "huffman: duplicate Finish");
        Require(state_ == State::kEnd, "huffman: truncated stream");
        finished_ = true;
    }

private:
    enum class State { kMagic, kHeader, kPayload, kEnd };

    void Consume(ISink& out) {
        if (state_ == State::kMagic) {
            Require(std::equal(pending_.begin(), pending_.end(), kMagic.begin()),
                    "huffman: invalid magic or version");
            state_ = State::kHeader;
            needed_ = kHeaderSize;
        } else if (state_ == State::kHeader) {
            mode_ = pending_[0];
            original_ = Read32(pending_.data() + 1);
            const size_t stored = Read32(pending_.data() + 5);
            crc_ = Read32(pending_.data() + 9);
            if (mode_ == 0) {
                Require(original_ == 0 && stored == 0 && crc_ == 0, "huffman: invalid end marker");
                state_ = State::kEnd;
                return;
            }
            Require(original_ > 0 && original_ <= kBlockSize, "huffman: invalid block length");
            Require((mode_ == 1 && stored == original_) ||
                        (mode_ == 2 && stored >= kTableSize && stored < original_),
                    "huffman: invalid mode or payload length");
            state_ = State::kPayload;
            needed_ = stored;
        } else {
            std::vector<uint8_t> decoded;
            const std::vector<uint8_t>* bytes = &pending_;
            if (mode_ == 2) {
                decoded = DecodeBlock();
                bytes = &decoded;
            }
            Require(ComputeCrc32(bytes->data(), bytes->size()) == crc_,
                    "huffman: block checksum mismatch");
            // 当前块完全校验后才输出，避免向解包器交付损坏块。
            out.Write(bytes->data(), bytes->size());
            state_ = State::kHeader;
            needed_ = kHeaderSize;
        }
    }

    std::vector<uint8_t> DecodeBlock() const {
        Frequencies frequencies{};
        uint64_t total = 0;
        for (size_t i = 0; i < frequencies.size(); ++i) {
            frequencies[i] = Read32(pending_.data() + i * 4);
            total += frequencies[i];
        }
        Require(total == original_, "huffman: invalid frequency total");
        const Tree tree(frequencies);
        const size_t bits = tree.BitCount(frequencies);
        Require(kTableSize + (bits + 7) / 8 == pending_.size(), "huffman: invalid bit length");
        if (bits % 8 != 0) {
            const unsigned mask = (1u << (8 - bits % 8)) - 1;
            Require((pending_.back() & mask) == 0, "huffman: nonzero padding");
        }
        std::vector<uint8_t> decoded;
        decoded.reserve(original_);
        size_t position = 0;
        for (size_t i = 0; i < original_; ++i) {
            int node = tree.root;
            while (tree.nodes[node].symbol == -1) {
                Require(position < bits, "huffman: incomplete codeword");
                const auto bit = (pending_[kTableSize + position / 8] >> (7 - position % 8)) & 1;
                ++position;
                node = bit ? tree.nodes[node].right : tree.nodes[node].left;
            }
            const auto symbol = static_cast<uint8_t>(tree.nodes[node].symbol);
            Require(frequencies[symbol] != 0, "huffman: symbol frequency mismatch");
            --frequencies[symbol];
            decoded.push_back(symbol);
        }
        Require(position == bits, "huffman: unused codewords");
        return decoded;
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

std::unique_ptr<IStage> HuffmanStageFactory::CreateForward() {
    return std::make_unique<Encoder>();
}

std::unique_ptr<IStage> HuffmanStageFactory::CreateInverse() {
    return std::make_unique<Decoder>();
}

}  // namespace cbk
