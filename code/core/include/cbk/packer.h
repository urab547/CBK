// Copyright 2026 CBK Project. 打包解包接口（评分项：打包解包，每种算法 10 分）。
#ifndef CBK_PACKER_H_
#define CBK_PACKER_H_

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "cbk/sink.h"
#include "cbk/types.h"

namespace cbk {

/// 打包器：决定「条目边界在哪」，即一条条目的元数据与内容
/// 按什么格式排进字节流，以及怎么再解析回来。
///
/// 这是结构层。它上游是 Scanner（一条条给条目），下游是 IStage 链
/// （压缩、加密，只认字节不认条目）。
///
/// ## 实现方须知
///
/// 1. 严禁使用第三方库直接实现（libarchive 之类）。课程规定这样做该项
///    扩展分记为 50%。tar / cpio 的格式规范自己查，头部自己拼。
/// 2. 打包必须是流式的：writeData 会被调用很多次，每次给你一小块，
///    不保证长度对齐。不要假设一次就拿到整个文件。
/// 3. 每个实现都必须有往返测试：手写几个 EntryMeta（一个普通文件、
///    一个目录、一个符号链接、一个中文名文件），打包再解包，
///    断言元数据和内容一字节不差。这个测试不依赖 Scanner，可以独立写。
///
/// ## 调用顺序
///
///     for each entry:
///         BeginEntry(meta, out)
///         WriteData(...)   // 0 到 N 次；目录和链接是 0 次
///         EndEntry(out)
///     Finish(out)
class IPacker {
public:
    virtual ~IPacker() = default;

    /// 算法名，会原样写进容器的管线描述串。用小写英文，如 "tar"、"cpio"。
    virtual std::string Name() const = 0;

    // —— 打包 ——

    /// 开始一条新条目，通常在这里写出该格式的头部。
    virtual void BeginEntry(const EntryMeta& meta, ISink& out) = 0;

    /// 写出条目内容的一块。可能被调用多次，也可能一次都不被调用。
    virtual void WriteData(const uint8_t* data, size_t len, ISink& out) = 0;

    /// 结束当前条目，通常在这里补齐块对齐（如 tar 的 512 字节）。
    virtual void EndEntry(ISink& out) = 0;

    /// 全部条目写完，写出结束标记（如 tar 尾部的两个全零块）。
    virtual void Finish(ISink& out) = 0;

    // —— 解包 ——

    /// 从 src 顺序读取并解析。每解析出一条条目就调用一次 on_entry，
    /// 该条目的内容通过 on_data 分块吐出（可能调用多次或零次），
    /// 之后才允许开始下一条。
    virtual void Unpack(ISource& src, const std::function<void(const EntryMeta&)>& on_entry,
                        const std::function<void(const uint8_t*, size_t)>& on_data) = 0;
};

/// 打包器注册表。CLI 的 `cbk info` 会列出这里注册的所有名字，
/// GUI 拿这个列表填下拉框——所以新加一种算法，界面不用改一行代码。
class PackerRegistry {
public:
    using Factory = std::function<std::unique_ptr<IPacker>()>;

    static PackerRegistry& Instance();

    void Register(const std::string& name, Factory factory);
    bool Has(const std::string& name) const;
    /// 名字不存在时返回 nullptr，由调用方报参数错误（退出码 3）。
    std::unique_ptr<IPacker> Create(const std::string& name) const;
    std::vector<std::string> Names() const;

private:
    PackerRegistry() = default;
    std::map<std::string, Factory> factories_;
};

/// 注册所有内置打包器。
///
/// 故意做成一个显式函数，而不是靠每个 .cpp 里的静态对象自动注册：
/// 静态库里没有被引用到的目标文件会被链接器直接丢掉，
/// 自动注册在那种情况下会静默失效，排查起来非常痛苦。
/// 新增一种算法时，在 core/src/builtins.cpp 里加一行即可。
void RegisterBuiltinPackers();

}  // namespace cbk

#endif  // CBK_PACKER_H_
