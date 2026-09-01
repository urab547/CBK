// Copyright 2026 CBK Project. Win32 薄封装的实现。
#include "src/platform_win.h"

#include <string>
#include <vector>

namespace cbk {
namespace platform {

namespace {

/// \\?\ 与 \\.\ 前缀都是 4 个字符。
constexpr size_t kPrefixLen = 4;

/// 判断是不是 "C:\" 这种盘符根，或者 UNC 的 "\\" 开头。
bool IsUncPath(const std::wstring& path) {
    return path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\';
}

}  // namespace

// ---------------------------------------------------------------- 编码转换

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

// -------------------------------------------------------------------- 路径

bool IsExtendedPath(const std::wstring& path) {
    if (path.size() < kPrefixLen) return false;
    if (path[0] != L'\\' || path[1] != L'\\' || path[3] != L'\\') return false;
    return path[2] == L'?' || path[2] == L'.';
}

std::wstring NormalizePath(const std::wstring& path) {
    if (path.empty()) return std::wstring();
    // \\?\ 路径的语义就是"原样送给文件系统"，动它反而会出错。
    if (IsExtendedPath(path)) return path;

    std::wstring slashed = path;
    for (wchar_t& c : slashed) {
        if (c == L'/') c = L'\\';
    }

    // GetFullPathNameW 顺手做了三件事：补全成绝对路径、消掉 "." 和 ".."、
    // 折叠重复的分隔符。自己手写这段逻辑很容易在 UNC 和盘符相对路径上出错。
    const DWORD needed = GetFullPathNameW(slashed.c_str(), 0, nullptr, nullptr);
    if (needed == 0) return slashed;  // 拿不到就退回原值，交给上层去失败并报错

    std::vector<wchar_t> buffer(needed);
    const DWORD written = GetFullPathNameW(slashed.c_str(), needed, buffer.data(), nullptr);
    if (written == 0 || written >= needed) return slashed;

    std::wstring full(buffer.data(), written);

    // 去掉结尾多余的分隔符，但保留 "C:\" 这种根路径本身的那个。
    while (full.size() > 3 && full.back() == L'\\') {
        full.pop_back();
    }
    return full;
}

std::wstring ToExtendedPath(const std::wstring& path) {
    if (path.empty()) return std::wstring();
    if (IsExtendedPath(path)) return path;  // 幂等

    const std::wstring full = NormalizePath(path);
    if (IsUncPath(full)) {
        // \\srv\share\a  ->  \\?\UNC\srv\share\a
        // 去掉开头两个反斜杠里的一个，接在 \\?\UNC 后面。
        return std::wstring(L"\\\\?\\UNC") + full.substr(1);
    }
    return std::wstring(L"\\\\?\\") + full;
}

std::wstring JoinPath(const std::wstring& base, const std::wstring& child) {
    if (base.empty()) return child;
    if (child.empty()) return base;

    std::wstring out = base;
    if (out.back() != L'\\') out.push_back(L'\\');
    size_t start = 0;
    while (start < child.size() && child[start] == L'\\') ++start;
    out.append(child, start, std::wstring::npos);
    return out;
}

// -------------------------------------------------------------------- 句柄

ScopedHandle& ScopedHandle::operator=(ScopedHandle&& other) noexcept {
    if (this != &other) {
        Reset(other.handle_);
        other.handle_ = INVALID_HANDLE_VALUE;
    }
    return *this;
}

HANDLE ScopedHandle::Release() {
    HANDLE released = handle_;
    handle_ = INVALID_HANDLE_VALUE;
    return released;
}

void ScopedHandle::Reset(HANDLE handle) {
    if (IsValid()) CloseHandle(handle_);
    handle_ = handle;
}

ScopedHandle OpenPath(const std::wstring& path, DWORD access, DWORD disposition,
                      bool no_follow_reparse) {
    // FILE_FLAG_BACKUP_SEMANTICS 必须有：既允许打开目录句柄，也配合
    // SeBackupPrivilege 绕过 ACL。FILE_FLAG_SEQUENTIAL_SCAN 只是给缓存
    // 管理器的预读提示，对写入无害。
    DWORD flags = FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_SEQUENTIAL_SCAN;
    if (no_follow_reparse) flags |= FILE_FLAG_OPEN_REPARSE_POINT;

    // 三个共享位全开。少任何一个，别的进程正在写的文件就打不开。
    const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

    HANDLE handle = CreateFileW(ToExtendedPath(path).c_str(), access, share, nullptr, disposition,
                                flags, nullptr);
    return ScopedHandle(handle);
}

ScopedHandle OpenForRead(const std::wstring& path, bool no_follow_reparse) {
    return OpenPath(path, GENERIC_READ, OPEN_EXISTING, no_follow_reparse);
}

ScopedHandle OpenForAttributeWrite(const std::wstring& path, bool no_follow_reparse) {
    return OpenPath(path, FILE_WRITE_ATTRIBUTES, OPEN_EXISTING, no_follow_reparse);
}

ScopedHandle CreateForWrite(const std::wstring& path) {
    return OpenPath(path, GENERIC_WRITE, CREATE_ALWAYS, true);
}

// -------------------------------------------------------------------- 特权

bool EnablePrivilege(const wchar_t* privilege_name) {
    HANDLE raw_token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &raw_token)) {
        return false;
    }
    ScopedHandle token(raw_token);

    LUID luid = {};
    if (!LookupPrivilegeValueW(nullptr, privilege_name, &luid)) return false;

    TOKEN_PRIVILEGES privileges = {};
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Luid = luid;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    // 这里是经典坑：AdjustTokenPrivileges 即使一个特权都没启用成功
    // 也会返回 TRUE，必须靠 GetLastError 区分。而它在完全成功时不一定
    // 会写 last error，所以先手动清零。
    SetLastError(ERROR_SUCCESS);
    if (!AdjustTokenPrivileges(token.Get(), FALSE, &privileges, sizeof(privileges), nullptr,
                               nullptr)) {
        return false;
    }
    return GetLastError() != ERROR_NOT_ALL_ASSIGNED;
}

PrivilegeState EnableBackupPrivileges() {
    PrivilegeState state;
    state.backup = EnablePrivilege(SE_BACKUP_NAME);
    state.restore = EnablePrivilege(SE_RESTORE_NAME);
    state.security = EnablePrivilege(SE_SECURITY_NAME);
    return state;
}

// -------------------------------------------------------------------- 错误

std::wstring FormatWinError(uint32_t error_code) {
    LPWSTR buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error_code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);

    if (length == 0 || buffer == nullptr) {
        if (buffer != nullptr) LocalFree(buffer);
        return L"Windows 错误 " + std::to_wstring(error_code);
    }

    std::wstring message(buffer, length);
    LocalFree(buffer);

    // 系统描述结尾带 "\r\n"，拼进日志里会把一行拆成三行。
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n')) {
        message.pop_back();
    }
    return message;
}

}  // namespace platform
}  // namespace cbk
