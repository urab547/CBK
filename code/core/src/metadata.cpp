// Copyright 2026 CBK Project. 元数据读写的实现。
#include "src/metadata.h"

#include <aclapi.h>
#include <sddl.h>

#include <string>

#include "cbk/text.h"
#include "src/platform_win.h"

namespace cbk {

namespace {

/// FILETIME（两个 32 位半截）→ 一个 64 位整数。单位仍是 100 纳秒，不丢精度。
uint64_t FromFileTime(const FILETIME& time) {
    return (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

/// 上面的逆操作：64 位整数 → FILETIME，交给 SetFileTime 用。
FILETIME ToFileTime(uint64_t value) {
    FILETIME time;
    time.dwLowDateTime = static_cast<DWORD>(value & 0xFFFFFFFFull);
    time.dwHighDateTime = static_cast<DWORD>(value >> 32);
    return time;
}

/// 把刚才失败的 Win32 错误码交给调用方（可为 nullptr）。
void Record(uint32_t* win_error) {
    if (win_error != nullptr) *win_error = GetLastError();
}

}  // namespace

/// 从一个已打开的句柄读属性位和三个时间戳。
/// 备份引擎读文件内容时已经开着句柄，顺手用这个，不必再开一次。
bool ReadMetadataFromHandle(HANDLE handle, EntryMeta* meta) {
    BY_HANDLE_FILE_INFORMATION info = {};
    if (GetFileInformationByHandle(handle, &info) == 0) return false;

    meta->attributes = info.dwFileAttributes;
    meta->creation_time = FromFileTime(info.ftCreationTime);
    meta->last_access_time = FromFileTime(info.ftLastAccessTime);
    meta->last_write_time = FromFileTime(info.ftLastWriteTime);
    return true;
}

/// 按路径读属性位和时间戳：自己开一个只读属性的句柄，再转给上面那个函数。
bool ReadMetadataByPath(const std::wstring& path, bool no_follow_reparse, EntryMeta* meta) {
    // 只要读元数据，不要 GENERIC_READ——那会在没有读权限的对象上失败。
    // FILE_READ_ATTRIBUTES 配合 FILE_FLAG_BACKUP_SEMANTICS 连目录都能开。
    platform::ScopedHandle handle =
        platform::OpenPath(path, FILE_READ_ATTRIBUTES, OPEN_EXISTING, no_follow_reparse);
    if (!handle.IsValid()) return false;
    return ReadMetadataFromHandle(handle.Get(), meta);
}

/// 还原三个时间戳。
///
/// 只要 FILE_WRITE_ATTRIBUTES 权限，不要 GENERIC_WRITE。
/// 权限要得越少，能成功的对象越多——比如一个我们只有"写属性"权限、
/// 没有写数据权限的文件，要 GENERIC_WRITE 会直接失败，而它的时间戳
/// 本来是设得上的。
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

/// 还原属性位（只读、隐藏、系统……）。
/// 先和 kSettableAttributes 相与，把目录、重解析点、压缩这类由文件系统维护、
/// 不能手动设置的位滤掉，否则 SetFileAttributesW 会报参数错误。
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
    /// 离开作用域时自动 LocalFree。
    ~LocalPtr() {
        if (value_ != nullptr) LocalFree(value_);
    }
    /// 交给 Win32 API 的"输出参数"：API 把分配好的指针写进来。
    T* Receive() { return &value_; }
    /// 取出指针使用，所有权仍归本对象。
    T Get() const { return value_; }

private:
    T value_ = nullptr;  ///< 被管理的指针
};

/// 两个 SID 是否指同一个账户。任何一边为空或无效都算"不同"——
/// 调用方据此决定要不要去设，判成"不同"的代价只是多试一次，安全。
bool SameSid(PSID a, PSID b) {
    if (a == nullptr || b == nullptr) return false;
    if (IsValidSid(a) == 0 || IsValidSid(b) == 0) return false;
    return EqualSid(a, b) != 0;
}

/// 我们关心的三段。SACL 不读——那要 SeSecurityPrivilege，且与评分项无关。
constexpr SECURITY_INFORMATION kSections =
    OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;

}  // namespace

/// 读安全描述符并转成 SDDL 文本。两步：
/// ① GetNamedSecurityInfoW 取二进制描述符（属主 + 属组 + DACL）；
/// ② ConvertSecurityDescriptorToStringSecurityDescriptorW 转成文本，再转 UTF-8 存起来。
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

    // 二进制 → SDDL 文本（形如 "O:S-1-5-...G:...D:P(A;;FA;;;SY)..."）。
    LocalPtr<LPWSTR> text;
    if (ConvertSecurityDescriptorToStringSecurityDescriptorW(
            descriptor.Get(), SDDL_REVISION_1, kSections, text.Receive(), nullptr) == 0) {
        Record(win_error);
        return false;
    }
    *sddl = ToUtf8(text.Get());
    return true;
}

/// 把 SDDL 文本还原到文件上。步骤：
/// ① SDDL 文本 → 二进制描述符；② 拆出属主、属组、DACL 三段；
/// ③ 跟文件现状比，已经一样的段不设；④ 决定继承开关；
/// ⑤ 调 SetNamedSecurityInfoW；设属主失败时退一步只设 DACL。
bool ApplySecurityDescriptor(const std::wstring& path, const std::string& sddl, bool* owner_skipped,
                             uint32_t* win_error) {
    if (owner_skipped != nullptr) *owner_skipped = false;
    if (sddl.empty()) return true;  // 备份时就没读到，没什么可还原的

    // ① SDDL 文本 → 二进制描述符。文本坏了就直接失败。
    LocalPtr<PSECURITY_DESCRIPTOR> descriptor;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
            FromUtf8(sddl).c_str(), SDDL_REVISION_1, descriptor.Receive(), nullptr) == 0) {
        Record(win_error);
        return false;
    }

    // ② SetNamedSecurityInfoW 要的是拆开的各段指针，不是整块描述符。
    PSID owner = nullptr;
    PSID group = nullptr;
    PACL dacl = nullptr;
    BOOL defaulted = FALSE;
    BOOL dacl_present = FALSE;
    GetSecurityDescriptorOwner(descriptor.Get(), &owner, &defaulted);
    GetSecurityDescriptorGroup(descriptor.Get(), &group, &defaulted);
    GetSecurityDescriptorDacl(descriptor.Get(), &dacl_present, &dacl, &defaulted);

    const std::wstring extended = platform::ToExtendedPath(path);

    // ③ 属主、属组已经是对的就别去设。
    //
    // 设属主要的是目标上的 WRITE_OWNER 访问权（或 SeRestorePrivilege），
    // 跟"要设成谁"无关。非提权用户在 D 盘这类位置通常只有"修改"权限，
    // 不含 WRITE_OWNER——哪怕要设的属主就是他自己、也正是文件现在的属主，
    // 调用照样被拒。结果在最常见的场景（自己还原自己的文件）里，每个条目
    // 都白白失败一次、退回只设 DACL、再报一条"属主未还原"，而属主其实
    // 一直是对的。
    //
    // 读不到现状就当作"不一样"，照常去设：宁可多试一次，不能因为读失败漏设。
    {
        LocalPtr<PSECURITY_DESCRIPTOR> current;
        PSID current_owner = nullptr;
        PSID current_group = nullptr;
        if (GetNamedSecurityInfoW(extended.c_str(), SE_FILE_OBJECT,
                                  OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION,
                                  &current_owner, &current_group, nullptr, nullptr,
                                  current.Receive()) == ERROR_SUCCESS) {
            if (SameSid(owner, current_owner)) owner = nullptr;
            if (SameSid(group, current_group)) group = nullptr;
        }
    }

    // 只设 SDDL 里真有的、而且确实需要改的段。传了 OWNER_SECURITY_INFORMATION 却给个空属主，
    // 整个调用会失败——连带 DACL 也设不上。
    SECURITY_INFORMATION sections = 0;
    if (owner != nullptr) sections |= OWNER_SECURITY_INFORMATION;
    if (group != nullptr) sections |= GROUP_SECURITY_INFORMATION;
    if (dacl_present) sections |= DACL_SECURITY_INFORMATION;
    if (sections == 0) return true;  // 三段都没有，没什么可设的

    // ④ 继承开关必须显式给：SetNamedSecurityInfoW 不看 SDDL 里的 `P`。
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

    // ⑤ 第一次尝试：所有需要的段一起设。
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
