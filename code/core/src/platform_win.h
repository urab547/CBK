// Copyright 2026 CBK Project. Win32 API 的薄封装。
//
// 这一层不含任何业务逻辑，只把 Windows 那些容易用错的调用包一层，
// 让上面的 scanner / archive / engine 不必反复处理同样的陷阱：
//   · 260 字符的 MAX_PATH 限制（要加 \\?\ 前缀，而且前缀路径不做规范化）
//   · 打开文件时缺一个共享位就会失败
//   · AdjustTokenPrivileges 部分失败也返回 TRUE
//   · HANDLE 有 nullptr 和 INVALID_HANDLE_VALUE 两种无效值
#ifndef CODE_CORE_SRC_PLATFORM_WIN_H_
#define CODE_CORE_SRC_PLATFORM_WIN_H_

#include <windows.h>

#include <cstdint>
#include <string>

namespace cbk {
namespace platform {

// ============================================================================
// 编码转换
// ============================================================================
//
// 规矩：内存里一律 std::wstring（UTF-16），只有落到字节流——写进 .cbk 容器、
// 打进 stdout 的 JSON——才转 UTF-8。Windows 的文件名本来就是 UTF-16，
// 中途来回转只会丢字符。

/// UTF-16 转 UTF-8。输入为空时返回空串。
std::string ToUtf8(const std::wstring& text);

/// UTF-8 转 UTF-16。输入为空时返回空串。
std::wstring FromUtf8(const std::string& text);

// ============================================================================
// 路径
// ============================================================================

/// 判断路径是否已经带了 \\?\ 或 \\.\ 前缀。
bool IsExtendedPath(const std::wstring& path);

/// 规范化：正斜杠换成反斜杠，转成绝对路径，消掉 "." 和 ".."，
/// 去掉结尾多余的分隔符（根目录 "C:\" 的那个除外）。
///
/// 相对路径按当前工作目录解析。已经是 \\?\ 前缀的路径**原样返回**——
/// 这种路径的语义就是"别动它"。
std::wstring NormalizePath(const std::wstring& path);

/// 转成扩展长度路径，绕过 MAX_PATH = 260 的限制。
///
///     C:\a\b            ->  \\?\C:\a\b
///     \\srv\share\a     ->  \\?\UNC\srv\share\a
///     \\?\C:\a          ->  \\?\C:\a        （幂等）
///
/// **必须先规范化再加前缀**：\\?\ 路径会被内核原样送给文件系统，
/// 不做任何规范化，里头留着 "." 或 ".." 或正斜杠都会直接失败。
/// 本函数内部已经调过 NormalizePath，调用方不用再来一遍。
std::wstring ToExtendedPath(const std::wstring& path);

/// 拼接路径，自动处理分隔符。任一侧为空时返回另一侧。
std::wstring JoinPath(const std::wstring& base, const std::wstring& child);

// ============================================================================
// 句柄
// ============================================================================

/// HANDLE 的 RAII 包装。
///
/// 注意 Win32 有两种"无效句柄"：CreateFile 失败返回 INVALID_HANDLE_VALUE，
/// 而 CreateEvent 之类失败返回 nullptr。IsValid() 两种都认。
class ScopedHandle {
public:
    ScopedHandle() = default;
    explicit ScopedHandle(HANDLE handle) : handle_(handle) {}
    ~ScopedHandle() { Reset(); }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    ScopedHandle(ScopedHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = INVALID_HANDLE_VALUE;
    }
    ScopedHandle& operator=(ScopedHandle&& other) noexcept;

    bool IsValid() const { return handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr; }
    HANDLE Get() const { return handle_; }

    /// 交出所有权，之后本对象不再负责关闭。
    HANDLE Release();

    /// 关掉当前句柄，接管新的（默认接管一个无效值，即单纯关闭）。
    void Reset(HANDLE handle = INVALID_HANDLE_VALUE);

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

/// 打开文件或目录的通用入口。path 传原始路径即可，内部会转扩展长度路径。
///
/// 三个共享位一律全开（FILE_SHARE_READ | WRITE | DELETE）：备份软件经常要读
/// 别的进程正在写的文件，少一位就直接打不开。
///
/// 一律带 FILE_FLAG_BACKUP_SEMANTICS，它有两个作用：允许拿到目录句柄
/// （设目录时间戳时必需），以及配合 SeBackupPrivilege 绕过 ACL 读取
/// 本来没有权限的文件。
///
/// @param access       GENERIC_READ / GENERIC_WRITE / FILE_WRITE_ATTRIBUTES ...
/// @param disposition  OPEN_EXISTING / CREATE_ALWAYS ...
/// @param no_follow_reparse  true 时加 FILE_FLAG_OPEN_REPARSE_POINT，
///                           拿到的是链接本身而不是它指向的目标。
///                           备份链接时必须为 true。
ScopedHandle OpenPath(const std::wstring& path, DWORD access, DWORD disposition,
                      bool no_follow_reparse);

/// 以读取方式打开，用于备份文件内容。
ScopedHandle OpenForRead(const std::wstring& path, bool no_follow_reparse);

/// 只要 FILE_WRITE_ATTRIBUTES 的句柄，用于 SetFileTime。
/// 目录也能用——这正是需要 FILE_FLAG_BACKUP_SEMANTICS 的原因。
ScopedHandle OpenForAttributeWrite(const std::wstring& path, bool no_follow_reparse);

/// 新建（或截断已有）文件用于还原内容。
ScopedHandle CreateForWrite(const std::wstring& path);

// ============================================================================
// 特权
// ============================================================================

/// 三个备份相关特权的启用结果。
///
/// 拿不到不是致命错误——以普通用户身份运行时本来就拿不到。降级继续跑，
/// 只是遇到没权限的文件会跳过并 warn。GUI 可以据此提示
/// "以管理员身份重启可以备份更多文件"。
struct PrivilegeState {
    bool backup = false;    ///< SE_BACKUP_NAME，绕过 ACL 读
    bool restore = false;   ///< SE_RESTORE_NAME，绕过 ACL 写、设置任意属主
    bool security = false;  ///< SE_SECURITY_NAME，读写 SACL

    bool AllGranted() const { return backup && restore && security; }
};

/// 给当前进程令牌启用一个特权。
///
/// 坑：AdjustTokenPrivileges **即使一个特权都没启用成功也返回 TRUE**，
/// 必须再查 GetLastError() == ERROR_NOT_ALL_ASSIGNED 才知道结果。
/// 本函数已经处理了这一点，返回值可以直接信。
bool EnablePrivilege(const wchar_t* privilege_name);

/// 一次性尝试启用三个备份特权。部分失败不算错，看返回值的各个字段。
PrivilegeState EnableBackupPrivileges();

// ============================================================================
// 错误
// ============================================================================

/// 把 GetLastError() 的错误码转成可读文本，用于 warn 事件。
/// 取不到系统描述时退化成 "Windows 错误 <码>"。
std::wstring FormatWinError(uint32_t error_code);

}  // namespace platform
}  // namespace cbk

#endif  // CODE_CORE_SRC_PLATFORM_WIN_H_
