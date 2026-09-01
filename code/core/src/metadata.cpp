// Copyright 2026 CBK Project. 元数据读写的实现。
#include "src/metadata.h"

#include <string>

#include "src/platform_win.h"

namespace cbk {

namespace {

uint64_t FromFileTime(const FILETIME& time) {
    return (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

FILETIME ToFileTime(uint64_t value) {
    FILETIME time;
    time.dwLowDateTime = static_cast<DWORD>(value & 0xFFFFFFFFull);
    time.dwHighDateTime = static_cast<DWORD>(value >> 32);
    return time;
}

void Record(uint32_t* win_error) {
    if (win_error != nullptr) *win_error = GetLastError();
}

}  // namespace

bool ReadMetadataFromHandle(HANDLE handle, EntryMeta* meta) {
    BY_HANDLE_FILE_INFORMATION info = {};
    if (GetFileInformationByHandle(handle, &info) == 0) return false;

    meta->attributes = info.dwFileAttributes;
    meta->creation_time = FromFileTime(info.ftCreationTime);
    meta->last_access_time = FromFileTime(info.ftLastAccessTime);
    meta->last_write_time = FromFileTime(info.ftLastWriteTime);
    return true;
}

bool ReadMetadataByPath(const std::wstring& path, bool no_follow_reparse, EntryMeta* meta) {
    // 只要读元数据，不要 GENERIC_READ——那会在没有读权限的对象上失败。
    // FILE_READ_ATTRIBUTES 配合 FILE_FLAG_BACKUP_SEMANTICS 连目录都能开。
    platform::ScopedHandle handle =
        platform::OpenPath(path, FILE_READ_ATTRIBUTES, OPEN_EXISTING, no_follow_reparse);
    if (!handle.IsValid()) return false;
    return ReadMetadataFromHandle(handle.Get(), meta);
}

bool ApplyTimestamps(const std::wstring& path, const EntryMeta& meta, bool no_follow_reparse,
                     uint32_t* win_error) {
    platform::ScopedHandle handle = platform::OpenForAttributeWrite(path, no_follow_reparse);
    if (!handle.IsValid()) {
        Record(win_error);
        return false;
    }

    // 0 表示这一项没记录（比如包是老版本写的）。传 nullptr 给 SetFileTime
    // 就是"这一项别动"，比写成 1601-01-01 强。
    const FILETIME creation = ToFileTime(meta.creation_time);
    const FILETIME access = ToFileTime(meta.last_access_time);
    const FILETIME write = ToFileTime(meta.last_write_time);

    const BOOL ok = SetFileTime(handle.Get(), meta.creation_time != 0 ? &creation : nullptr,
                                meta.last_access_time != 0 ? &access : nullptr,
                                meta.last_write_time != 0 ? &write : nullptr);
    if (ok == 0) {
        Record(win_error);
        return false;
    }
    return true;
}

bool ApplyAttributes(const std::wstring& path, const EntryMeta& meta, uint32_t* win_error) {
    const DWORD attributes = static_cast<DWORD>(meta.attributes & kSettableAttributes);

    // 一位都不用设时就别调了：SetFileAttributesW 不接受 0，
    // 得传 FILE_ATTRIBUTE_NORMAL，而那会把已有的位全清掉。
    if (attributes == 0) return true;

    if (SetFileAttributesW(platform::ToExtendedPath(path).c_str(), attributes) == 0) {
        Record(win_error);
        return false;
    }
    return true;
}

}  // namespace cbk
