// Copyright 2026 CBK Project. 元数据的读写。
//
// 目前覆盖属性位和三个时间戳。属主与 DACL（SDDL）是 #9，还没做。
#ifndef CODE_CORE_SRC_METADATA_H_
#define CODE_CORE_SRC_METADATA_H_

#include <windows.h>

#include <cstdint>
#include <string>

#include "cbk/types.h"

namespace cbk {

// ============================================================================
// 读
// ============================================================================
//
// 为什么需要这两个函数——WIN32_FIND_DATAW 不总是可信：
//
// FindFirstFileW 返回的元数据来自**父目录的索引项**，而 NTFS 更新这个索引项
// 是惰性的。对象自己 MFT 记录里的值（GetFileInformationByHandle 读到的那个）
// 才是权威的。
//
// 目录尤其明显：目录的 lastWriteTime 是被子项的创建/删除改的，而父目录里
// 那份索引项要过一会儿才跟上。刚往目录里写完文件就去遍历，读到的时间戳
// 能差好几毫秒——往返测试直接挂，而且看着像还原侧的 bug，其实是备份侧
// 一开始就记错了。本项目实测差过 8 毫秒。
//
// 文件一般没这个问题（关闭文件时会刷新父索引项），但正在被别的进程写的
// 文件同样会读到旧值。所以只要手上已经有句柄，就从句柄上重新读一遍。

/// 从已经打开的句柄上读权威的属性位与三个时间戳，覆盖 meta 里的对应字段。
bool ReadMetadataFromHandle(HANDLE handle, EntryMeta* meta);

/// 打开路径再读。用于手上没有现成句柄的场合，比如目录。
///
/// 打不开时返回 false，调用方应保留 WIN32_FIND_DATAW 给出的那份——
/// 陈旧的值也比没有值强。
bool ReadMetadataByPath(const std::wstring& path, bool no_follow_reparse, EntryMeta* meta);

// ============================================================================
// 写
// ============================================================================

/// 还原时允许设置的属性位。
///
/// 剩下的那些——DIRECTORY / REPARSE_POINT / COMPRESSED / ENCRYPTED /
/// SPARSE_FILE——是文件系统自己维护的，硬塞给 SetFileAttributesW 会返回
/// ERROR_INVALID_PARAMETER，而且是整个调用失败，不是"忽略这几位"。
/// 所以必须先与上这个掩码。
inline constexpr uint32_t kSettableAttributes = 0x00000001u |  // FILE_ATTRIBUTE_READONLY
                                                0x00000002u |  // FILE_ATTRIBUTE_HIDDEN
                                                0x00000004u |  // FILE_ATTRIBUTE_SYSTEM
                                                0x00000020u |  // FILE_ATTRIBUTE_ARCHIVE
                                                0x00000100u |  // FILE_ATTRIBUTE_TEMPORARY
                                                0x00001000u |  // FILE_ATTRIBUTE_OFFLINE
                                                0x00002000u;   // FILE_ATTRIBUTE_NOT_CONTENT_INDEXED

/// 设置三个时间戳。
///
/// 目录也能设——打开目录句柄靠的是 FILE_FLAG_BACKUP_SEMANTICS，
/// platform::OpenForAttributeWrite 里已经带上了。
///
/// 时间戳值为 0 表示"没记录"，对应的那一项会被跳过而不是写成 1601 年。
///
/// @param no_follow_reparse 对链接本身设置时传 true，否则设的是它指向的目标。
/// @param win_error 失败时写入 GetLastError()，可为 nullptr。
bool ApplyTimestamps(const std::wstring& path, const EntryMeta& meta, bool no_follow_reparse,
                     uint32_t* win_error);

/// 设置属性位（自动屏蔽掉不可设置的那些）。
///
/// **这一步必须放在最后。** 先设了 FILE_ATTRIBUTE_READONLY 的话，
/// 后续再去设时间戳和 ACL 都会失败——而且失败得很安静，只是元数据没生效。
///
/// @param win_error 失败时写入 GetLastError()，可为 nullptr。
bool ApplyAttributes(const std::wstring& path, const EntryMeta& meta, uint32_t* win_error);

}  // namespace cbk

#endif  // CODE_CORE_SRC_METADATA_H_
