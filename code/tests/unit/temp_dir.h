// Copyright 2026 CBK Project. 测试用的临时目录。
//
// 只给单元测试用。真备份/还原路径上的删除逻辑不走这里。
#ifndef CODE_TESTS_UNIT_TEMP_DIR_H_
#define CODE_TESTS_UNIT_TEMP_DIR_H_

#include <windows.h>

#include <atomic>
#include <string>
#include <vector>

#include "src/platform_win.h"

namespace cbk_test {

/// 递归删掉一棵目录树。
///
/// 两个必须注意的点，删测试产物和删真实数据是一样的：
///   · 一律走 \\?\ 扩展路径，否则删不掉自己刚建出来的深目录。
///   · 遇到重解析点**不递归进去**，直接删链接本身。否则删的是链接指向的
///     真实目录，那就是在破坏测试机上的其它数据了。
inline void RemoveTreeRecursively(const std::wstring& path) {
    const std::wstring extended = cbk::platform::ToExtendedPath(path);

    const DWORD attributes = GetFileAttributesW(extended.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return;

    // 只读位会让删除失败，先摘掉。
    if ((attributes & FILE_ATTRIBUTE_READONLY) != 0) {
        SetFileAttributesW(extended.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY);
    }

    const bool is_directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    const bool is_reparse = (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;

    if (!is_directory) {
        DeleteFileW(extended.c_str());
        return;
    }
    if (is_reparse) {
        RemoveDirectoryW(extended.c_str());  // 删链接本身，不碰目标
        return;
    }

    WIN32_FIND_DATAW find_data = {};
    HANDLE find = FindFirstFileW(cbk::platform::JoinPath(extended, L"*").c_str(), &find_data);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = find_data.cFileName;
            if (name == L"." || name == L"..") continue;
            RemoveTreeRecursively(cbk::platform::JoinPath(path, name));
        } while (FindNextFileW(find, &find_data) != 0);
        FindClose(find);
    }
    RemoveDirectoryW(extended.c_str());
}

/// 建一个独一无二的临时目录，析构时整棵删掉。
class TempDir {
public:
    TempDir() {
        static std::atomic<unsigned> counter{0};
        wchar_t temp[MAX_PATH] = {};
        const DWORD len = GetTempPathW(MAX_PATH, temp);
        const std::wstring name = L"cbk_test_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
                                  std::to_wstring(counter.fetch_add(1));
        path_ = cbk::platform::JoinPath(std::wstring(temp, len), name);
        RemoveTreeRecursively(path_);  // 上一轮跑崩了留下的残骸
        CreateDirectoryW(cbk::platform::ToExtendedPath(path_).c_str(), nullptr);
    }

    ~TempDir() { RemoveTreeRecursively(path_); }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::wstring& Path() const { return path_; }

    /// 相对路径转成这个临时目录下的绝对路径。
    std::wstring At(const std::wstring& relative) const {
        return cbk::platform::JoinPath(path_, relative);
    }

    /// 建一级子目录（父目录必须已存在），返回绝对路径。
    std::wstring MakeDir(const std::wstring& relative) const {
        const std::wstring full = At(relative);
        CreateDirectoryW(cbk::platform::ToExtendedPath(full).c_str(), nullptr);
        return full;
    }

    /// 建文件并写入内容，返回绝对路径。
    std::wstring MakeFile(const std::wstring& relative, const std::string& content) const {
        const std::wstring full = At(relative);
        cbk::platform::ScopedHandle handle = cbk::platform::CreateForWrite(full);
        if (handle.IsValid() && !content.empty()) {
            DWORD written = 0;
            WriteFile(handle.Get(), content.data(), static_cast<DWORD>(content.size()), &written,
                      nullptr);
        }
        return full;
    }

    /// 读回整个文件，用于比对还原结果。
    std::string ReadFile(const std::wstring& relative) const {
        cbk::platform::ScopedHandle handle = cbk::platform::OpenForRead(At(relative), true);
        std::string content;
        if (!handle.IsValid()) return content;
        std::vector<char> buffer(64 * 1024);
        for (;;) {
            DWORD got = 0;
            if (!::ReadFile(handle.Get(), buffer.data(), static_cast<DWORD>(buffer.size()), &got,
                            nullptr) ||
                got == 0) {
                break;
            }
            content.append(buffer.data(), got);
        }
        return content;
    }

private:
    std::wstring path_;
};

}  // namespace cbk_test

#endif  // CODE_TESTS_UNIT_TEMP_DIR_H_
