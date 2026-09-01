// Copyright 2026 CBK Project. platform_win 的单元测试。
//
// 路径转换是整个项目最容易埋雷的地方：漏了 \\?\ 前缀，在浅目录上一切正常，
// 到了深目录才莫名其妙失败；加了前缀却忘了先规范化，路径里留个 ".." 就直接
// 报 ERROR_INVALID_NAME。所以这里既测纯字符串变换，也真的去文件系统上
// 建一棵超过 260 字符的深目录跑一遍。

#include <windows.h>

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "src/platform_win.h"

namespace {

namespace pf = cbk::platform;

// ============================================================ 编码转换

TEST(Utf8, RoundTripsAsciiChineseAndEmoji) {
    const std::wstring cases[] = {
        L"plain",
        L"带中文的文件名",
        L"emoji \U0001F600 混排",
        L"空格 和 符号 !@#$%^&()",
    };
    for (const std::wstring& original : cases) {
        EXPECT_EQ(original, pf::FromUtf8(pf::ToUtf8(original)));
    }
}

TEST(Utf8, EmptyStringStaysEmpty) {
    EXPECT_TRUE(pf::ToUtf8(L"").empty());
    EXPECT_TRUE(pf::FromUtf8("").empty());
}

TEST(Utf8, ChineseTakesThreeBytesEach) {
    // 顺带钉住一件事：转出来的是 UTF-8 而不是本地代码页（GBK 会是 2 字节）。
    EXPECT_EQ(6u, pf::ToUtf8(L"中文").size());
}

// ============================================================ IsExtendedPath

TEST(ExtendedPath, RecognizesBothNamespacePrefixes) {
    EXPECT_TRUE(pf::IsExtendedPath(L"\\\\?\\C:\\a"));
    EXPECT_TRUE(pf::IsExtendedPath(L"\\\\?\\UNC\\srv\\share"));
    EXPECT_TRUE(pf::IsExtendedPath(L"\\\\.\\PhysicalDrive0"));

    EXPECT_FALSE(pf::IsExtendedPath(L"C:\\a"));
    EXPECT_FALSE(pf::IsExtendedPath(L"\\\\srv\\share"));
    EXPECT_FALSE(pf::IsExtendedPath(L""));
    EXPECT_FALSE(pf::IsExtendedPath(L"\\\\?"));
}

// ============================================================ NormalizePath

TEST(NormalizePath, ConvertsForwardSlashes) {
    EXPECT_EQ(L"C:\\a\\b", pf::NormalizePath(L"C:/a/b"));
}

TEST(NormalizePath, ResolvesDotAndDotDot) {
    EXPECT_EQ(L"C:\\b", pf::NormalizePath(L"C:\\a\\..\\b"));
    EXPECT_EQ(L"C:\\a\\b", pf::NormalizePath(L"C:\\a\\.\\b"));
}

TEST(NormalizePath, StripsTrailingSeparatorButKeepsDriveRoot) {
    EXPECT_EQ(L"C:\\a\\b", pf::NormalizePath(L"C:\\a\\b\\"));
    EXPECT_EQ(L"C:\\", pf::NormalizePath(L"C:\\"));
}

TEST(NormalizePath, LeavesExtendedPathUntouched) {
    // \\?\ 路径的语义就是"原样送给文件系统"，规范化它反而会改变含义。
    const std::wstring weird = L"\\\\?\\C:\\a\\..\\b";
    EXPECT_EQ(weird, pf::NormalizePath(weird));
}

TEST(NormalizePath, MakesRelativePathAbsolute) {
    const std::wstring result = pf::NormalizePath(L"sub\\file.txt");
    EXPECT_NE(std::wstring::npos, result.find(L':'));
    EXPECT_NE(std::wstring::npos, result.find(L"sub\\file.txt"));
}

TEST(NormalizePath, EmptyStaysEmpty) {
    EXPECT_TRUE(pf::NormalizePath(L"").empty());
}

// ============================================================ ToExtendedPath

TEST(ToExtendedPath, PrefixesDrivePath) {
    EXPECT_EQ(L"\\\\?\\C:\\a\\b", pf::ToExtendedPath(L"C:\\a\\b"));
}

TEST(ToExtendedPath, NormalizesBeforePrefixing) {
    // 这条是重点：\\?\ 路径不会被内核规范化，所以 ".." 必须在加前缀之前消掉，
    // 否则拿去 CreateFileW 会直接失败。
    EXPECT_EQ(L"\\\\?\\C:\\b", pf::ToExtendedPath(L"C:\\a\\..\\b"));
    EXPECT_EQ(L"\\\\?\\C:\\a\\b", pf::ToExtendedPath(L"C:/a/b"));
}

TEST(ToExtendedPath, IsIdempotent) {
    const std::wstring once = pf::ToExtendedPath(L"C:\\a\\b");
    EXPECT_EQ(once, pf::ToExtendedPath(once));
}

TEST(ToExtendedPath, UsesUncFormForNetworkPaths) {
    EXPECT_EQ(L"\\\\?\\UNC\\srv\\share\\a", pf::ToExtendedPath(L"\\\\srv\\share\\a"));
}

TEST(ToExtendedPath, EmptyStaysEmpty) {
    EXPECT_TRUE(pf::ToExtendedPath(L"").empty());
}

// ============================================================ JoinPath

TEST(JoinPath, InsertsExactlyOneSeparator) {
    EXPECT_EQ(L"C:\\a\\b", pf::JoinPath(L"C:\\a", L"b"));
    EXPECT_EQ(L"C:\\a\\b", pf::JoinPath(L"C:\\a\\", L"b"));
    EXPECT_EQ(L"C:\\a\\b", pf::JoinPath(L"C:\\a", L"\\b"));
    EXPECT_EQ(L"C:\\a\\b", pf::JoinPath(L"C:\\a\\", L"\\b"));
}

TEST(JoinPath, EmptySideReturnsOther) {
    EXPECT_EQ(L"C:\\a", pf::JoinPath(L"C:\\a", L""));
    EXPECT_EQ(L"b", pf::JoinPath(L"", L"b"));
}

// ============================================================ 深目录实测

namespace {

/// 造一个唯一的测试根目录名，避免并行跑测试时互相踩。
std::wstring MakeTempRoot() {
    wchar_t temp[MAX_PATH] = {};
    const DWORD len = GetTempPathW(MAX_PATH, temp);
    EXPECT_GT(len, 0u);
    return pf::JoinPath(std::wstring(temp, len),
                        L"cbk_path_test_" + std::to_wstring(GetCurrentProcessId()));
}

/// 递归删掉一棵目录树。只在测试里用，实现怎么笨都行，但路径必须走扩展前缀，
/// 否则删不掉自己刚建出来的深目录。
void RemoveTreeDeep(const std::wstring& root, const std::vector<std::wstring>& segments) {
    std::wstring path = root;
    for (const std::wstring& segment : segments) {
        path = pf::JoinPath(path, segment);
    }
    // 从最深处往外删。
    for (size_t i = segments.size(); i > 0; --i) {
        RemoveDirectoryW(pf::ToExtendedPath(path).c_str());
        const size_t cut = path.find_last_of(L'\\');
        if (cut == std::wstring::npos) break;
        path.resize(cut);
    }
    RemoveDirectoryW(pf::ToExtendedPath(root).c_str());
}

}  // namespace

TEST(LongPath, CreatesReadsAndDeletesPathBeyondMaxPath) {
    const std::wstring root = MakeTempRoot();
    // 每层 40 个字符，12 层就稳超 260。
    const std::wstring segment(40, L'd');
    std::vector<std::wstring> segments;
    for (int i = 0; i < 12; ++i) segments.push_back(segment);

    ASSERT_NE(0, CreateDirectoryW(pf::ToExtendedPath(root).c_str(), nullptr))
        << "建测试根目录失败: " << pf::ToUtf8(pf::FormatWinError(GetLastError()));

    std::wstring deep = root;
    for (const std::wstring& s : segments) {
        deep = pf::JoinPath(deep, s);
        ASSERT_NE(0, CreateDirectoryW(pf::ToExtendedPath(deep).c_str(), nullptr))
            << "在第 " << deep.size()
            << " 字符处建目录失败: " << pf::ToUtf8(pf::FormatWinError(GetLastError()));
    }
    EXPECT_GT(deep.size(), 260u) << "测试没造出足够深的路径，这个用例就白测了";

    const std::wstring file = pf::JoinPath(deep, L"payload.bin");
    const char payload[] = "cbk-long-path";

    {
        pf::ScopedHandle writer = pf::CreateForWrite(file);
        ASSERT_TRUE(writer.IsValid())
            << "深路径下建文件失败: " << pf::ToUtf8(pf::FormatWinError(GetLastError()));
        DWORD written = 0;
        ASSERT_NE(0, WriteFile(writer.Get(), payload, sizeof(payload) - 1, &written, nullptr));
        EXPECT_EQ(sizeof(payload) - 1, written);
    }

    {
        pf::ScopedHandle reader = pf::OpenForRead(file, true);
        ASSERT_TRUE(reader.IsValid())
            << "深路径下读文件失败: " << pf::ToUtf8(pf::FormatWinError(GetLastError()));
        char buffer[64] = {};
        DWORD read = 0;
        ASSERT_NE(0, ReadFile(reader.Get(), buffer, sizeof(buffer), &read, nullptr));
        EXPECT_EQ(sizeof(payload) - 1, read);
        EXPECT_STREQ(payload, buffer);
    }

    DeleteFileW(pf::ToExtendedPath(file).c_str());
    RemoveTreeDeep(root, segments);
}

TEST(LongPath, HandlesChineseDirectoryNames) {
    const std::wstring root = MakeTempRoot() + L"_zh";
    ASSERT_NE(0, CreateDirectoryW(pf::ToExtendedPath(root).c_str(), nullptr));

    const std::wstring dir = pf::JoinPath(root, L"中文目录 with spaces");
    ASSERT_NE(0, CreateDirectoryW(pf::ToExtendedPath(dir).c_str(), nullptr));

    const std::wstring file = pf::JoinPath(dir, L"文件名.txt");
    {
        pf::ScopedHandle writer = pf::CreateForWrite(file);
        EXPECT_TRUE(writer.IsValid());
    }

    DeleteFileW(pf::ToExtendedPath(file).c_str());
    RemoveDirectoryW(pf::ToExtendedPath(dir).c_str());
    RemoveDirectoryW(pf::ToExtendedPath(root).c_str());
}

// ============================================================ ScopedHandle

TEST(ScopedHandle, DefaultIsInvalidAndSafeToDestroy) {
    pf::ScopedHandle handle;
    EXPECT_FALSE(handle.IsValid());
}

TEST(ScopedHandle, TreatsNullptrAsInvalid) {
    // Win32 有两种无效值：CreateFile 失败给 INVALID_HANDLE_VALUE，
    // 别的 API 失败给 nullptr。两种都不能拿去 CloseHandle。
    pf::ScopedHandle handle(nullptr);
    EXPECT_FALSE(handle.IsValid());
}

TEST(ScopedHandle, MoveTransfersOwnership) {
    HANDLE raw = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ASSERT_NE(nullptr, raw);

    pf::ScopedHandle source(raw);
    ASSERT_TRUE(source.IsValid());

    pf::ScopedHandle target = std::move(source);
    EXPECT_TRUE(target.IsValid());
    EXPECT_FALSE(source.IsValid());
    EXPECT_EQ(raw, target.Get());
}

TEST(ScopedHandle, ReleaseGivesUpOwnership) {
    HANDLE raw = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ASSERT_NE(nullptr, raw);

    pf::ScopedHandle handle(raw);
    EXPECT_EQ(raw, handle.Release());
    EXPECT_FALSE(handle.IsValid());
    CloseHandle(raw);  // 所有权已经交回来了，得自己关
}

// ============================================================ 特权与错误

TEST(Privilege, EnablingIsNeverFatal) {
    // 以普通用户跑时这三个都会失败，那是正常的——降级运行即可。
    // 这个用例只保证调用本身不崩、不抛。
    const pf::PrivilegeState state = pf::EnableBackupPrivileges();
    EXPECT_EQ(state.AllGranted(), state.backup && state.restore && state.security);
}

TEST(FormatWinError, ProducesReadableTextWithoutTrailingNewline) {
    const std::wstring text = pf::FormatWinError(ERROR_FILE_NOT_FOUND);
    EXPECT_FALSE(text.empty());
    EXPECT_NE(L'\n', text.back());
    EXPECT_NE(L'\r', text.back());
}

TEST(FormatWinError, FallsBackForUnknownCode) {
    const std::wstring text = pf::FormatWinError(0xDEADBEEF);
    EXPECT_FALSE(text.empty());
}

}  // namespace
