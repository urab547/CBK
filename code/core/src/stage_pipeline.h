// Copyright 2026 CBK Project. 把若干 IStage 串成一条流水线。
//
// IStage 是**推**式的：Process(data, len, out) 把结果写进下游 ISink。
// 备份方向天然合拍——打包器往里推，一路推到容器。
//
// 还原方向不合拍：IPacker::Unpack 要的是**拉**式的 ISource。所以这里提供
// 两个东西：
//
//   StageChainSink   —— 备份用。打包器 -> stage1 -> stage2 -> 容器
//   StageChainSource —— 还原用。把推式的链包成拉式的 ISource，
//                       内部按需从上游拉一块、推过链、缓冲、再供给调用方。
//
// StageChainSource 的缓冲是有界的（一次一块 kIoBlockSize 的输入），
// 不会把整个数据区读进内存——那是这个项目的硬要求。
#ifndef CODE_CORE_SRC_STAGE_PIPELINE_H_
#define CODE_CORE_SRC_STAGE_PIPELINE_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "cbk/sink.h"
#include "cbk/stage.h"

namespace cbk {

/// 备份方向：把 stages 串成一条 ISink 链，最后写进 terminal。
///
/// stages 为空时 Entry() 直接就是 terminal，没有额外开销。
class StageChainSink {
public:
    /// @param stages   按应用顺序排列。stages[0] 最先看到数据。
    /// @param terminal 链尾，通常是容器的数据区或索引区。必须比本对象活得久。
    StageChainSink(std::vector<std::unique_ptr<IStage>> stages, ISink* terminal);
    ~StageChainSink();

    StageChainSink(const StageChainSink&) = delete;
    StageChainSink& operator=(const StageChainSink&) = delete;

    /// 链头。往这里写就等于走完整条链。
    ISink& Entry();

    /// 依次冲刷每一段。
    ///
    /// **必须从链头往链尾依次 Finish**：stage0 冲刷出来的尾部字节还要经过
    /// stage1、stage2。顺序反了的话，压缩器最后吐出的那点数据就不会被加密，
    /// 而且这种 bug 只在小文件上不出现——大文件的尾块照样错。
    void Finish();

private:
    class Adapter;

    ISink* terminal_;
    std::vector<std::unique_ptr<IStage>> stages_;
    std::vector<std::unique_ptr<Adapter>> adapters_;
    bool finished_ = false;
};

/// 还原方向：把推式的 Stage 链包成一个拉式的 ISource。
class StageChainSource : public ISource {
public:
    /// @param upstream 原始字节来源（容器的数据区或索引区）。必须比本对象活得久。
    /// @param stages   逆变换，按应用顺序（与写入时相反）排列。
    StageChainSource(ISource* upstream, std::vector<std::unique_ptr<IStage>> stages);
    ~StageChainSource() override;

    size_t Read(uint8_t* buf, size_t len) override;

private:
    /// 链尾：把 Stage 吐出来的字节攒进 pending_。定义在 .cpp 里。
    class Terminal;

    /// 从上游拉一块、推过链、结果落进 pending_。
    /// @return 还有没有可能产出更多数据。
    bool PumpOnce();

    ISource* upstream_;
    std::vector<uint8_t> input_;    ///< 一次拉一块的临时缓冲
    std::vector<uint8_t> pending_;  ///< 链尾吐出来、还没被取走的字节
    size_t pending_offset_ = 0;
    bool finished_ = false;

    // 声明顺序即构造顺序，销毁时反过来：chain_ 先于 terminal_ 先于 pending_，
    // 保证链在它写入的缓冲还活着的时候就已经拆掉了。
    std::unique_ptr<Terminal> terminal_;
    std::unique_ptr<StageChainSink> chain_;
};

}  // namespace cbk

#endif  // CODE_CORE_SRC_STAGE_PIPELINE_H_
