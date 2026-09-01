// Copyright 2026 CBK Project. 重解析点读写的实现。
#include "src/reparse.h"

#include <winioctl.h>

#include <cstring>
#include <string>
#include <vector>

#include "src/platform_win.h"

namespace cbk {

namespace {

/// ntifs.h 里的结构体，用户态 SDK 不带，照文档自己声明一遍。
///
/// 布局要点：PathBuffer 里连着放两个字符串（SubstituteName 和 PrintName），
/// 各自的位置由前面那两对 Offset/Length 指出来，单位是**字节不是字符**。
struct ReparseDataBuffer {
    ULONG ReparseTag;
    USHORT ReparseDataLength;
    USHORT Reserved;
    union {
        struct {
            USHORT SubstituteNameOffset;
            USHORT SubstituteNameLength;
            USHORT PrintNameOffset;
            USHORT PrintNameLength;
            ULONG Flags;
            WCHAR PathBuffer[1];
        } SymbolicLink;
        struct {
            USHORT SubstituteNameOffset;
            USHORT SubstituteNameLength;
            USHORT PrintNameOffset;
            USHORT PrintNameLength;
            WCHAR PathBuffer[1];
        } MountPoint;
        struct {
            UCHAR DataBuffer[1];
        } Generic;
    };
};

/// ReparseTag + ReparseDataLength + Reserved。
constexpr size_t kReparseHeaderSize = 8;

/// 相对符号链接的标志位。winnt.h 里有，但某些 SDK 版本缺，自己兜一个。
#ifndef SYMLINK_FLAG_RELATIVE
constexpr ULONG kSymlinkFlagRelative = 0x00000001;
#else
constexpr ULONG kSymlinkFlagRelative = SYMLINK_FLAG_RELATIVE;
#endif

/// NT 内核路径前缀。SubstituteName 里是 `\??\C:\x` 这种形式。
const wchar_t kNtPrefix[] = L"\\??\\";
constexpr size_t kNtPrefixLen = 4;

void Record(uint32_t* win_error) {
    if (win_error != nullptr) *win_error = GetLastError();
}

/// 从 PathBuffer 里按字节偏移和字节长度取一个字符串。
std::wstring Slice(const WCHAR* path_buffer, USHORT byte_offset, USHORT byte_length) {
    if (byte_length == 0) return std::wstring();
    const WCHAR* start = path_buffer + (byte_offset / sizeof(WCHAR));
    return std::wstring(start, byte_length / sizeof(WCHAR));
}

/// 剥掉 `\??\` 前缀，把 NT 形式还原成用户可见形式。
std::wstring StripNtPrefix(const std::wstring& text) {
    if (text.size() > kNtPrefixLen && text.compare(0, kNtPrefixLen, kNtPrefix) == 0) {
        return text.substr(kNtPrefixLen);
    }
    return text;
}

}  // namespace

bool ReadReparsePoint(const std::wstring& path, ReparseInfo* out, uint32_t* win_error) {
    // 必须带 FILE_FLAG_OPEN_REPARSE_POINT，否则打开的是链接指向的目标，
    // 拿到的重解析点数据就是目标的（如果目标本身也是链接）或者干脆没有。
    platform::ScopedHandle handle =
        platform::OpenPath(path, FILE_READ_ATTRIBUTES, OPEN_EXISTING, true);
    if (!handle.IsValid()) {
        Record(win_error);
        return false;
    }

    std::vector<uint8_t> buffer(MAXIMUM_REPARSE_DATA_BUFFER_SIZE);
    DWORD returned = 0;
    if (DeviceIoControl(handle.Get(), FSCTL_GET_REPARSE_POINT, nullptr, 0, buffer.data(),
                        static_cast<DWORD>(buffer.size()), &returned, nullptr) == 0) {
        Record(win_error);
        return false;
    }
    if (returned < kReparseHeaderSize) {
        if (win_error != nullptr) *win_error = ERROR_INVALID_DATA;
        return false;
    }

    const ReparseDataBuffer* data = reinterpret_cast<const ReparseDataBuffer*>(buffer.data());
    out->tag = data->ReparseTag;
    out->is_relative = false;

    switch (data->ReparseTag) {
        case IO_REPARSE_TAG_SYMLINK: {
            const auto& link = data->SymbolicLink;
            out->is_relative = (link.Flags & kSymlinkFlagRelative) != 0;
            // PrintName 是用户可见形式，优先用它。
            out->target = Slice(link.PathBuffer, link.PrintNameOffset, link.PrintNameLength);
            if (out->target.empty()) {
                out->target = StripNtPrefix(
                    Slice(link.PathBuffer, link.SubstituteNameOffset, link.SubstituteNameLength));
            }
            // 目标是文件还是目录，看的是链接自身的属性位，不是目标的——
            // 目标可能根本不存在（悬空链接），但链接的类型是建的时候就定死的。
            const DWORD attributes = GetFileAttributesW(platform::ToExtendedPath(path).c_str());
            const bool is_directory = attributes != INVALID_FILE_ATTRIBUTES &&
                                      (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            out->type = is_directory ? FileType::kSymlinkDir : FileType::kSymlinkFile;
            return true;
        }
        case IO_REPARSE_TAG_MOUNT_POINT: {
            const auto& mount = data->MountPoint;
            out->target = Slice(mount.PathBuffer, mount.PrintNameOffset, mount.PrintNameLength);
            if (out->target.empty()) {
                out->target = StripNtPrefix(Slice(mount.PathBuffer, mount.SubstituteNameOffset,
                                                  mount.SubstituteNameLength));
            }
            out->type = FileType::kJunction;
            return true;
        }
        default:
            // OneDrive 占位符、AppExecLink 之类。记下标签就够了，不还原。
            out->type = FileType::kUnsupported;
            out->target.clear();
            return true;
    }
}

bool CreateSymlink(const std::wstring& link_path, const std::wstring& target, bool directory,
                   uint32_t* win_error) {
    const std::wstring extended_link = platform::ToExtendedPath(link_path);
    const DWORD base_flags = directory ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0u;

    // 目标原样传进去——相对链接就得保持相对，转成绝对路径的话，
    // 还原到别的机器或别的目录下就指错地方了。
    const DWORD unprivileged = 0x2;  // SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
    if (CreateSymbolicLinkW(extended_link.c_str(), target.c_str(), base_flags | unprivileged) !=
        0) {
        return true;
    }
    // 老系统不认 0x2，会直接报 ERROR_INVALID_PARAMETER。去掉再试一次。
    if (CreateSymbolicLinkW(extended_link.c_str(), target.c_str(), base_flags) != 0) {
        return true;
    }
    Record(win_error);
    return false;
}

bool CreateJunction(const std::wstring& link_path, const std::wstring& target,
                    uint32_t* win_error) {
    // junction 只能指向本地绝对路径，所以这里必须规范化成绝对的。
    // 这跟符号链接不同——符号链接允许相对目标，原样保留。
    const std::wstring absolute_target = platform::NormalizePath(target);
    const std::wstring substitute = std::wstring(kNtPrefix) + absolute_target;
    const std::wstring& print = absolute_target;

    const std::wstring extended_link = platform::ToExtendedPath(link_path);
    if (CreateDirectoryW(extended_link.c_str(), nullptr) == 0 &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        Record(win_error);
        return false;
    }

    // 两个字符串连着放，各自带一个结尾 NUL。这是 mklink /J 生成的布局，
    // 照着来兼容性最好。
    const size_t substitute_bytes = substitute.size() * sizeof(WCHAR);
    const size_t print_bytes = print.size() * sizeof(WCHAR);
    const size_t path_bytes = substitute_bytes + sizeof(WCHAR) + print_bytes + sizeof(WCHAR);
    const size_t mount_fields = 4 * sizeof(USHORT);
    const size_t total = kReparseHeaderSize + mount_fields + path_bytes;
    if (total > MAXIMUM_REPARSE_DATA_BUFFER_SIZE) {
        if (win_error != nullptr) *win_error = ERROR_FILENAME_EXCED_RANGE;
        return false;
    }

    std::vector<uint8_t> buffer(total, 0);
    ReparseDataBuffer* data = reinterpret_cast<ReparseDataBuffer*>(buffer.data());
    data->ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
    data->ReparseDataLength = static_cast<USHORT>(mount_fields + path_bytes);
    data->Reserved = 0;
    data->MountPoint.SubstituteNameOffset = 0;
    data->MountPoint.SubstituteNameLength = static_cast<USHORT>(substitute_bytes);
    data->MountPoint.PrintNameOffset = static_cast<USHORT>(substitute_bytes + sizeof(WCHAR));
    data->MountPoint.PrintNameLength = static_cast<USHORT>(print_bytes);
    std::memcpy(data->MountPoint.PathBuffer, substitute.c_str(), substitute_bytes + sizeof(WCHAR));
    std::memcpy(
        reinterpret_cast<uint8_t*>(data->MountPoint.PathBuffer) + substitute_bytes + sizeof(WCHAR),
        print.c_str(), print_bytes + sizeof(WCHAR));

    // 写重解析点要能写这个目录。仍然要带 FILE_FLAG_OPEN_REPARSE_POINT，
    // 否则打开的是它指向的地方（虽然此刻还没指向任何地方）。
    platform::ScopedHandle handle =
        platform::OpenPath(link_path, GENERIC_WRITE, OPEN_EXISTING, true);
    if (!handle.IsValid()) {
        Record(win_error);
        RemoveDirectoryW(extended_link.c_str());
        return false;
    }

    DWORD returned = 0;
    if (DeviceIoControl(handle.Get(), FSCTL_SET_REPARSE_POINT, buffer.data(),
                        static_cast<DWORD>(buffer.size()), nullptr, 0, &returned, nullptr) == 0) {
        Record(win_error);
        handle.Reset();
        // 别留一个空目录冒充 junction——那比没有更误导人。
        RemoveDirectoryW(extended_link.c_str());
        return false;
    }
    return true;
}

}  // namespace cbk
