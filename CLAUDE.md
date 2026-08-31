# CBK — 数据备份软件

Windows 平台的数据备份软件，《软件开发综合实验》课程项目。
把一棵目录树完整存成单个 `.cbk` 容器文件，之后能一字不差地还原回去——
包括文件内容，也包括链接关系、时间戳和权限。

三人小组。本文件目前只覆盖 **Z 负责的部分**（基础备份还原、文件类型支持、
元数据支持）以及全项目共用的硬约束。队友的模块见文末的补充区。

---

## 一、不可违反的约束

这四条来自课程评分规则和格式契约，违反的代价是直接扣分或让已生成的备份包作废。
写任何代码之前先确认没有踩到。

### 1. 后台逻辑必须是 C++

课件原文：*"用户界面可以采用脚本语言编写；但若后台逻辑也选择脚本语言，
则小组基础分记 10 分。"* 基础分满分 40。

- 备份、还原、遍历、元数据、打包、压缩、加密——全部在 C++ 里。
- Python 只允许出现在 `gui/`，且只做参数收集、进程启动、进度渲染。
- **不要为了方便把任何业务判断挪进 Python。**

### 2. 扩展功能不许用第三方库直接实现

课件原文：*"对所有扩展功能，如使用第三方库/程序/代码'直接'实现，
对应功能扩展分总分记为原来的 50%。"*

- `core/` 这个静态库**零第三方依赖**，只能用 C++ 标准库和 Windows SDK。
- 压缩不许用 zlib / miniz；加密不许用 OpenSSL / Windows CryptoAPI；
  打包不许用 libarchive。算法自己写。
- Win32 API 是操作系统接口，不算第三方库，随便用。
- 第三方依赖只能出现在 `tests/`（GoogleTest）和 `gui/`（PySide6）。

**如果我让你"实现压缩/加密"，不要建议引入任何压缩或加密库。**

### 3. 流水线顺序写死为「打包 → 压缩 → 加密」

- 加密输出是高熵伪随机字节，压缩它压不动还会变大，所以压缩必须在加密之前。
- 打包决定条目边界（结构层），压缩加密只认字节（字节层）；
  结构层在内、字节层在外，解包时才能先还原字节流再按结构解析。
- `ValidatePipelineOrder()` 负责拦截配错的顺序，返回 `Status::kBadArgs`。

### 4. 容器格式已冻结

`core/include/cbk/types.h` 里的 `kMagic` / `kFormatVersion` /
`kFileHeaderSize` / `kFooterSize`，以及 `EntryMeta` 的字段布局，
是三个人共用的契约。改动必须先在组内评审并升 `kFormatVersion`——
否则之前生成的备份包全部读不了。`registry_test.cpp` 里有测试盯着这几个常量。

---

## 二、Z 负责的三块

### 基础备份还原（40 分）

- `scanner.*` — 目录遍历、类型识别、硬链接去重
- `archive.*` — `.cbk` 容器的读写（文件头、数据区、索引区、尾部校验）
- `engine.*` — `BackupEngine` / `RestoreEngine`，编排整条流水线
- `platform_win.*` — Win32 薄封装：路径转换、句柄、特权提升

### 文件类型支持（10 分）

符号链接（文件/目录两种）、硬链接、目录联接 junction。
其它重解析点识别为 `FileType::kUnsupported`，记录 `reparse_tag` 并告警，不递归进去。

命名管道和设备文件在 Windows 上位于 `\\.\pipe\` 和 `\\.\` 命名空间，
**不出现在文件系统目录树里**，所以备份目录树时遇不到。
这是平台差异，要在设计文档里写清楚，不是"没做"。

### 元数据支持（10 分）

属性位、创建/访问/修改三个时间戳（100ns 精度）、属主 SID、DACL 权限表。
SACL（审计）不做，与评分项无关。

---

## 三、写这部分代码时必须注意的坑

这些是 Windows 备份软件的经典陷阱，踩了不会立刻报错，
而是在深目录、中文名、非管理员、还原阶段才暴露。

**路径**

- 一律加 `\\?\` 前缀绕过 260 字符的 `MAX_PATH` 限制
  （UNC 路径是 `\\?\UNC\server\share`）。
- `\\?\` 路径不做任何规范化，所以传进去之前必须自己转绝对路径、
  去掉 `.` 和 `..`、把 `/` 换成 `\`。
- 内存里一律 `std::wstring`（UTF-16），只有写进容器时才转 UTF-8。

**打开文件**

- 三个共享位全开：`FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE`，
  否则别人正在写的文件根本打不开。
- 必须带 `FILE_FLAG_BACKUP_SEMANTICS`：它既允许打开目录句柄
  （设目录时间戳要用），也配合 `SeBackupPrivilege` 绕过 ACL 读取无权访问的文件。
- 打不开的文件发 warn、跳过、继续，最终退出码 1，**不要中断整个备份**。

**特权提升**

- 启动时尝试启用 `SE_BACKUP_NAME` / `SE_RESTORE_NAME` / `SE_SECURITY_NAME`。
- `AdjustTokenPrivileges` **即使部分失败也返回 TRUE**，
  必须再查 `GetLastError() == ERROR_NOT_ALL_ASSIGNED` 才知道到底成没成。
- 拿不到特权不是致命错误，降级运行即可。

**还原的顺序**（三个陷阱，都会静默出错）

1. 目录时间戳必须**后序**设置：建目录 → 填内容 → 最后才设时间戳。
   往目录里写文件会更新目录的 `lastWriteTime`。
2. 只读属性必须**最后**设。先设了 `FILE_ATTRIBUTE_READONLY`，
   后续设时间戳和 ACL 会失败。
3. 硬链接必须在目标条目还原之后建。按 id 升序还原就自然满足。

所以 `RestoreEngine` 分三趟：
建所有目录（不设元数据）→ 按 id 升序还原内容和链接、逐个设元数据 →
**逆序**遍历目录列表设置目录的时间戳、ACL、属性位。

**属性位**

还原时要屏蔽掉不可设置的位。`DIRECTORY` / `REPARSE_POINT` / `COMPRESSED` /
`ENCRYPTED` / `SPARSE_FILE` 是文件系统维护的，硬塞会返回 `ERROR_INVALID_PARAMETER`。
可设置的只有 `READONLY` / `HIDDEN` / `SYSTEM` / `ARCHIVE` / `TEMPORARY` /
`OFFLINE` / `NOT_CONTENT_INDEXED`。

**符号链接与 junction**

- 读目标用 `DeviceIoControl(FSCTL_GET_REPARSE_POINT)` 拿 `REPARSE_DATA_BUFFER`，
  存 `PrintName` 和 `SYMLINK_FLAG_RELATIVE` 标志。
  **不要用 `GetFinalPathNameByHandle`**——那会把链接解析成最终目标，
  正好丢掉我们要备份的信息。
- 创建符号链接：`CreateSymbolicLinkW` 总是先带上
  `SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE (0x2)` 试一次
  （开了开发者模式的 Win10/11 上免管理员），失败了再不带该标志重试一次
  （老系统上这个标志会导致 `ERROR_INVALID_PARAMETER`）。
  两次都失败就降级并 warn，不要中断还原。
- junction 用 `FSCTL_SET_REPARSE_POINT` 自己构造缓冲区写入，**不需要管理员权限**。
- 默认**不跟随**链接。跟随模式下必须用 `(volumeSerial, fileIndex)` 集合断环。

**硬链接**

- 只对 `nNumberOfLinks > 1` 的文件查表，避免给几万个普通文件白建哈希表项。
- 键是 `(dwVolumeSerialNumber, nFileIndexHigh, nFileIndexLow)`，
  来自 `GetFileInformationByHandle`。
- 硬链接不能跨卷，还原目标跨卷时降级为复制并 warn。

**时间戳**

- 存 `FILETIME` 原值，不要转 `time_t`（秒），会丢精度导致往返测试挂掉。
- `lastAccessTime` 在 Windows 上可能被系统禁用更新
  （`NtfsDisableLastAccessUpdate`），断言访问时间时要留容差或在测试文档里说明。

**安全描述符**

- 存 SDDL 文本而不是二进制：可读、可 diff、写测试时能肉眼验证。
  `ConvertSecurityDescriptorToStringSecurityDescriptorW` 与其逆函数无损互转。
- 设置属主需要 `SeRestorePrivilege`。没有就跳过 OWNER 段、只还原 DACL、发 warn。
- SID 跨机器无意义。还原到异机时无法解析的 ACE 要 warn。
  这是所有备份软件的固有限制，写进文档而不是假装不存在。
- 注意 `SE_DACL_PROTECTED` 控制位，决定要不要用
  `PROTECTED_DACL_SECURITY_INFORMATION` 阻断继承。

**大文件与内存**

- 一切流式，固定 `kIoBlockSize`（64 KB）缓冲，任何情况下不整文件读进内存。
- 所有偏移和长度字段一律 `uint64_t`，禁止用 `long` 或 `DWORD`。

---

## 四、代码约定

- C++17。`std::filesystem` 只用于路径拼接，遍历用 `FindFirstFileW` /
  `FindNextFileW`（`WIN32_FIND_DATAW` 一次就带回属性、时间戳、大小，
  重解析点标签在 `dwReserved0` 里，比逐个 `CreateFile` 快一个数量级）。
- 遍历用**显式栈迭代**，不用递归——深目录会爆栈，而且迭代版本才好做取消。
- 命名遵循 Google C++ Style：类型 `PascalCase`，函数 `PascalCase`，
  变量 `snake_case`，成员变量 `snake_case_` 带尾下划线，常量 `kPascalCase`。
- 缩进 4 空格，行宽 100。提交前跑 `scripts/format.ps1`（clang-format）。
- 注释用 Doxygen 风格写在头文件里。评分表有"注释达到 10% / 20%"两档，
  别等到最后一周才发现不够。
- 错误处理：可预期的失败（文件被占用、无权限）发 warn 事件并继续；
  不可恢复的失败抛异常，由 CLI 层捕获转成退出码 2。

---

## 五、构建与测试

```powershell
cd code
cmake --preset msvc            # 配置（需要 VS 2022 Build Tools）
cmake --build --preset release # 构建
ctest --preset release         # 跑单元测试
.\build\msvc\bin\Release\cbk.exe info   # 冒烟：应输出一行 JSON
```

Debug 版把 `release` 换成 `debug`。

CI 在每个 PR 上跑同样这几条，外加 cpplint。`main` 分支受保护，CI 不绿合不进去。

---

## 六、目录结构

```
code/
  core/                  静态库 libcbk，零第三方依赖
    include/cbk/         对外接口（三个人共用的契约，改动要评审）
      types.h            EntryMeta / FileType / 格式常量
      sink.h             ISink / ISource
      packer.h           IPacker + 注册表        ← 队友 A 实现
      stage.h            IStage + 注册表          ← 队友 A / B 实现
    src/
      registry.cpp       注册表实现
      builtins.cpp       ★ 唯一的算法注册点，加算法在这里加一行
      packers/ stages/   各算法实现
  cli/                   cbk.exe，参数解析 + JSON 事件输出
  tests/unit/            GoogleTest
gui/                     Python + PySide6            ← 队友 B
docs/                    需求 / 设计 / 测试三份文档
```

`gui/core_bridge.py` 是**唯一**允许启动 `cbk.exe` 的 Python 文件。
这条规矩保证业务逻辑不会一点点渗进 Python 层（见约束 1）。

---

## 七、与 GUI 的接口契约

- 命令：`backup` / `restore` / `list` / `verify` / `info`
- 密码走 **stdin**（`--password-stdin`），不走命令行参数——
  命令行参数在 Windows 上能被其它进程读到。
- 进度走 **stdout**，每行一个完整 JSON，**写完立刻 flush**。
  不 flush 的话输出会攒在管道缓冲里，界面进度条会一直不动然后一次跳完。
  事件类型：`start` / `progress` / `warn` / `result`。
- 退出码：0 成功 / 1 部分成功（有条目被跳过，包仍可用）/ 2 失败 / 3 参数错误。
- `cbk info` 输出本程序支持的算法列表，GUI 用它动态填下拉框——
  新增算法界面不用改代码。

---

## 八、队友补充区

队友接手自己模块后，在下面追加各自的小节。
不要修改上面第一节的四条约束，那是全组共同的前提。

<!-- 队友 A（打包解包 / 压缩解压）：在此追加 -->

<!-- 队友 B（加密解密 / 图形界面）：在此追加 -->
