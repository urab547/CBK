// Copyright 2026 CBK Project. UTF-16 与 UTF-8 的互转。
//
// 放在对外契约里，是因为 core 的接口上到处是 std::wstring（路径、
// 错误信息），而容器文件和 stdout 上的 JSON 都必须是 UTF-8。
// 任何用 core 的人——CLI、测试——都需要这两个函数。
//
// 规矩：**内存里一律 std::wstring（UTF-16），只有落到字节流时才转 UTF-8。**
// Windows 的文件名本来就是 UTF-16，中途来回转只会丢字符。
#ifndef CBK_TEXT_H_
#define CBK_TEXT_H_

#include <string>

namespace cbk {

/// UTF-16 转 UTF-8。输入为空时返回空串。
std::string ToUtf8(const std::wstring& text);

/// UTF-8 转 UTF-16。输入为空时返回空串。
std::wstring FromUtf8(const std::string& text);

}  // namespace cbk

#endif  // CBK_TEXT_H_
