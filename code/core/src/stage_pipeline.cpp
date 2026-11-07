// Copyright 2026 CBK Project. Stage 流水线的实现。
#include "src/stage_pipeline.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "cbk/types.h"

namespace cbk {

// ============================================================ StageChainSink

/// 一段适配器：把写进来的字节交给自己那个 Stage，Stage 的产物流向下游。
class StageChainSink::Adapter : public ISink {
public:
    /// @param stage      本段的变换（不持有）
    /// @param downstream 本段产物流向哪里（下一段适配器或链尾，不持有）
    Adapter(IStage* stage, ISink* downstream) : stage_(stage), downstream_(downstream) {}

    /// 上游写进来的数据交给本段 Stage 处理，结果写给下游。
    void Write(const uint8_t* data, size_t len) override {
        stage_->Process(data, len, *downstream_);
    }

    /// 让本段 Stage 吐出缓存在内部的最后一点数据。
    void FinishStage() { stage_->Finish(*downstream_); }

private:
    IStage* stage_;      ///< 本段变换
    ISink* downstream_;  ///< 下游
};

/// 搭建整条链。
///
/// 适配器只存裸指针，不持有 Stage 也不持有下游。
/// 生命周期都由 StageChainSink 兜着：stages_ 在本对象里，terminal 由调用方
/// 保证活得更久（头文件里写明了这条约定）。适配器自己的析构什么都不做，
/// 所以它们之间的销毁顺序无所谓。
StageChainSink::StageChainSink(std::vector<std::unique_ptr<IStage>> stages, ISink* terminal)
    : terminal_(terminal), stages_(std::move(stages)) {
    // 从链尾往链头搭：每一段的下游是它后面那一段，最后一段的下游是 terminal。
    adapters_.resize(stages_.size());
    ISink* downstream = terminal_;
    for (size_t i = stages_.size(); i > 0; --i) {
        const size_t index = i - 1;
        adapters_[index].reset(new Adapter(stages_[index].get(), downstream));
        downstream = adapters_[index].get();
    }
}

StageChainSink::~StageChainSink() = default;

/// 链头：没有任何 Stage 时直接返回链尾，数据原样落地。
ISink& StageChainSink::Entry() {
    if (adapters_.empty()) return *terminal_;
    return *adapters_.front();
}

/// 冲刷整条链。
void StageChainSink::Finish() {
    // 幂等。析构和显式调用都可能走到这里，而 Stage 的 Finish 被调两次
    // 通常会吐出两份尾部数据，直接把流写坏。
    if (finished_) return;
    finished_ = true;
    // 从链头往链尾。stage0 冲刷出来的尾部字节还要经过后面每一段。
    for (std::unique_ptr<Adapter>& adapter : adapters_) {
        adapter->FinishStage();
    }
}

// ========================================================== StageChainSource

/// 链尾：把 Stage 吐出来的字节攒进调用方给的 vector。
class StageChainSource::Terminal : public ISink {
public:
    /// @param out 收集产物的缓冲（就是 StageChainSource::pending_）
    explicit Terminal(std::vector<uint8_t>* out) : out_(out) {}

    /// 追加到缓冲末尾。
    void Write(const uint8_t* data, size_t len) override {
        if (data == nullptr || len == 0) return;
        out_->insert(out_->end(), data, data + len);
    }

private:
    std::vector<uint8_t>* out_;  ///< 收集产物的缓冲
};

/// 构造：准备 64 KB 输入缓冲，建链尾，再用推式的 StageChainSink 把各段串到链尾上。
StageChainSource::StageChainSource(ISource* upstream, std::vector<std::unique_ptr<IStage>> stages)
    : upstream_(upstream),
      input_(kIoBlockSize),
      terminal_(new Terminal(&pending_)),
      chain_(new StageChainSink(std::move(stages), terminal_.get())) {}

StageChainSource::~StageChainSource() = default;

/// 泵一次：从上游拉一块（最多 64 KB）推进链头，链尾产物自动落进 pending_。
bool StageChainSource::PumpOnce() {
    if (finished_) return false;

    const size_t got = upstream_->Read(input_.data(), input_.size());
    if (got > 0) {
        chain_->Entry().Write(input_.data(), got);
        return true;
    }

    // 上游读完了。冲刷各段的余料——StageChainSink::Finish 会按链头到链尾
    // 的正确顺序走一遍。
    finished_ = true;
    chain_->Finish();
    return false;
}

/// 拉式读取：pending_ 里还有就直接给；没有了就泵，直到有数据或彻底读完。
///
/// 泵一次不一定拿得到数据：压缩器完全可能吞掉一整块只为攒够窗口。
/// 所以要循环泵，直到攒出字节、或者 PumpOnce 报告"再也不会有了"。
///
/// 每轮开头清空 pending_ 而不是往后追加。不清的话缓冲会随着整个数据区
/// 一路涨大，"任何情况下不整个读进内存"这条硬要求就破了。
size_t StageChainSource::Read(uint8_t* buf, size_t len) {
    if (len == 0) return 0;

    while (pending_offset_ >= pending_.size()) {
        pending_.clear();
        pending_offset_ = 0;
        if (!PumpOnce()) break;
    }

    // 从 pending_ 里拷出"想要的"和"剩下的"中较小者。
    const size_t available = pending_.size() - pending_offset_;
    if (available == 0) return 0;

    const size_t count = std::min(len, available);
    std::copy(pending_.begin() + static_cast<std::ptrdiff_t>(pending_offset_),
              pending_.begin() + static_cast<std::ptrdiff_t>(pending_offset_ + count), buf);
    pending_offset_ += count;
    return count;
}

}  // namespace cbk
