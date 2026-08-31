// Copyright 2026 CBK Project. cbk.exe 命令行入口。
//
// 这一层只做三件事：解析参数、调用 core、把事件按协议打到 stdout。
// 任何业务逻辑都不属于这里。
//
// 与 GUI 的契约见 docs/CBK组内开工说明（第 04 节）。
// 要点：每行一个完整 JSON，写完立刻 flush；密码走 stdin 不走参数；
// 退出码 0 成功 / 1 部分成功 / 2 失败 / 3 参数错误。

#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include "cbk/packer.h"
#include "cbk/stage.h"
#include "cbk/types.h"

namespace {

/// 把字符串转义成合法的 JSON 字符串字面量（含首尾双引号）。
std::string JsonQuote(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 2);
    out.push_back('"');
    for (char c : text) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out.push_back(c);
        }
    }
    out.push_back('"');
    return out;
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

/// `cbk info` —— 报告本程序支持哪些算法。
///
/// GUI 启动时先跑一次这个，用返回的列表填下拉框。
/// 这样队友新加一种算法，界面一行都不用改就多出一个选项。
int CommandInfo() {
    const cbk::PackerRegistry& packers = cbk::PackerRegistry::Instance();
    const cbk::StageRegistry& stages = cbk::StageRegistry::Instance();

    std::string out = "{";
    out += "\"formatVersion\":" + std::to_string(cbk::kFormatVersion);
    out += ",\"packers\":" + JsonArray(packers.Names());
    out += ",\"compressors\":" + JsonArray(stages.Names(cbk::StageKind::kCompress));
    out += ",\"ciphers\":" + JsonArray(stages.Names(cbk::StageKind::kEncrypt));
    out += "}";

    std::cout << out << std::endl;  // endl 即 flush，协议要求逐行刷新
    return static_cast<int>(cbk::Status::kOk);
}

void PrintUsage() {
    std::cerr <<
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
}

/// 尚未实现的命令统一走这里，保证 CI 和 GUI 拿到的是明确的失败而不是崩溃。
int NotImplemented(const std::string& command) {
    std::cerr << "cbk: 命令 '" << command << "' 尚未实现\n";
    return static_cast<int>(cbk::Status::kFailed);
}

}  // namespace

int main(int argc, char** argv) {
    // 内置算法在这里注册一次，之后 core 的任何地方都能查到。
    cbk::RegisterBuiltinPackers();
    cbk::RegisterBuiltinStages();

    if (argc < 2) {
        PrintUsage();
        return static_cast<int>(cbk::Status::kBadArgs);
    }

    const std::string command = argv[1];
    if (command == "info")    return CommandInfo();
    if (command == "backup")  return NotImplemented(command);
    if (command == "restore") return NotImplemented(command);
    if (command == "list")    return NotImplemented(command);
    if (command == "verify")  return NotImplemented(command);
    if (command == "--help" || command == "-h" || command == "help") {
        PrintUsage();
        return static_cast<int>(cbk::Status::kOk);
    }

    std::cerr << "cbk: 未知命令 '" << command << "'\n\n";
    PrintUsage();
    return static_cast<int>(cbk::Status::kBadArgs);
}
