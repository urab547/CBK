// Copyright 2026 CBK Project. 重解析点（符号链接与目录联接）的读写。
//
// Windows 把符号链接和 junction 都实现成"重解析点"：目录项照常存在，
// 但带一个 FILE_ATTRIBUTE_REPARSE_POINT 标志和一段附加数据，附加数据里
// 记着它指向哪儿。读写这段数据要走 DeviceIoControl，没有现成的高层 API。
//
// ## 为什么不用 GetFinalPathNameByHandle
//
// 那个函数会把链接一路解析到最终目标，返回一个规范化的绝对路径——
// 正好把我们要备份的信息弄丢了。符号链接的语义就是"存了一个字符串"，
// 存的是相对路径就得原样保留相对路径，解析成绝对路径之后还原到别的
// 机器上就废了。
//
// ## REPARSE_DATA_BUFFER 为什么要自己声明
//
// 这个结构体定义在 ntifs.h 里，那是驱动开发用的头文件，不在普通的
// Windows SDK 里。用户态程序要用它只能自己照着文档声明一遍——
// 这是处理重解析点的标准做法，不是什么歪招。
#ifndef CODE_CORE_SRC_REPARSE_H_
#define CODE_CORE_SRC_REPARSE_H_

#include <windows.h>

#include <cstdint>
#include <string>

#include "cbk/types.h"

namespace cbk {

/// 从重解析点读出来的信息。
struct ReparseInfo {
    uint32_t tag = 0;  ///< IO_REPARSE_TAG_SYMLINK / _MOUNT_POINT / 其它
    FileType type = FileType::kUnsupported;
    std::wstring target;       ///< 原始目标字符串，可能是相对路径
    bool is_relative = false;  ///< 仅符号链接有意义
};

/// 读一个重解析点。
///
/// 存的是 PrintName（用户可见形式，如 `C:\目标`）而不是 SubstituteName
/// （NT 内核形式，如 `\??\C:\目标`）。PrintName 为空时退回 SubstituteName
/// 并剥掉 `\??\` 前缀——有些工具建出来的链接确实只有 SubstituteName。
///
/// @param win_error 失败时写入 GetLastError()，可为 nullptr。
bool ReadReparsePoint(const std::wstring& path, ReparseInfo* out, uint32_t* win_error);

/// 建一个符号链接。
///
/// 会先带 SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE 试一次：Win10/11
/// 开了开发者模式的话，普通用户就能建符号链接。老系统不认这个标志，
/// 会直接报 ERROR_INVALID_PARAMETER，所以失败之后不带该标志再试一次。
///
/// 两次都失败通常是 ERROR_PRIVILEGE_NOT_HELD——既不是管理员也没开
/// 开发者模式。调用方应该降级处理并 warn，不要中断整个还原。
///
/// @param directory 目标是目录时为 true，决定加不加 SYMBOLIC_LINK_FLAG_DIRECTORY。
///                  判错了建出来的链接是坏的。
bool CreateSymlink(const std::wstring& link_path, const std::wstring& target, bool directory,
                   uint32_t* win_error);

/// 建一个目录联接（junction）。
///
/// 分两步：先建一个空目录，再往它身上写重解析点数据。
/// **不需要管理员权限，也不需要开发者模式**——这是 junction 比符号链接
/// 方便的地方，也是为什么它值得单独支持而不是当成符号链接的近似。
///
/// junction 只能指向本地绝对路径。target 传相对路径会被转成绝对的。
bool CreateJunction(const std::wstring& link_path, const std::wstring& target, uint32_t* win_error);

}  // namespace cbk

#endif  // CODE_CORE_SRC_REPARSE_H_
