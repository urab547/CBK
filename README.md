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

提 issue 时按下表打标签，全部标签见 [Labels 页面](https://github.com/urab547/CBK/labels)。

| 模块 | 负责人 | Issue 标签 |
|---|---|---|
| 基础备份还原（40 分） | [@urab547](https://github.com/urab547) | `基础备份还原` |
| 文件类型支持（10 分） | [@urab547](https://github.com/urab547) | `文件类型支持` |
| 元数据支持（10 分） | [@urab547](https://github.com/urab547) | `元数据支持` |
| 打包解包（10 分/种） | [@onepiece142575-sudo](https://github.com/onepiece142575-sudo) | `打包解包` |
| 压缩解压（10 分/种） | [@onepiece142575-sudo](https://github.com/onepiece142575-sudo) | `压缩解压` |
| 加密解密（10 分/种） | [@27588569](https://github.com/27588569) | `加密解密` |
| 图形界面 | [@27588569](https://github.com/27588569) | `图形界面` |
| 测试矩阵 / 三份文档 / 注释率 | 全员 | `工程质量` |

各人自己拆自己模块的 issue。评分表里「项目管理工具的使用情况」「人员分工安排情况」
看的就是这些卡片上有没有负责人、有没有截止日期，别人代提的不算你的过程证据。
格式照着 [#1](https://github.com/urab547/CBK/issues/1) 写：任务清单 + 已知的坑 + 验收标准。

### 不用等别人写完

`core/include/cbk/*.h` 四个接口头文件（`types.h` / `sink.h` / `packer.h` / `stage.h`）
**已经冻结并合进 `main`**，拿到就能开工：

- 打包器只依赖 `IPacker` + `ISink`。往返测试手捏几个 `EntryMeta` 就能写，不依赖 Scanner。
- 压缩、加密只依赖 `IStage`。它们只认字节流，跟目录树没关系。
- 界面可以先对着 `cbk info` 的输出和 JSON 事件协议做，拿假事件流驱动进度条。

真正需要等引擎落地的只有端到端联调。

新人先读 `docs/CBK组内开工说明.pdf`，再读 `CLAUDE.md`。
排期见 `docs/项目进度.md`。

## 两条不能碰的红线

1. **后台逻辑必须是 C++。** 课程规定后台用脚本语言则基础分记 10 分（满分 40）。
   Python 只能写界面。
2. **扩展功能不许用第三方库直接实现。** 压缩不能用 zlib，加密不能用 OpenSSL，
   打包不能用 libarchive，否则该项扩展分打五折。`core/` 保持零第三方依赖。

细节见 `CLAUDE.md`。

## 协作

`main` 分支开了保护，**不能直接 push**。规则：

- 必须走 Pull Request
- 两个 CI 检查（`构建与测试 (Windows)`、`代码规范检查`）都得绿
- **不强制要求 Approve**（见下）
- 分支要先跟 `main` 同步
- PR 上的评论要解决完
- 禁止强推、禁止删分支

**为什么不强制 Approve。** GitHub 不允许给自己的 PR 点 Approve，这是平台
硬规则、没有开关能改。三个人的小组里，强制"至少 1 人批准"的结果是每个人
都得等另外两个人有空，而绝大多数 PR 其实只要 CI 绿就够了。所以把必需的
批准数设成了 0：**CI 绿就能合**。

这不等于不用 review。改到别人模块的接口、改容器格式、改 `include/cbk/`
下的契约，仍然要拉一个人看过再合——只是靠约定，不靠机器卡着。

### 一轮完整流程

```powershell
git checkout main; git pull
git checkout -b feat/pack-tar     # 分支名：feat/<范围>-<东西>

# ……改代码……

.\scripts\format.ps1              # 提交前必跑
git add -A
git commit -m "feat(pack): 实现 ustar 头部的写入与解析"
git push -u origin feat/pack-tar
gh pr create --fill --base main
```

PR 开出来后 CI 自动跑，两个检查都绿了就能合：

```powershell
gh pr merge --squash --delete-branch
```

### 提交信息

`类型(范围): 说明`，类型用 `feat` / `fix` / `test` / `docs` / `chore`。
这样 `git log --oneline` 直接能当答辩 PPT 里「项目进度把控」那页的素材。

### 里程碑 tag

`v0.1-base` / `v0.2-filetype` / `v0.3-metadata` / `v0.4-pack` / `v0.5-compress` /
`v0.6-crypt` / `v1.0-gui`，每个 tag 对应一个能演示的版本。
