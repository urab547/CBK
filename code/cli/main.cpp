// Copyright 2026 CBK Project. cbk.exe 命令行入口。
//
// 这一层只做三件事：解析参数、调用 core、把事件按协议打到 stdout。
// 任何业务逻辑都不属于这里。
//
// 与 GUI 的契约见 docs/CBK组内开工说明（第 04 节）。
// 要点：每行一个完整 JSON，写完立刻 flush；密码走 stdin 不走参数；
// 退出码 0 成功 / 1 部分成功 / 2 失败 / 3 参数错误。

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include "cbk/engine.h"
#include "cbk/event.h"
#include "cbk/packer.h"
#include "cbk/stage.h"
#include "cbk/text.h"
#include "cbk/types.h"

namespace {

// ============================================================ JSON 输出

/// 把字符串转义成合法的 JSON 字符串字面量（含首尾双引号）。
std::string JsonQuote(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 2);
    out.push_back('"');
    for (char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                // 控制字符必须转成 \u00XX，直接塞进去的 JSON 是非法的。
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buffer[8] = {};
                    std::snprintf(buffer, sizeof(buffer), "\\u%04X", static_cast<unsigned char>(c));
                    out += buffer;
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
    return out;
}

/// 宽字符串先转 UTF-8 再转义。内存里一律 UTF-16，只有落到 stdout 才转。
std::string JsonQuoteW(const std::wstring& text) {
    return JsonQuote(cbk::ToUtf8(text));
}

std::string JsonArray(const std::vector<std::string>& items) {
    std::string out = "[";
    for (size_t i = 0; i < items.size(); ++i) {
        if (i > 0) out += ",";
        out += JsonQuote(items[i]);
    }
    out += "]";
    return out;
}

/// 打一行 JSON 并立刻 flush。
///
/// flush 不能省：不刷的话输出会攒在管道缓冲里，界面的进度条会一直不动，
/// 然后在进程结束时一次跳到底。
void EmitLine(const std::string& json) {
    std::fwrite(json.data(), 1, json.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

// ============================================================ 取消

/// Ctrl+C 把它置起来，引擎在下一次查取消时就会停下并清理半成品。
std::atomic<bool> g_cancel_requested{false};

BOOL WINAPI ConsoleHandler(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT || signal == CTRL_CLOSE_EVENT) {
        g_cancel_requested.store(true);
        return TRUE;  // 自己处理，别让默认行为直接杀进程——那样半成品就留下了
    }
    return FALSE;
}

// ============================================================ 事件观察者

/// 把 core 的事件序列化成逐行 JSON。
class JsonObserver : public cbk::IProgressObserver {
public:
    explicit JsonObserver(bool enabled) : enabled_(enabled) {}

    void OnStart(const cbk::StartInfo& info) override {
        if (!enabled_) return;
        EmitLine("{\"event\":\"start\",\"totalEntries\":" + std::to_string(info.total_entries) +
                 ",\"totalBytes\":" + std::to_string(info.total_bytes) + "}");
    }

    void OnProgress(const cbk::ProgressInfo& info) override {
        if (!enabled_) return;
        // 节流在这一层做，不在 core 里。core 忠实上报每一块，
        // 但每行 JSON 都要 flush，一万个小文件全打出去会把 stdout 淹掉，
        // 界面忙着解析反而更卡。
        const ULONGLONG now = GetTickCount64();
        const bool finished = info.total_entries > 0 && info.done_entries >= info.total_entries;
        if (!finished && now - last_emit_ms_ < kThrottleMs) return;
        last_emit_ms_ = now;

        std::string line =
            "{\"event\":\"progress\",\"doneEntries\":" + std::to_string(info.done_entries) +
            ",\"totalEntries\":" + std::to_string(info.total_entries) +
            ",\"doneBytes\":" + std::to_string(info.done_bytes) +
            ",\"totalBytes\":" + std::to_string(info.total_bytes);
        if (info.current != nullptr) {
            line += ",\"path\":" + JsonQuoteW(info.current->relative_path);
        }
        line += "}";
        EmitLine(line);
    }

    void OnWarn(const cbk::WarnInfo& info) override {
        if (!enabled_) return;
        EmitLine("{\"event\":\"warn\",\"path\":" + JsonQuoteW(info.path) +
                 ",\"message\":" + JsonQuoteW(info.message) +
                 ",\"winError\":" + std::to_string(info.win_error) + "}");
    }

    void OnResult(const cbk::ResultInfo& info) override {
        if (!enabled_) return;
        EmitLine(
            "{\"event\":\"result\",\"status\":" + std::to_string(static_cast<int>(info.status)) +
            ",\"entriesDone\":" + std::to_string(info.entries_done) +
            ",\"entriesSkipped\":" + std::to_string(info.entries_skipped) +
            ",\"bytesRead\":" + std::to_string(info.bytes_read) +
            ",\"bytesWritten\":" + std::to_string(info.bytes_written) + "}");
    }

    bool IsCancelled() override { return g_cancel_requested.load(); }

private:
    static constexpr ULONGLONG kThrottleMs = 100;

    bool enabled_;
    ULONGLONG last_emit_ms_ = 0;
};

// ============================================================ 参数解析

/// 解析好的命令行。
struct Args {
    std::wstring source;
    std::wstring dest;
    std::wstring archive;
    std::string packer = "cbk-native";
    std::string compress;
    std::string encrypt;
    std::wstring overwrite = L"skip";
    bool password_stdin = false;
    bool follow_symlinks = false;
    bool no_metadata = false;
    bool json = false;
    bool progress_json = false;

    std::wstring error;  ///< 非空表示解析失败
};

/// 取下一个参数值，缺了就记错误。
//
// 注意它会直接推进调用方的循环下标——把值消费掉，免得下一轮又把这个值
// 当成选项名去匹配。这是手写解析器最容易漏的一步：漏了的话，选项后面
// 跟的那个路径会被当成"不认识的选项"报错。
bool TakeValue(const std::vector<std::wstring>& argv, size_t* index, const std::wstring& option,
               std::wstring* out, Args* args) {
    if (*index + 1 >= argv.size()) {
        args->error = option + L" 后面缺一个值";
        return false;
    }
    ++(*index);
    *out = argv[*index];
    return true;
}

// 手写参数解析，不引第三方库。
//
// 理论上可以用 CLI11 之类，但为了一个五命令的程序引一个几千行的头文件
// 不划算，而且 core 本来就是零第三方依赖，这边跟着保持一致——交付时
// "整个项目除了 GoogleTest 和 PySide6 没有别的依赖"这句话更好说。
//
// 遇到第一个错误就 break，不继续往下解析。参数写错时用户要的是一条
// 清楚的提示，不是一串连锁报错。
Args ParseArgs(const std::vector<std::wstring>& argv, size_t start) {
    Args args;
    for (size_t i = start; i < argv.size(); ++i) {
        const std::wstring& option = argv[i];
        std::wstring value;

        if (option == L"--source") {
            if (!TakeValue(argv, &i, option, &args.source, &args)) break;
        } else if (option == L"--dest") {
            if (!TakeValue(argv, &i, option, &args.dest, &args)) break;
        } else if (option == L"--archive") {
            if (!TakeValue(argv, &i, option, &args.archive, &args)) break;
        } else if (option == L"--packer") {
            if (!TakeValue(argv, &i, option, &value, &args)) break;
            args.packer = cbk::ToUtf8(value);
        } else if (option == L"--compress") {
            if (!TakeValue(argv, &i, option, &value, &args)) break;
            args.compress = cbk::ToUtf8(value);
        } else if (option == L"--encrypt") {
            if (!TakeValue(argv, &i, option, &value, &args)) break;
            args.encrypt = cbk::ToUtf8(value);
        } else if (option == L"--overwrite") {
            if (!TakeValue(argv, &i, option, &args.overwrite, &args)) break;
        } else if (option == L"--progress") {
            if (!TakeValue(argv, &i, option, &value, &args)) break;
            if (value != L"json") {
                args.error = L"--progress 目前只支持 json";
                break;
            }
            args.progress_json = true;
        } else if (option == L"--password-stdin") {
            args.password_stdin = true;
        } else if (option == L"--follow-symlinks") {
            args.follow_symlinks = true;
        } else if (option == L"--no-metadata") {
            args.no_metadata = true;
        } else if (option == L"--json") {
            args.json = true;
        } else {
            args.error = L"不认识的选项 " + option;
            break;
        }
    }
    return args;
}

/// 从 stdin 读一行当密码。
///
/// 密码绝不走命令行参数：Windows 上别的进程能通过 NtQueryInformationProcess
/// 读到任意进程的命令行，密码写在参数里等于明文广播。
std::string ReadPasswordFromStdin() {
    std::string password;
    std::getline(std::cin, password);
    while (!password.empty() && (password.back() == '\r' || password.back() == '\n')) {
        password.pop_back();
    }
    return password;
}

int ToExitCode(cbk::Status status) {
    return static_cast<int>(status);
}

/// 参数错误统一从这里出去，保证退出码是 3 而不是 2。
int BadArgs(const std::wstring& message) {
    std::fwrite(cbk::ToUtf8(L"cbk: " + message + L"\n").c_str(), 1,
                cbk::ToUtf8(L"cbk: " + message + L"\n").size(), stderr);
    return ToExitCode(cbk::Status::kBadArgs);
}

// ============================================================ 各命令

int CommandInfo() {
    const cbk::PackerRegistry& packers = cbk::PackerRegistry::Instance();
    const cbk::StageRegistry& stages = cbk::StageRegistry::Instance();

    std::string out = "{";
    out += "\"formatVersion\":" + std::to_string(cbk::kFormatVersion);
    out += ",\"packers\":" + JsonArray(packers.Names());
    out += ",\"compressors\":" + JsonArray(stages.Names(cbk::StageKind::kCompress));
    out += ",\"ciphers\":" + JsonArray(stages.Names(cbk::StageKind::kEncrypt));
    out += "}";

    EmitLine(out);
    return ToExitCode(cbk::Status::kOk);
}

int CommandBackup(const Args& args) {
    if (args.source.empty()) return BadArgs(L"backup 需要 --source");
    if (args.dest.empty()) return BadArgs(L"backup 需要 --dest");

    cbk::BackupOptions options;
    options.source_root = args.source;
    options.dest_archive = args.dest;
    options.packer = args.packer;
    options.follow_symlinks = args.follow_symlinks;
    // 顺序写死为「压缩 -> 加密」。加密输出是高熵伪随机字节，
    // 压缩它压不动还会变大，所以压缩必须在前。
    if (!args.compress.empty()) options.stages.push_back(args.compress);
    if (!args.encrypt.empty()) options.stages.push_back(args.encrypt);
    if (args.password_stdin) options.password = ReadPasswordFromStdin();

    JsonObserver observer(args.progress_json);
    const cbk::EngineResult result = cbk::RunBackup(options, &observer);

    if (!result.error.empty()) {
        const std::string message = cbk::ToUtf8(L"cbk: " + result.error + L"\n");
        std::fwrite(message.data(), 1, message.size(), stderr);
    }
    return ToExitCode(result.status);
}

int CommandRestore(const Args& args) {
    if (args.archive.empty()) return BadArgs(L"restore 需要 --archive");
    if (args.dest.empty()) return BadArgs(L"restore 需要 --dest");

    cbk::RestoreOptions options;
    options.archive = args.archive;
    options.dest_root = args.dest;
    options.restore_metadata = !args.no_metadata;
    if (args.overwrite == L"skip") {
        options.overwrite = cbk::OverwritePolicy::kSkip;
    } else if (args.overwrite == L"force") {
        options.overwrite = cbk::OverwritePolicy::kForce;
    } else if (args.overwrite == L"rename") {
        options.overwrite = cbk::OverwritePolicy::kRename;
    } else {
        return BadArgs(L"--overwrite 只能是 skip / force / rename");
    }
    if (args.password_stdin) options.password = ReadPasswordFromStdin();

    JsonObserver observer(args.progress_json);
    const cbk::EngineResult result = cbk::RunRestore(options, &observer);

    if (!result.error.empty()) {
        const std::string message = cbk::ToUtf8(L"cbk: " + result.error + L"\n");
        std::fwrite(message.data(), 1, message.size(), stderr);
    }
    return ToExitCode(result.status);
}

int CommandList(const Args& args) {
    if (args.archive.empty()) return BadArgs(L"list 需要 --archive");

    std::string password;
    if (args.password_stdin) password = ReadPasswordFromStdin();

    cbk::ArchiveInfo info;
    std::vector<cbk::EntryMeta> entries;
    std::wstring error;
    const cbk::Status status =
        cbk::ReadArchiveListing(args.archive, password, &info, &entries, &error);
    if (status != cbk::Status::kOk) {
        const std::string message = cbk::ToUtf8(L"cbk: " + error + L"\n");
        std::fwrite(message.data(), 1, message.size(), stderr);
        return ToExitCode(status);
    }

    if (args.json) {
        std::string out = "{\"formatVersion\":" + std::to_string(info.format_version);
        out += ",\"sourceRoot\":" + JsonQuoteW(info.source_root);
        out += ",\"packer\":" + JsonQuote(info.packer);
        out += ",\"stages\":" + JsonArray(info.stages);
        out += ",\"entryCount\":" + std::to_string(info.entry_count);
        out += ",\"totalOriginalBytes\":" + std::to_string(info.total_original_bytes);
        out += ",\"entries\":[";
        for (size_t i = 0; i < entries.size(); ++i) {
            const cbk::EntryMeta& entry = entries[i];
            if (i > 0) out += ",";
            out += "{\"id\":" + std::to_string(entry.id);
            out += ",\"path\":" + JsonQuoteW(entry.relative_path);
            out += ",\"type\":" + JsonQuote(cbk::ToString(entry.type));
            out += ",\"size\":" + std::to_string(entry.original_size);
            out += ",\"attributes\":" + std::to_string(entry.attributes);
            out += ",\"lastWriteTime\":" + std::to_string(entry.last_write_time);
            out += "}";
        }
        out += "]}";
        EmitLine(out);
        return ToExitCode(cbk::Status::kOk);
    }

    std::string text = "源根: " + cbk::ToUtf8(info.source_root) + "\n";
    text += "打包: " + info.packer + "\n";
    text += "条目: " + std::to_string(info.entry_count) + "\n\n";
    for (const cbk::EntryMeta& entry : entries) {
        text += std::string(cbk::ToString(entry.type)) + "\t" +
                std::to_string(entry.original_size) + "\t" + cbk::ToUtf8(entry.relative_path) +
                "\n";
    }
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fflush(stdout);
    return ToExitCode(cbk::Status::kOk);
}

int CommandVerify(const Args& args) {
    if (args.archive.empty()) return BadArgs(L"verify 需要 --archive");

    std::wstring error;
    const cbk::Status status = cbk::VerifyArchive(args.archive, &error);
    if (status != cbk::Status::kOk) {
        const std::string message = cbk::ToUtf8(L"cbk: " + error + L"\n");
        std::fwrite(message.data(), 1, message.size(), stderr);
        return ToExitCode(status);
    }
    EmitLine("{\"event\":\"result\",\"status\":0,\"message\":\"校验通过\"}");
    return ToExitCode(cbk::Status::kOk);
}

// 用法输出到 stderr 而不是 stdout。
//
// stdout 留给 JSON 事件流，GUI 在那头逐行解析。把用法文本混进去会让它
// 撞上一行解析不了的东西。虽然只在打印帮助时才发生，但契约就是契约：
// stdout 上只能有 JSON。
void PrintUsage() {
    static const char kUsage[] =
        "用法: cbk <命令> [选项]\n"
        "\n"
        "命令:\n"
        "  backup   --source <目录> --dest <文件.cbk>\n"
        "           [--packer <名字>] [--compress <名字>] [--encrypt <名字>]\n"
        "           [--password-stdin] [--follow-symlinks] [--progress json]\n"
        "  restore  --archive <文件.cbk> --dest <目录>\n"
        "           [--password-stdin] [--overwrite skip|force|rename]\n"
        "           [--no-metadata] [--progress json]\n"
        "  list     --archive <文件.cbk> [--json] [--password-stdin]\n"
        "  verify   --archive <文件.cbk>\n"
        "  info     输出本程序支持的算法列表（JSON）\n"
        "\n"
        "退出码: 0 成功 / 1 部分成功 / 2 失败 / 3 参数错误\n";
    std::fwrite(kUsage, 1, sizeof(kUsage) - 1, stderr);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    // 控制台按 UTF-8 显示。管道给 GUI 时字节本来就是 UTF-8，
    // 这一行只影响人直接在终端里看的时候不至于是乱码。
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);

    // 内置算法在这里注册一次，之后 core 的任何地方都能查到。
    cbk::RegisterBuiltinPackers();
    cbk::RegisterBuiltinStages();

    if (argc < 2) {
        PrintUsage();
        return ToExitCode(cbk::Status::kBadArgs);
    }

    std::vector<std::wstring> args;
    args.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i) args.push_back(argv[i]);

    const std::wstring command = args[1];
    if (command == L"--help" || command == L"-h" || command == L"help") {
        PrintUsage();
        return ToExitCode(cbk::Status::kOk);
    }
    if (command == L"info") return CommandInfo();

    const Args parsed = ParseArgs(args, 2);
    if (!parsed.error.empty()) return BadArgs(parsed.error);

    if (command == L"backup") return CommandBackup(parsed);
    if (command == L"restore") return CommandRestore(parsed);
    if (command == L"list") return CommandList(parsed);
    if (command == L"verify") return CommandVerify(parsed);

    const std::string message = cbk::ToUtf8(L"cbk: 未知命令 " + command + L"\n\n");
    std::fwrite(message.data(), 1, message.size(), stderr);
    PrintUsage();
    return ToExitCode(cbk::Status::kBadArgs);
}
