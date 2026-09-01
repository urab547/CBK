// Copyright 2026 CBK Project. UTF-16 与 UTF-8 互转的实现。
#include "cbk/text.h"

#include <windows.h>

#include <string>

namespace cbk {

std::string ToUtf8(const std::wstring& text) {
    if (text.empty()) return std::string();

    // 先问长度再分配。传 -1 会把结尾的 NUL 也算进去，所以这里显式传长度。
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) return std::string();

    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &out[0], size,
                        nullptr, nullptr);
    return out;
}

std::wstring FromUtf8(const std::string& text) {
    if (text.empty()) return std::wstring();

    const int size =
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) return std::wstring();

    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &out[0], size);
    return out;
}

}  // namespace cbk
