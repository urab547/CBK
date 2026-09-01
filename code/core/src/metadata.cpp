// Copyright 2026 CBK Project. 元数据读写的实现。
#include "src/metadata.h"

#include <aclapi.h>
#include <sddl.h>

#include <string>

#include "cbk/text.h"
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

// ---------------------------------------------------------------- 安全描述符

namespace {

/// LocalFree 的 RAII 包装。安全描述符相关的 API 一律用 LocalAlloc 分配，
/// 早退分支多，忘一个就漏内存。
template <typename T>
class LocalPtr {
public:
    ~LocalPtr() {
        if (value_ != nullptr) LocalFree(value_);
    }
    T* Receive() { return &value_; }
    T Get() const { return value_; }

private:
    T value_ = nullptr;
};

/// 我们关心的三段。SACL 不读——那要 SeSecurityPrivilege，且与评分项无关。
constexpr SECURITY_INFORMATION kSections =
    OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;

}  // namespace

bool ReadSecurityDescriptor(const std::wstring& path, std::string* sddl, uint32_t* win_error) {
    const std::wstring extended = platform::ToExtendedPath(path);

    // psd 指向的是一整块自描述的内存，里面的 owner/group/dacl 指针都指向
    // 它内部，所以只需要释放 psd 这一个。
    LocalPtr<PSECURITY_DESCRIPTOR> descriptor;
    const DWORD status = GetNamedSecurityInfoW(extended.c_str(), SE_FILE_OBJECT, kSections, nullptr,
                                               nullptr, nullptr, nullptr, descriptor.Receive());
    if (status != ERROR_SUCCESS) {
        if (win_error != nullptr) *win_error = status;
        return false;
    }

    LocalPtr<LPWSTR> text;
    if (ConvertSecurityDescriptorToStringSecurityDescriptorW(
            descriptor.Get(), SDDL_REVISION_1, kSections, text.Receive(), nullptr) == 0) {
        Record(win_error);
        return false;
    }
    *sddl = ToUtf8(text.Get());
    return true;
}

bool ApplySecurityDescriptor(const std::wstring& path, const std::string& sddl, bool* owner_skipped,
                             uint32_t* win_error) {
    if (owner_skipped != nullptr) *owner_skipped = false;
    if (sddl.empty()) return true;  // 备份时就没读到，没什么可还原的

    LocalPtr<PSECURITY_DESCRIPTOR> descriptor;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
            FromUtf8(sddl).c_str(), SDDL_REVISION_1, descriptor.Receive(), nullptr) == 0) {
        Record(win_error);
        return false;
    }

    // SetNamedSecurityInfoW 要的是拆开的各段指针，不是整块描述符。
    PSID owner = nullptr;
    PSID group = nullptr;
    PACL dacl = nullptr;
    BOOL defaulted = FALSE;
    BOOL dacl_present = FALSE;
    GetSecurityDescriptorOwner(descriptor.Get(), &owner, &defaulted);
    GetSecurityDescriptorGroup(descriptor.Get(), &group, &defaulted);
    GetSecurityDescriptorDacl(descriptor.Get(), &dacl_present, &dacl, &defaulted);

    // 只设 SDDL 里真有的段。传了 OWNER_SECURITY_INFORMATION 却给个空属主，
    // 整个调用会失败——连带 DACL 也设不上。
    SECURITY_INFORMATION sections = 0;
    if (owner != nullptr) sections |= OWNER_SECURITY_INFORMATION;
    if (group != nullptr) sections |= GROUP_SECURITY_INFORMATION;
    if (dacl_present) sections |= DACL_SECURITY_INFORMATION;
    if (sections == 0) return true;  // 三段都没有，没什么可设的

    // 继承开关必须显式给：SetNamedSecurityInfoW 不看 SDDL 里的 `P`。
    // 不传的话，还原出来的目录会去继承目标位置的权限，跟源端对不上。
    SECURITY_INFORMATION inheritance = 0;
    if (dacl_present) {
        SECURITY_DESCRIPTOR_CONTROL control = 0;
        DWORD revision = 0;
        const bool protected_dacl =
            GetSecurityDescriptorControl(descriptor.Get(), &control, &revision) != 0 &&
            (control & SE_DACL_PROTECTED) != 0;
        inheritance = protected_dacl ? PROTECTED_DACL_SECURITY_INFORMATION
                                     : UNPROTECTED_DACL_SECURITY_INFORMATION;
    }

    const std::wstring extended = platform::ToExtendedPath(path);
    DWORD status = SetNamedSecurityInfoW(const_cast<LPWSTR>(extended.c_str()), SE_FILE_OBJECT,
                                         sections | inheritance, owner, group, dacl, nullptr);
    if (status == ERROR_SUCCESS) return true;
    if ((sections & OWNER_SECURITY_INFORMATION) == 0 &&
        (sections & GROUP_SECURITY_INFORMATION) == 0) {
        // 本来就没打算设属主，失败跟特权无关，不用再退一步试。
        if (win_error != nullptr) *win_error = status;
        return false;
    }

    // 设属主要 SeRestorePrivilege，普通用户只能把属主设成自己或自己所属的组。
    // 失败的是整个调用——连 DACL 都一起没设上。退一步只设 DACL 再试一次，
    // 拿到大部分权限总比一点都不还原强。
    const DWORD first_error = status;
    status = SetNamedSecurityInfoW(const_cast<LPWSTR>(extended.c_str()), SE_FILE_OBJECT,
                                   DACL_SECURITY_INFORMATION | inheritance, nullptr, nullptr, dacl,
                                   nullptr);
    if (status == ERROR_SUCCESS) {
        if (owner_skipped != nullptr) *owner_skipped = true;
        if (win_error != nullptr) *win_error = first_error;
        return true;
    }
    if (win_error != nullptr) *win_error = status;
    return false;
}

}  // namespace cbk
