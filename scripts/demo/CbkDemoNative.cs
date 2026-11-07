// 演示脚本共用的 Win32 辅助类，由 make_demo_data.ps1 和 compare_trees.ps1 用 Add-Type 加载。
//
// 为什么不用 PowerShell 自带的命令：Windows PowerShell 5.1 跑在老的 .NET 路径处理上，
// 超过 260 字符的路径会直接报错；Remove-Item -Recurse 遇到 junction 还会钻进去删目标。
// 演示数据里两样都有，所以底层操作全部直接调 Win32，并一律加 \\?\ 前缀。
//
// 注意：Add-Type 用的是 .NET Framework 自带的 C# 5 编译器，不能用字符串插值、out var 等新语法。
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using Microsoft.Win32.SafeHandles;

/// 一个条目的全部可比较信息。
public class CbkDemoEntry {
    public string Rel;            // 相对根目录的路径
    public uint Attributes;       // 属性位（来自句柄，权威值）
    public uint ReparseTag;       // 重解析标签，0 表示不是重解析点
    public long CreationTime;     // FILETIME 原值，100 纳秒
    public long LastAccessTime;
    public long LastWriteTime;
    public long Size;
    public uint VolumeSerial;     // 卷序列号 + 文件索引号 = 文件身份，用来判断硬链接
    public ulong FileIndex;
    public uint LinkCount;        // 硬链接数
    public string LinkTarget;     // 符号链接 / junction 的目标（PrintName）
    public bool LinkRelative;     // 符号链接是否是相对路径
    public string Sddl;           // 属主 + 属组 + DACL，读不到为 null
    public string Hash;           // 普通文件内容的 SHA-256
    public string Error;          // 读信息时出的错，正常为 null

    public bool IsDirectory { get { return (Attributes & 0x10) != 0; } }
    public bool IsReparse { get { return ReparseTag != 0; } }
}

public static class CbkDemo {
    // ------------------------------------------------------------ Win32 声明

    const uint FILE_READ_ATTRIBUTES = 0x80;
    const uint FILE_WRITE_ATTRIBUTES = 0x100;
    const uint GENERIC_READ = 0x80000000;
    const uint GENERIC_WRITE = 0x40000000;
    const uint SHARE_ALL = 7;
    const uint OPEN_EXISTING = 3;
    const uint CREATE_ALWAYS = 2;
    const uint FLAG_BACKUP_SEMANTICS = 0x02000000;
    const uint FLAG_OPEN_REPARSE_POINT = 0x00200000;
    const uint FLAG_SEQUENTIAL_SCAN = 0x08000000;
    const uint FSCTL_GET_REPARSE_POINT = 0x000900A8;
    const uint TAG_SYMLINK = 0xA000000C;
    const uint TAG_MOUNT_POINT = 0xA0000003;

    // Pack = 4：FILETIME 是两个 DWORD，只按 4 字节对齐；不写的话 long 会被对齐到 8，字段全部错位。
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode, Pack = 4)]
    struct WIN32_FIND_DATAW {
        public uint dwFileAttributes;
        public long ftCreationTime_unused1;   // 只用来占位，真正的值从句柄上读
        public long ftLastAccessTime_unused;
        public long ftLastWriteTime_unused;
        public uint nFileSizeHigh;
        public uint nFileSizeLow;
        public uint dwReserved0;              // 重解析点标签
        public uint dwReserved1;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)] public string cFileName;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 14)] public string cAlternateFileName;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 4)]
    struct BY_HANDLE_FILE_INFORMATION {
        public uint Attributes;
        public long CreationTime;
        public long LastAccessTime;
        public long LastWriteTime;
        public uint VolumeSerial;
        public uint SizeHigh;
        public uint SizeLow;
        public uint LinkCount;
        public uint IndexHigh;
        public uint IndexLow;
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr FindFirstFileW(string path, out WIN32_FIND_DATAW data);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool FindNextFileW(IntPtr handle, out WIN32_FIND_DATAW data);
    [DllImport("kernel32.dll")]
    static extern bool FindClose(IntPtr handle);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern SafeFileHandle CreateFileW(string path, uint access, uint share, IntPtr sa,
                                             uint disposition, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool GetFileInformationByHandle(SafeFileHandle h, out BY_HANDLE_FILE_INFORMATION info);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool SetFileTime(SafeFileHandle h, ref long c, ref long a, ref long w);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool DeviceIoControl(SafeFileHandle h, uint code, IntPtr inBuf, uint inSize,
                                       byte[] outBuf, uint outSize, out uint returned, IntPtr ov);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool CreateDirectoryW(string path, IntPtr sa);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool RemoveDirectoryW(string path);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool DeleteFileW(string path);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool SetFileAttributesW(string path, uint attributes);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern uint GetFileAttributesW(string path);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool CreateHardLinkW(string link, string existing, IntPtr sa);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern byte CreateSymbolicLinkW(string link, string target, uint flags);
    [DllImport("advapi32.dll", CharSet = CharSet.Unicode)]
    static extern uint GetNamedSecurityInfoW(string name, int type, int info, IntPtr owner,
                                             IntPtr group, IntPtr dacl, IntPtr sacl, out IntPtr sd);
    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool ConvertSecurityDescriptorToStringSecurityDescriptorW(
        IntPtr sd, uint revision, int info, out IntPtr text, IntPtr length);
    [DllImport("kernel32.dll")]
    static extern IntPtr LocalFree(IntPtr p);

    static Exception LastError(string what, string path) {
        return new Win32Exception(Marshal.GetLastWin32Error(), what + ": " + path);
    }

    // ------------------------------------------------------------ 路径

    /// 绝对路径加 \\?\ 前缀（UNC 路径加 \\?\UNC\），绕过 260 字符限制。
    public static string Ext(string path) {
        if (path.StartsWith(@"\\?\")) return path;
        string full = Path.GetFullPath(path);
        if (full.StartsWith(@"\\")) return @"\\?\UNC\" + full.Substring(2);
        return @"\\?\" + full;
    }

    // ------------------------------------------------------------ 生成数据

    /// 逐级建目录，已存在不算错。
    public static void EnsureDir(string path) {
        string ext = Ext(path);
        int start = ext.StartsWith(@"\\?\UNC\") ? 8 : 7;   // 跳过前缀和盘符
        for (int i = start; i <= ext.Length; i++) {
            if (i == ext.Length || ext[i] == '\\') {
                string part = ext.Substring(0, i);
                if (!CreateDirectoryW(part, IntPtr.Zero) && Marshal.GetLastWin32Error() != 183) {
                    throw LastError("建目录失败", part);
                }
            }
        }
    }

    /// 写一段字节到文件（覆盖）。
    public static void WriteBytes(string path, byte[] data) {
        using (SafeFileHandle h = CreateFileW(Ext(path), GENERIC_WRITE, SHARE_ALL, IntPtr.Zero,
                                              CREATE_ALWAYS, 0, IntPtr.Zero)) {
            if (h.IsInvalid) throw LastError("建文件失败", path);
            using (FileStream fs = new FileStream(h, FileAccess.Write)) {
                fs.Write(data, 0, data.Length);
            }
        }
    }

    /// 写 UTF-8 文本（不带 BOM）。
    public static void WriteText(string path, string text) {
        WriteBytes(path, new UTF8Encoding(false).GetBytes(text));
    }

    /// 写一个指定大小的随机内容文件，16 MB 一块地写，内存占用固定。
    public static void WriteRandomFile(string path, long size, int seed) {
        Random rng = new Random(seed);
        byte[] buffer = new byte[16 * 1024 * 1024];
        using (SafeFileHandle h = CreateFileW(Ext(path), GENERIC_WRITE, SHARE_ALL, IntPtr.Zero,
                                              CREATE_ALWAYS, FLAG_SEQUENTIAL_SCAN, IntPtr.Zero)) {
            if (h.IsInvalid) throw LastError("建文件失败", path);
            using (FileStream fs = new FileStream(h, FileAccess.Write, 1 << 20)) {
                long left = size;
                while (left > 0) {
                    rng.NextBytes(buffer);
                    int n = (int)Math.Min(left, buffer.Length);
                    fs.Write(buffer, 0, n);
                    left -= n;
                }
            }
        }
    }

    /// 设三个时间戳（FILETIME 原值）。对链接设的是链接本身。
    public static void SetTimes(string path, long creation, long access, long write) {
        using (SafeFileHandle h = CreateFileW(Ext(path), FILE_WRITE_ATTRIBUTES, SHARE_ALL, IntPtr.Zero,
                                              OPEN_EXISTING,
                                              FLAG_BACKUP_SEMANTICS | FLAG_OPEN_REPARSE_POINT,
                                              IntPtr.Zero)) {
            if (h.IsInvalid) throw LastError("打开失败", path);
            if (!SetFileTime(h, ref creation, ref access, ref write)) throw LastError("设时间失败", path);
        }
    }

    /// 设属性位。
    public static void SetAttributes(string path, uint attributes) {
        if (!SetFileAttributesW(Ext(path), attributes)) throw LastError("设属性失败", path);
    }

    /// 建硬链接。
    public static void HardLink(string link, string existing) {
        if (!CreateHardLinkW(Ext(link), Ext(existing), IntPtr.Zero)) throw LastError("建硬链接失败", link);
    }

    /// 建符号链接。target 原样保存（相对就是相对）。先带"免管理员"标志，失败再不带。
    /// 返回 0 表示成功，否则是 Win32 错误码。
    public static int SymLink(string link, string target, bool directory) {
        uint flags = directory ? 1u : 0u;
        if (CreateSymbolicLinkW(Ext(link), target, flags | 2u) != 0) return 0;
        if (CreateSymbolicLinkW(Ext(link), target, flags) != 0) return 0;
        return Marshal.GetLastWin32Error();
    }

    // ------------------------------------------------------------ 删除

    /// 安全地删整棵树：链接只删链接本身，绝不进入它指向的地方；只读位先摘掉。
    public static void DeleteTree(string path) {
        string root = Ext(path);
        uint attr = GetFileAttributesW(root);
        if (attr == 0xFFFFFFFF) return;   // 不存在
        DeleteEntry(root, attr);
    }

    static void DeleteEntry(string ext, uint attr) {
        SetFileAttributesW(ext, (attr & 0x10) != 0 ? 0x10u : 0x80u);
        bool isDir = (attr & 0x10) != 0;
        bool isReparse = (attr & 0x400) != 0;
        if (isDir && !isReparse) {
            WIN32_FIND_DATAW data;
            IntPtr h = FindFirstFileW(ext + @"\*", out data);
            if (h != new IntPtr(-1)) {
                try {
                    do {
                        if (data.cFileName == "." || data.cFileName == "..") continue;
                        DeleteEntry(ext + @"\" + data.cFileName, data.dwFileAttributes);
                    } while (FindNextFileW(h, out data));
                } finally {
                    FindClose(h);
                }
            }
        }
        if (isDir) {
            if (!RemoveDirectoryW(ext)) throw LastError("删目录失败", ext);
        } else {
            if (!DeleteFileW(ext)) throw LastError("删文件失败", ext);
        }
    }

    // ------------------------------------------------------------ 读取与比较

    /// 遍历整棵树（不含根本身），不跟随任何链接，返回每个条目的完整信息。
    /// hash 为 true 时计算普通文件内容的 SHA-256。
    public static List<CbkDemoEntry> Scan(string rootPath, bool hash) {
        List<CbkDemoEntry> result = new List<CbkDemoEntry>();
        string root = Ext(rootPath).TrimEnd('\\');
        Stack<string> pending = new Stack<string>();
        pending.Push("");
        while (pending.Count > 0) {
            string rel = pending.Pop();
            string dir = rel.Length == 0 ? root : root + @"\" + rel;
            WIN32_FIND_DATAW data;
            IntPtr h = FindFirstFileW(dir + @"\*", out data);
            if (h == new IntPtr(-1)) continue;
            List<string> children = new List<string>();
            try {
                do {
                    if (data.cFileName == "." || data.cFileName == "..") continue;
                    string childRel = rel.Length == 0 ? data.cFileName : rel + @"\" + data.cFileName;
                    CbkDemoEntry e = Inspect(root + @"\" + childRel, childRel, data.dwFileAttributes,
                                             data.dwReserved0, hash);
                    result.Add(e);
                    if (e.IsDirectory && !e.IsReparse) children.Add(childRel);
                } while (FindNextFileW(h, out data));
            } finally {
                FindClose(h);
            }
            for (int i = children.Count - 1; i >= 0; i--) pending.Push(children[i]);
        }
        return result;
    }

    static CbkDemoEntry Inspect(string ext, string rel, uint findAttr, uint findTag, bool hash) {
        CbkDemoEntry e = new CbkDemoEntry();
        e.Rel = rel;
        e.Attributes = findAttr;
        e.ReparseTag = (findAttr & 0x400) != 0 ? findTag : 0;

        // 时间戳、属性、文件身份：从链接本身的句柄上读。
        using (SafeFileHandle h = CreateFileW(ext, FILE_READ_ATTRIBUTES, SHARE_ALL, IntPtr.Zero, OPEN_EXISTING,
                                              FLAG_BACKUP_SEMANTICS | FLAG_OPEN_REPARSE_POINT,
                                              IntPtr.Zero)) {
            if (h.IsInvalid) {
                e.Error = new Win32Exception(Marshal.GetLastWin32Error()).Message;
                return e;
            }
            BY_HANDLE_FILE_INFORMATION info;
            if (GetFileInformationByHandle(h, out info)) {
                e.Attributes = info.Attributes;
                e.CreationTime = info.CreationTime;
                e.LastAccessTime = info.LastAccessTime;
                e.LastWriteTime = info.LastWriteTime;
                e.Size = ((long)info.SizeHigh << 32) | info.SizeLow;
                e.VolumeSerial = info.VolumeSerial;
                e.FileIndex = ((ulong)info.IndexHigh << 32) | info.IndexLow;
                e.LinkCount = info.LinkCount;
            }
            if (e.IsReparse) ReadReparse(h, e);
        }

        e.Sddl = ReadSddl(ext);

        if (hash && !e.IsDirectory && !e.IsReparse) {
            using (SafeFileHandle h = CreateFileW(ext, GENERIC_READ, SHARE_ALL, IntPtr.Zero, OPEN_EXISTING,
                                                  FLAG_BACKUP_SEMANTICS | FLAG_SEQUENTIAL_SCAN,
                                                  IntPtr.Zero)) {
                if (h.IsInvalid) {
                    e.Error = "读内容失败：" + new Win32Exception(Marshal.GetLastWin32Error()).Message;
                } else {
                    using (FileStream fs = new FileStream(h, FileAccess.Read, 1 << 20))
                    using (SHA256 sha = SHA256.Create()) {
                        e.Hash = BitConverter.ToString(sha.ComputeHash(fs)).Replace("-", "");
                    }
                }
            }
        }
        return e;
    }

    /// 解析 REPARSE_DATA_BUFFER，取 PrintName（为空时退回 SubstituteName 并去掉 \??\）。
    static void ReadReparse(SafeFileHandle h, CbkDemoEntry e) {
        byte[] buf = new byte[16 * 1024];
        uint got;
        if (!DeviceIoControl(h, FSCTL_GET_REPARSE_POINT, IntPtr.Zero, 0, buf, (uint)buf.Length, out got,
                             IntPtr.Zero)) {
            return;
        }
        uint tag = BitConverter.ToUInt32(buf, 0);
        int pathStart;
        if (tag == TAG_SYMLINK) {
            pathStart = 20;
            e.LinkRelative = (BitConverter.ToUInt32(buf, 16) & 1) != 0;
        } else if (tag == TAG_MOUNT_POINT) {
            pathStart = 16;
        } else {
            return;
        }
        int subOff = BitConverter.ToUInt16(buf, 8), subLen = BitConverter.ToUInt16(buf, 10);
        int prnOff = BitConverter.ToUInt16(buf, 12), prnLen = BitConverter.ToUInt16(buf, 14);
        string print = Encoding.Unicode.GetString(buf, pathStart + prnOff, prnLen);
        if (print.Length == 0) {
            print = Encoding.Unicode.GetString(buf, pathStart + subOff, subLen);
            if (print.StartsWith(@"\??\")) print = print.Substring(4);
        }
        e.LinkTarget = print;
    }

    /// 读属主 + 属组 + DACL，转成 SDDL 文本。读不到返回 null。
    public static string ReadSddl(string path) {
        IntPtr sd;
        uint status = GetNamedSecurityInfoW(Ext(path), 1, 7, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero,
                                            IntPtr.Zero, out sd);
        if (status != 0) return null;
        try {
            IntPtr text;
            if (!ConvertSecurityDescriptorToStringSecurityDescriptorW(sd, 1, 7, out text, IntPtr.Zero)) {
                return null;
            }
            string sddl = Marshal.PtrToStringUni(text);
            LocalFree(text);
            return sddl;
        } finally {
            LocalFree(sd);
        }
    }
}
