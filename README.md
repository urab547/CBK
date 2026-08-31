# CBK — 数据备份软件

Windows 平台的数据备份软件，《软件开发综合实验》课程作业。
把一棵目录树完整存成单个 `.cbk` 容器文件，之后能一字不差地还原回去——
包括文件内容，也包括链接关系、时间戳和权限。

后台 C++17，界面 Python + PySide6，构建用 CMake + MSVC。

## 快速开始

需要 Visual Studio 2022 Build Tools（勾选「使用 C++ 的桌面开发」）和 CMake 3.20+。

```powershell
git clone <仓库地址>
cd CBK/code

cmake --preset msvc              # 配置
cmake --build --preset release   # 构建
ctest --preset release           # 跑单元测试

.\build\msvc\bin\Release\cbk.exe info
```

最后一条应该输出一行 JSON：

```json
{"formatVersion":1,"packers":[],"compressors":[],"ciphers":[]}
```

列表现在是空的——算法还没实现。这条命令就是给 GUI 用的：
界面启动时问一次核心支持哪些算法，用返回的列表填下拉框，
所以新增算法界面一行代码都不用改。

Debug 版把 `release` 换成 `debug`。

## 目录

```
code/core/          核心静态库，零第三方依赖
    include/cbk/    三个人共用的接口契约，改动要评审
    src/builtins.cpp  ★ 唯一的算法注册点，加算法在这里加一行
code/cli/           cbk.exe，参数解析 + JSON 事件输出
code/tests/         GoogleTest 单元测试
gui/                Python 界面
docs/               需求 / 设计 / 测试三份文档，以及开工说明和进度甘特图
scripts/            格式化、注释率统计
```

## 分工

| 模块 | 负责人 |
|---|---|
| 基础备份还原、文件类型支持、元数据支持 | Z |
| 打包解包、压缩解压 | 队友 A |
| 加密解密、图形界面 | 队友 B |

新人先读 `docs/CBK组内开工说明.pdf`，再读 `CLAUDE.md`。
排期见 `docs/项目进度.md`。

## 两条不能碰的红线

1. **后台逻辑必须是 C++。** 课程规定后台用脚本语言则基础分记 10 分（满分 40）。
   Python 只能写界面。
2. **扩展功能不许用第三方库直接实现。** 压缩不能用 zlib，加密不能用 OpenSSL，
   打包不能用 libarchive，否则该项扩展分打五折。`core/` 保持零第三方依赖。

细节见 `CLAUDE.md`。

## 协作

- `main` 分支受保护，只接受 Pull Request，CI 不绿合不进去。
- 分支名：`feat/core-scanner`、`feat/pack-tar`、`feat/gui` 这样。
- 提交信息用 `类型(范围): 说明`，例如 `feat(pack): 实现 ustar 头部写入`。
- 提交前跑 `.\scripts\format.ps1`。
