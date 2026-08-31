// Copyright 2026 CBK Project. 数据备份软件核心类型定义。
#ifndef CBK_TYPES_H_
#define CBK_TYPES_H_

#include <cstddef>
#include <cstdint>
#include <string>

namespace cbk {

// ============================================================================
// 容器格式常量
// 改动这些值等于改变 .cbk 文件格式，必须同时升 kFormatVersion 并通知全组。
// ============================================================================

/// 文件头魔数，位于 .cbk 文件最开头 4 字节。
inline constexpr char kMagic[4] = {'C', 'B', 'K', 'F'};
/// 尾部校验块魔数。
inline constexpr char kFooterMagic[4] = {'C', 'B', 'K', 'E'};
/// 当前容器格式版本。
inline constexpr uint16_t kFormatVersion = 1;
/// 文件头固定长度（字节）。
inline constexpr size_t kFileHeaderSize = 128;
/// 尾部校验块固定长度（字节）。
inline constexpr size_t kFooterSize = 64;
/// 流式读写的缓冲块大小。任何情况下都不要把整个文件读进内存。
inline constexpr size_t kIoBlockSize = 64 * 1024;

// ============================================================================
// 文件类型
// ============================================================================

/// 备份条目的类型。Windows 上的取值参见设计文档《平台差异映射表》一节。
enum class FileType : uint8_t {
    kRegular = 0,      ///< 普通文件，有内容
    kDirectory = 1,    ///< 目录，无内容
    kSymlinkFile = 2,  ///< 指向文件的符号链接，存 link_target
    kSymlinkDir = 3,   ///< 指向目录的符号链接，存 link_target
    kJunction = 4,     ///< 目录联接（重解析点 IO_REPARSE_TAG_MOUNT_POINT）
    kHardlinkRef = 5,  ///< 硬链接，指向已存条目，不重复存内容
    kUnsupported = 6,  ///< 其它重解析点/设备，仅记录 reparse_tag，不还原
};

/// 返回类型的短名，用于日志与 JSON 输出。
const char* ToString(FileType type);

// ============================================================================
// 条目元数据
// ============================================================================

/// 一个备份条目的全部描述信息。
///
/// 由 Scanner 在备份时填充，由 IPacker 序列化进容器，
/// 还原时由 IPacker 解析出来交还给 RestoreEngine。
///
/// 注意：路径一律用 std::wstring（UTF-16）在内存中传递，
/// 只有在写入容器时才转成 UTF-8。Windows 的文件名本来就是 UTF-16，
/// 中途转来转去只会丢字符。
struct EntryMeta {
    // —— 标识 ——
    uint64_t id = 0;              ///< 条目序号，从 0 开始递增
    std::wstring relative_path;   ///< 相对源根的路径，'\\' 分隔，不以分隔符开头
    FileType type = FileType::kRegular;

    // —— 元数据（对应评分项"属主/时间/权限"）——
    uint32_t attributes = 0;        ///< FILE_ATTRIBUTE_* 位掩码
    uint64_t creation_time = 0;     ///< FILETIME，100ns 自 1601-01-01 UTC
    uint64_t last_access_time = 0;  ///< FILETIME
    uint64_t last_write_time = 0;   ///< FILETIME
    std::string sddl;               ///< 安全描述符的 SDDL 文本，含 O:/G:/D: 三段

    // —— 内容 ——
    uint64_t original_size = 0;  ///< 原始字节数；目录与链接为 0
    uint64_t stored_size = 0;    ///< 经流水线处理后的字节数，由写入方回填
    uint64_t data_offset = 0;    ///< 在数据区内的偏移，由写入方回填
    uint32_t crc32 = 0;          ///< 原始内容的 CRC32，用于 verify

    // —— 类型相关 ——
    std::wstring link_target;   ///< 符号链接/junction 的原始目标（可能是相对路径）
    bool link_is_relative = false;  ///< 符号链接是否带 SYMLINK_FLAG_RELATIVE
    uint32_t reparse_tag = 0;   ///< 重解析点标签，kUnsupported 时用于诊断
    uint64_t hardlink_ref_id = 0;  ///< kHardlinkRef 指向的条目 id
};

// ============================================================================
// 运行结果
// ============================================================================

/// 一次备份/还原的总体结果，直接映射到进程退出码。
enum class Status : int {
    kOk = 0,       ///< 全部成功
    kPartial = 1,  ///< 包是完整可用的，但有条目被跳过（产生了 warn 事件）
    kFailed = 2,   ///< 失败，产物不可用
    kBadArgs = 3,  ///< 命令行参数错误。出现这个说明调用方（通常是 GUI）有 bug
};

}  // namespace cbk

#endif  // CBK_TYPES_H_
