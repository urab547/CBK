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

    std::string Serialize() const;

    /// @return 语法不合法、或含有不该出现的字符时返回 false。
    static bool Parse(const std::string& text, PipelineDesc* out);
};

// ============================================================================
// 头部
// ============================================================================

/// FileHeader 里的字段，加上紧随其后的两段变长明文。
struct ArchiveHeader {
    uint16_t format_version = kFormatVersion;
    uint16_t flags = 0;       ///< bit0 已打包 / bit1 已压缩 / bit2 已加密
    uint64_t created_at = 0;  ///< FILETIME
    uint64_t entry_count = 0;
    uint64_t data_offset = 0;
    uint64_t data_size = 0;
    uint64_t index_offset = 0;
    uint64_t index_size = 0;
    uint64_t total_original_bytes = 0;

    std::wstring source_root;  ///< 规范化的绝对路径，不带 \\?\ 前缀
    PipelineDesc pipeline;
};

/// flags 的三个位。
inline constexpr uint16_t kFlagPacked = 1u << 0;
inline constexpr uint16_t kFlagCompressed = 1u << 1;
inline constexpr uint16_t kFlagEncrypted = 1u << 2;

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
void WriteIndex(const std::vector<EntryMeta>& entries, ISink& out);

/// 从索引区解析条目表。
///
/// @param expected_count 头部记的条目数，用于校验读出来的条数对不对得上。
/// @return 记录损坏、或条数与头部不符时返回 false。
bool ReadIndex(ISource& source, uint64_t expected_count, std::vector<EntryMeta>* out);

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

    ArchiveWriter(const ArchiveWriter&) = delete;
    ArchiveWriter& operator=(const ArchiveWriter&) = delete;

    /// 创建文件，写出头部占位、PipelineDesc、源根。
    Status Open(const std::wstring& path, const std::wstring& source_root,
                const PipelineDesc& pipeline, std::wstring* error);

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
    class RegionSink;

    enum class Phase { kClosed, kData, kIndex, kDone };

    Status Fail(const std::wstring& what, std::wstring* error);

    platform::ScopedHandle file_;
    std::wstring path_;
    Phase phase_ = Phase::kClosed;

    ArchiveHeader header_;
    std::string pipeline_text_;
    std::string source_root_utf8_;

    std::unique_ptr<RegionSink> sink_;
    uint64_t data_bytes_ = 0;
    uint32_t data_crc_ = 0;
    uint64_t index_bytes_ = 0;
    uint32_t index_crc_ = 0;
};

// ============================================================================
// 读
// ============================================================================

/// 打开并解析一个 .cbk。只解析明文部分，数据区和索引区按需流式读取。
class ArchiveReader {
public:
    /// 读头部、PipelineDesc、源根、尾部，并做一致性校验。
    Status Open(const std::wstring& path, std::wstring* error);

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
    std::wstring path_;
    ArchiveHeader header_;
    uint32_t header_crc_ = 0;
    uint32_t data_crc_ = 0;
    uint32_t index_crc_ = 0;
};

}  // namespace cbk

#endif  // CODE_CORE_SRC_ARCHIVE_H_
