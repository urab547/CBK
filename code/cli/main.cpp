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
#include <exception>
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

/// 把一组字符串拼成 JSON 数组，如 ["a","b"]。cbk info 输出算法列表时用。
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

/// 控制台事件回调：Ctrl+C、Ctrl+Break、关窗口时由系统调用。
/// 只做一件事——置取消标志，让引擎自己有序收尾。
BOOL WINAPI ConsoleHandler(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT || signal == CTRL_CLOSE_EVENT) {
        g_cancel_requested.store(true);
        return TRUE;  // 自己处理，别让默认行为直接杀进程——那样半成品就留下了
    }
    return FALSE;
}

// ============================================================ 事件观察者

/// 把 core 的事件序列化成逐行 JSON。
///
/// core 不知道 JSON 的存在，只调用 IProgressObserver 的几个回调；
/// 这个类把每个回调翻译成一行 JSON 打到 stdout，GUI 在管道另一头逐行读。
class JsonObserver : public cbk::IProgressObserver {
public:
    /// @param enabled 只有带了 --progress json 才输出，否则所有回调都静默。
    explicit JsonObserver(bool enabled) : enabled_(enabled) {}

    /// 开始事件：告诉界面总共多少条目、多少字节，用来算进度条分母。
    void OnStart(const cbk::StartInfo& info) override {
        if (!enabled_) return;
        EmitLine("{\"event\":\"start\",\"totalEntries\":" + std::to_string(info.total_entries) +
                 ",\"totalBytes\":" + std::to_string(info.total_bytes) + "}");
    }

    /// 普通进度：阶段名记为 "transfer"（正在搬运数据）。
    void OnProgress(const cbk::ProgressInfo& info) override { EmitProgress(info, "transfer"); }

    /// 阶段切换（如扫描、写索引、设置目录元数据），带上阶段名。
    void OnPhase(const std::string& phase, const cbk::ProgressInfo& info) override {
        EmitProgress(info, phase);
    }

    /// 输出一行 progress 事件（带节流，同一阶段 100 毫秒内最多一行）。
    void EmitProgress(const cbk::ProgressInfo& info, const std::string& phase) {
        if (!enabled_) return;
        // 节流在这一层做，不在 core 里。core 忠实上报每一块，
        // 但每行 JSON 都要 flush，一万个小文件全打出去会把 stdout 淹掉，
        // 界面忙着解析反而更卡。
        const ULONGLONG now = GetTickCount64();
        const bool finished = info.total_entries > 0 && info.done_entries >= info.total_entries;
        if (phase == last_phase_ && !finished && now - last_emit_ms_ < kThrottleMs) return;
        last_emit_ms_ = now;
        last_phase_ = phase;

        // 拼出 JSON：阶段、已完成/总条目数、已完成/总字节数，以及当前正在处理的路径。
        std::string line = "{\"event\":\"progress\",\"phase\":" + JsonQuote(phase) +
                           ",\"doneEntries\":" + std::to_string(info.done_entries) +
                           ",\"totalEntries\":" + std::to_string(info.total_entries) +
                           ",\"doneBytes\":" + std::to_string(info.done_bytes) +
                           ",\"totalBytes\":" + std::to_string(info.total_bytes);
        if (info.current != nullptr) {
            line += ",\"path\":" + JsonQuoteW(info.current->relative_path);
        }
        line += "}";
        EmitLine(line);
    }

    /// 告警事件：哪个路径、出了什么问题、Win32 错误码。界面把它列进告警列表。
    void OnWarn(const cbk::WarnInfo& info) override {
        if (!enabled_) return;
        EmitLine("{\"event\":\"warn\",\"path\":" + JsonQuoteW(info.path) +
                 ",\"message\":" + JsonQuoteW(info.message) +
                 ",\"winError\":" + std::to_string(info.win_error) + "}");
    }

    /// 结束事件：最终状态和各项统计。每次运行恰好输出一次。
    void OnResult(const cbk::ResultInfo& info) override {
        if (!enabled_) return;
        EmitLine(
            "{\"event\":\"result\",\"status\":" + std::to_string(static_cast<int>(info.status)) +
            ",\"entriesDone\":" + std::to_string(info.entries_done) +
            ",\"entriesSkipped\":" + std::to_string(info.entries_skipped) +
            ",\"bytesRead\":" + std::to_string(info.bytes_read) +
            ",\"bytesWritten\":" + std::to_string(info.bytes_written) + "}");
    }

    /// 引擎定期来问"要不要停"，答案就是 Ctrl+C 有没有按过。
    bool IsCancelled() override { return g_cancel_requested.load(); }

private:
    static constexpr ULONGLONG kThrottleMs = 100;  ///< 节流间隔（毫秒）

    bool enabled_;                ///< 是否输出 JSON
    ULONGLONG last_emit_ms_ = 0;  ///< 上一次输出 progress 的时刻
    std::string last_phase_;      ///< 上一次输出的阶段名，阶段变了立即输出
};

// ============================================================ 参数解析

/// 解析好的命令行。
struct Args {
    std::wstring source;                ///< --source 备份的源目录
    std::wstring dest;                  ///< --dest 备份时是 .cbk 文件，还原时是目标目录
    std::wstring archive;               ///< --archive 要还原/列出/校验的 .cbk 文件
    std::string packer = "cbk-native";  ///< --packer 打包格式，默认自有格式
    std::string compress;               ///< --compress 压缩算法名，空表示不压缩
    std::string encrypt;                ///< --encrypt 加密算法名，空表示不加密
    std::wstring overwrite = L"skip";   ///< --overwrite 还原时目标已存在怎么办
    bool password_stdin = false;        ///< --password-stdin 从标准输入读密码
    bool follow_symlinks = false;       ///< --follow-symlinks 跟随链接
    bool no_metadata = false;           ///< --no-metadata 还原时不设元数据
    bool json = false;                  ///< --json list 命令输出 JSON
    bool progress_json = false;         ///< --progress json 输出进度事件流

    std::wstring error;  ///< 非空表示解析失败
};

/// 取下一个参数值，缺了就记错误。
///
/// 注意它会直接推进调用方的循环下标——把值消费掉，免得下一轮又把这个值
/// 当成选项名去匹配。这是手写解析器最容易漏的一步：漏了的话，选项后面
/// 跟的那个路径会被当成"不认识的选项"报错。
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

/// 解析命令名之后的全部选项。手写，不引第三方库。
///
/// 理论上可以用 CLI11 之类，但为了一个五命令的程序引一个几千行的头文件
/// 不划算，而且 core 本来就是零第三方依赖，这边跟着保持一致——交付时
/// "整个项目除了 GoogleTest 和 PySide6 没有别的依赖"这句话更好说。
///
/// 遇到第一个错误就 break，不继续往下解析。参数写错时用户要的是一条
/// 清楚的提示，不是一串连锁报错。
///
/// @param start 从 argv 的第几项开始解析（跳过程序名和命令名）。
Args ParseArgs(const std::vector<std::wstring>& argv, size_t start) {
    Args args;
    for (size_t i = start; i < argv.size(); ++i) {
        const std::wstring& option = argv[i];
        std::wstring value;

        // 带值的选项：用 TakeValue 取紧跟其后的那个参数。
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
            // 开关型选项：出现即为 true，不带值。
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
    // 读一整行，再去掉行尾的回车换行（GUI 写进来的可能是 \r\n）。
    std::getline(std::cin, password);
    while (!password.empty() && (password.back() == '\r' || password.back() == '\n')) {
        password.pop_back();
    }
    return password;
}

/// Status 枚举值本身就按退出码约定排好了（0/1/2/3），直接转成 int。
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

/// cbk info：输出格式版本和已注册的打包器、压缩器、加密器列表。
/// GUI 启动时调用它来填下拉框，新增算法时界面不用改代码。
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

/// cbk backup：检查必填参数 → 组装 BackupOptions → 调 core 的 RunBackup。
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

    // 真正的备份全在 core 里；这里只负责把事件转成 JSON、把错误打到 stderr。
    JsonObserver observer(args.progress_json);
    const cbk::EngineResult result = cbk::RunBackup(options, &observer);

    if (!result.error.empty()) {
        const std::string message = cbk::ToUtf8(L"cbk: " + result.error + L"\n");
        std::fwrite(message.data(), 1, message.size(), stderr);
    }
    return ToExitCode(result.status);
}

/// cbk restore：检查参数 → 把 --overwrite 文本翻成枚举 → 调 core 的 RunRestore。
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

/// cbk list：只读条目表（不还原），按 --json 决定输出 JSON 还是人看的表格。
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

    // JSON 形式：包头信息 + 每个条目的编号、路径、类型、大小、属性、修改时间。
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

    // 文本形式：每行「类型 \t 大小 \t 路径」，给人在终端里直接看。
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

/// cbk verify：不解压不还原，只核对文件头、尾部和 CRC32 校验和，判断包是否完好。
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

/// 打印用法说明。输出到 stderr 而不是 stdout。
///
/// stdout 留给 JSON 事件流，GUI 在那头逐行解析。把用法文本混进去会让它
/// 撞上一行解析不了的东西。虽然只在打印帮助时才发生，但契约就是契约：
/// stdout 上只能有 JSON。
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

/// 真正的主流程：初始化控制台和算法注册表 → 识别命令 → 解析选项 → 分发到各命令函数。
int RunMain(int argc, wchar_t** argv) {
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

    // 把 C 风格的 argv 数组转成 vector，后面处理方便。
    std::vector<std::wstring> args;
    args.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i) args.push_back(argv[i]);

    // argv[1] 是命令名。help 和 info 不需要选项，先单独处理。
    const std::wstring command = args[1];
    if (command == L"--help" || command == L"-h" || command == L"help") {
        PrintUsage();
        return ToExitCode(cbk::Status::kOk);
    }
    if (command == L"info") return CommandInfo();

    // 其余命令：先解析选项，再按命令名分发。
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

/// 程序入口。用 wmain 而不是 main：参数直接是 UTF-16，中文路径不会乱码。
/// 外面包一层 try/catch：core 里不可恢复的错误会抛异常，在这里统一转成退出码 2。
int wmain(int argc, wchar_t** argv) {
    try {
        return RunMain(argc, argv);
    } catch (const std::exception& exc) {
        std::fprintf(stderr, "cbk: operation failed: %s\n", exc.what());
        return 2;
    } catch (...) {
        std::fprintf(stderr, "cbk: unexpected backend error\n");
        return 2;
    }
}
