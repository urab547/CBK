// Copyright 2026 CBK Project. 字节流变换接口（评分项：压缩解压 / 加密解密，每种算法 10 分）。
#ifndef CBK_STAGE_H_
#define CBK_STAGE_H_

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "cbk/sink.h"

namespace cbk {

/// Stage 的用途。流水线顺序校验依赖这个分类。
enum class StageKind {
    kCompress,  ///< 压缩
    kEncrypt,   ///< 加密
};

/// 字节流变换器：进去一堆字节，出来另一堆字节。
///
/// 压缩和加密都实现这个接口，所以它们可以任意串联。
/// 它不关心条目边界——那是 IPacker 的事。
///
/// ## 实现方须知
///
/// 1. 严禁使用第三方库直接实现（zlib、miniz、OpenSSL、Windows CryptoAPI
///    等等）。课程规定这样做该项扩展分记为 50%。算法自己写。
/// 2. **必须自己维护余料缓冲。** Process 会被调用很多次，每次的长度
///    不保证是你的块大小的整数倍。以 AES-CBC 为例：攒够 16 字节才加密
///    一块，剩下的留着，等下一次 Process 或最后的 Finish。
///    直接假设"每次进来都是 16 的倍数"，在小文件多的目录上必挂。
/// 3. **必须有往返测试。** 造随机数据（含全零、全 0xFF、纯文本、
///    已压缩数据这些边界情况），CreateForward 处理一遍，
///    再用 CreateInverse 处理回来，断言与原文完全一致。
///    这个测试要进 CI。压缩算法最坑的 bug 是备份时一切正常，
///    还原时才发现数据全毁了。
class IStage {
public:
    virtual ~IStage() = default;

    /// 算法名，会原样写进容器的管线描述串。
    /// 用小写英文加连字符，如 "huffman"、"lz77"、"aes128-cbc"。
    virtual std::string Name() const = 0;

    /// 处理一块输入，产出的字节写进 out。
    /// 允许内部缓冲，不要求输入输出 1:1，可以一个字节都不输出。
    virtual void Process(const uint8_t* data, size_t len, ISink& out) = 0;

    /// 输入结束。冲刷内部缓冲，写出尾部
    /// （如 Huffman 的位对齐填充、CBC 的 PKCS#7 补齐）。
    /// Finish 之后不会再调用 Process。
    virtual void Finish(ISink& out) = 0;

    /// 需要密码的 Stage 从这里拿。密码经由 stdin 传入进程，
    /// 不走命令行参数——命令行参数在 Windows 上能被其它进程读到。
    virtual void SetPassword(const std::string& /*password*/) {}
};

/// 一种算法的工厂，必须同时提供正变换和逆变换。
class IStageFactory {
public:
    virtual ~IStageFactory() = default;

    virtual std::string Name() const = 0;
    virtual StageKind Kind() const = 0;

    virtual std::unique_ptr<IStage> CreateForward() = 0;  ///< 压缩 / 加密
    virtual std::unique_ptr<IStage> CreateInverse() = 0;  ///< 解压 / 解密
};

/// Stage 注册表。`cbk info` 会按 Kind 分组列出，供 GUI 填两个下拉框。
class StageRegistry {
public:
    static StageRegistry& Instance();

    void Register(std::unique_ptr<IStageFactory> factory);
    bool Has(const std::string& name) const;
    /// 名字不存在时返回 nullptr。
    IStageFactory* Find(const std::string& name) const;
    /// 列出指定用途的全部算法名。
    std::vector<std::string> Names(StageKind kind) const;

private:
    StageRegistry() = default;
    std::map<std::string, std::unique_ptr<IStageFactory>> factories_;
};

/// 校验流水线顺序。
///
/// 顺序被强制为「打包 → 压缩 → 加密」，理由有两条，都不是偏好问题：
///   * 加密的输出是高熵伪随机字节，压缩它不但压不动，还会因为码表开销
///     让文件变大。所以压缩必须在加密之前。
///   * 打包决定条目边界（结构层），压缩加密只认字节（字节层）。
///     结构层必须在内，字节层必须在外，解包时才能先还原出完整字节流
///     再按结构解析。
///
/// 顺序配错时返回 false，调用方应报参数错误退出（Status::kBadArgs），
/// 而不是生成一个能写出来却还原不了的包。
bool ValidatePipelineOrder(const std::vector<std::string>& stage_names);

/// 注册所有内置 Stage。原因同 RegisterBuiltinPackers()。
void RegisterBuiltinStages();

}  // namespace cbk

#endif  // CBK_STAGE_H_
