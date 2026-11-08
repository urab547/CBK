// Copyright 2026 CBK Project. .cbk 容器的读写。
//
// 布局（全部小端）：
//
//     offset 0
//     ├─ FileHeader      固定 128 字节            明文，永不加密
//     ├─ PipelineDesc    pipeline_desc_len 字节   明文，描述用了哪些算法
//     ├─ sourceRoot      source_root_len 字节     明文，UTF-8，源根路径
//     ├─ Data Region     data_size 字节           经 Packer + Stage 链处理
//     ├─ Index Region    index_size 字节          条目索引表，同样经 Stage 链
//     └─ Footer          固定 64 字节             明文，三个 CRC32
//
// 前三段必须明文：还原时得先知道这个包用了什么压缩、什么加密，才能组装出
// 逆向 Stage 链。这段也加密就成死锁了。代价是"用了哪种加密"这个事实不受
// 保护——所有加密归档格式都是这么做的。
//
// 索引放尾部不放头部：备份是流式写的，写完最后一个文件才知道每条的
// stored_size 和偏移。放尾部就不用为每个条目回头 seek 改写。
// 只有 128 字节的头部会在最后回填一次。
#ifndef CODE_CORE_SRC_ARCHIVE_H_
#define CODE_CORE_SRC_ARCHIVE_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "cbk/sink.h"
#include "cbk/types.h"
#include "src/platform_win.h"

namespace cbk {

// ============================================================================
// PipelineDesc
// ============================================================================

/// 数据区和索引区经过的处理链，**顺序即写入时的应用顺序**。
///
/// 序列化成一行 ASCII：
///
///     packer=tar;stages=lz77,huffman,aes128-cbc
///     packer=cbk-native;stages=
///
/// 还原时按这个字符串反查两个注册表重建逆向链。名字查不到就报错退出，
/// 不猜、不跳过——猜错了产出的是一堆垃圾字节，比直接失败糟糕得多。
struct PipelineDesc {
    std::string packer;               ///< 打包器名，如 "cbk-native"
    std::vector<std::string> stages;  ///< 字节变换链，按应用顺序

    /// 转成 "packer=...;stages=a,b" 文本。
    std::string Serialize() const;

    /// 从文本解析回结构体。
    /// @return 语法不合法、或含有不该出现的字符时返回 false。
    static bool Parse(const std::string& text, PipelineDesc* out);
};

// ============================================================================
// 头部
// ============================================================================

/// FileHeader 里的字段，加上紧随其后的两段变长明文。
struct ArchiveHeader {
    uint16_t format_version = kFormatVersion;  ///< 格式版本号
    uint16_t flags = 0;                        ///< bit0 已打包 / bit1 已压缩 / bit2 已加密
    uint64_t created_at = 0;                   ///< FILETIME
    uint64_t entry_count = 0;                  ///< 条目总数
    uint64_t data_offset = 0;                  ///< 数据区从文件第几字节开始
    uint64_t data_size = 0;                    ///< 数据区字节数
    uint64_t index_offset = 0;                 ///< 索引区从文件第几字节开始
    uint64_t index_size = 0;                   ///< 索引区字节数
    uint64_t total_original_bytes = 0;         ///< 所有文件原始大小之和

    std::wstring source_root;  ///< 规范化的绝对路径，不带 \\?\ 前缀
    PipelineDesc pipeline;     ///< 用了哪些算法
};

/// flags 的三个位。
inline constexpr uint16_t kFlagPacked = 1u << 0;
inline constexpr uint16_t kFlagCompressed = 1u << 1;
inline constexpr uint16_t kFlagEncrypted = 1u << 2;
// 索引前置一条独立的源根目录元数据记录，不计入 entry_count，不进入打包数据流。
inline constexpr uint16_t kFlagSourceRootMetadata = 1u << 3;

/// source_root 转成 UTF-8 后的长度上限——头部里那个字段只有 2 字节。
///
/// 现实中撞不到（最长的路径转 UTF-8 也就十万字节量级，而且那要求整条路径
/// 全是三字节字符），但撞到时必须明确报错。静默截断会产出一个"写得出来、
/// 根路径却是错的"的包，等到 cbk list 才看出来。
inline constexpr size_t kMaxSourceRootUtf8 = 0xFFFF;

// ============================================================================
// 索引区
// ============================================================================

/// 把条目表序列化进索引区。
void WriteIndex(const std::vector<EntryMeta>& entries, ISink& out,
                const EntryMeta* source_root = nullptr);

/// 从索引区解析条目表。
///
/// @param expected_count 头部记的条目数，用于校验读出来的条数对不对得上。
/// @return 记录损坏、或条数与头部不符时返回 false。
bool ReadIndex(ISource& source, uint64_t expected_count, std::vector<EntryMeta>* out,
               bool has_source_root = false, EntryMeta* source_root = nullptr);

// ============================================================================
// 写
// ============================================================================

/// 按 Open -> DataSink -> EndData -> IndexSink -> Close 的顺序使用。
///
/// 没有成功 Close 就析构（异常、取消、忘了调）时，析构函数会删掉半成品
/// 文件。留下一个残缺的 .cbk 比没有文件更糟——它看着像个能用的备份。
class ArchiveWriter {
public:
    // 构造和析构都必须放到 .cpp 里定义，不能写 = default 留在头文件。
    // sink_ 是个指向前置声明类型的 unique_ptr，头文件里看不到 RegionSink 的
    // 完整定义，编译器在这里生成不出销毁成员的代码。
    ArchiveWriter();
    ~ArchiveWriter();

    /// 禁止拷贝：一个对象独占一个打开的文件。
    ArchiveWriter(const ArchiveWriter&) = delete;
    ArchiveWriter& operator=(const ArchiveWriter&) = delete;

    /// 创建文件，写出头部占位、PipelineDesc、源根。
    Status Open(const std::wstring& path, const std::wstring& source_root,
                const PipelineDesc& pipeline, std::wstring* error,
                bool has_source_root_metadata = false);

    /// 数据区写入端。引擎把 Packer + Stage 链的输出接到这里。
    ISink& DataSink();

    /// 数据区写完，切换到索引区。
    void EndData();

    /// 索引区写入端。
    ISink& IndexSink();

    /// 回填头部、写出尾部、关闭文件。成功之后析构不会再删文件。
    Status Close(uint64_t entry_count, uint64_t total_original_bytes, std::wstring* error);

    /// 放弃：关闭并删除半成品文件。可以重复调用。
    void Abort();

    /// 数据区已写出的字节数（经过流水线之后的）。
    uint64_t DataBytesWritten() const { return data_bytes_; }

private:
    /// 写某个区域时顺带统计字节数和 CRC 的 ISink，定义在 .cpp 里。
    class RegionSink;

    /// 写入进度：未打开 → 写数据区 → 写索引区 → 已完成。用来拦截调用顺序错误。
    enum class Phase { kClosed, kData, kIndex, kDone };

    /// 出错时统一处理：拼错误信息、删半成品，返回 kFailed。
    Status Fail(const std::wstring& what, std::wstring* error);

    platform::ScopedHandle file_;   ///< 正在写的文件句柄
    std::wstring path_;             ///< 实际写入的临时文件路径（.partial-<pid>）
    std::wstring destination_;      ///< 最终的 .cbk 路径，Close 成功后改名过去
    Phase phase_ = Phase::kClosed;  ///< 当前进度

    ArchiveHeader header_;          ///< 头部字段，Close 时回填
    std::string pipeline_text_;     ///< PipelineDesc 的文本形式
    std::string source_root_utf8_;  ///< 源根的 UTF-8 形式

    std::unique_ptr<RegionSink> sink_;  ///< 当前区域的写入端
    uint64_t data_bytes_ = 0;           ///< 数据区字节数
    uint32_t data_crc_ = 0;             ///< 数据区 CRC32
    uint64_t index_bytes_ = 0;          ///< 索引区字节数
    uint32_t index_crc_ = 0;            ///< 索引区 CRC32
};

// ============================================================================
// 读
// ============================================================================

/// 打开并解析一个 .cbk。只解析明文部分，数据区和索引区按需流式读取。
class ArchiveReader {
public:
    /// 读头部、PipelineDesc、源根、尾部，并做一致性校验。
    Status Open(const std::wstring& path, std::wstring* error);

    /// 解析出来的头部信息。
    const ArchiveHeader& Header() const { return header_; }

    /// 尾部里记的三个校验和。
    uint32_t HeaderCrc() const { return header_crc_; }
    uint32_t DataCrc() const { return data_crc_; }
    uint32_t IndexCrc() const { return index_crc_; }

    /// 数据区 / 索引区的流式读取端。各自持有独立句柄，可以同时打开。
    /// 打开失败返回 nullptr。
    std::unique_ptr<ISource> OpenData() const;
    std::unique_ptr<ISource> OpenIndex() const;

    /// 重算三个 CRC 与尾部比对。cbk verify 就是它。
    Status Verify(std::wstring* error) const;

private:
    std::wstring path_;        ///< .cbk 文件路径
    ArchiveHeader header_;     ///< 解析出来的头部
    uint32_t header_crc_ = 0;  ///< 尾部记录的头部 CRC
    uint32_t data_crc_ = 0;    ///< 尾部记录的数据区 CRC
    uint32_t index_crc_ = 0;   ///< 尾部记录的索引区 CRC
};

}  // namespace cbk

#endif  // CODE_CORE_SRC_ARCHIVE_H_
