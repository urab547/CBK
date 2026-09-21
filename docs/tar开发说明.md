# tar 打包解包：开发说明


| 文件 | 作用 |
|---|---|
| `code/core/src/packers/tar_packer.h` | 声明 TarPacker，实现组员已经冻结的 IPacker 接口 |
| `code/core/src/packers/tar_packer.cpp` | 自己编写头部、PAX、补齐和解包逻辑 |
| `code/core/src/builtins.cpp` | 注册名字 `tar`，让 CLI 能发现它 |
| `code/core/CMakeLists.txt` | 把 tar 源码加入核心库 |
| `code/tests/unit/tar_packer_test.cpp` | 格式、边界、异常和引擎集成测试 |
| `code/tests/CMakeLists.txt` | 把新测试加入测试程序 |
| `scripts/test_tar_compat.ps1` | 用 CMake 自带的 tar 独立解包并比较 SHA256，仅用于测试 |

公共接口、容器版本、扫描器、还原引擎、GUI 均未修改。生产实现没有调用第三方打包库。

## tar 如何保存文件

每个条目按下面的布局写出：

```text
512 字节 PAX 头（type=x）
PAX 文本 + 补零到 512 字节边界
512 字节 ustar 文件头
文件内容 + 补零到 512 字节边界
……下一个条目……
两个全零的 512 字节块（整个归档结束）
```

ustar 头保存名字、类型、大小、修改时间与校验和。数字按 ASCII 八进制写入；
头部校验和是把校验字段暂当空格后，对 512 个无符号字节求和。
这是 tar 自己的头校验，和外层 `.cbk` 的 CRC32 是两种不同的检查。

PAX 是 tar 的标准扩展机制，每条记录形如 `长度 键=值\n`，长度包含整条记录本身。
`path`/`linkpath` 保存 UTF-8 路径；超过 ustar 长度字段范围的文件用 `size` 保存十进制大小。
自定义键 `CBK.meta` 保存现有 EntryMeta 编码的十六进制文本，完整保留 Windows 时间戳、
SDDL、属性、链接种类及条目 id。普通 tar 工具可以忽略这个自定义键并读取文件内容。
这里新增的是打包器内部格式，不改变已冻结的 `.cbk` 外层格式或 EntryMeta 布局。

硬链接用标准的“指向前一个文件路径”表示，同时在 CBK.meta 中保留原始引用 id。
junction 和目录符号链接在标准层映射为符号链接，在 CBK.meta 中区分原来的类型。
不支持的 Windows 对象只写空占位；由 CBK 解包时仍返回 unsupported 类型。

## 当前支持范围和限制

- 写入 ustar 头和逐条目 PAX 扩展；解包支持自身输出、普通 ustar，以及所需的逐条目 PAX 字段。
- 不承诺支持所有 tar 变体：GNU 长名头、稀疏文件、PAX 全局头、设备/FIFO 等明确拒绝。
- 外部 tar 没有 CBK.meta 时，只恢复标准字段能表达的信息；不能凭空恢复 Windows ACL、
  100ns 精度的三种时间或目录符号链接类型。外部 PAX 的额外时间/权限字段目前不导入。
- `Unpack` 是打包器接口，不代表 CLI 新增了直接导入任意 `.tar` 的命令；CLI 仍读取 `.cbk`。
- 文件内容直接流式转发，解包缓冲 64 KB。元数据缓冲有限制，但硬链接路径表随文件数增长。
- **文件大小必须在备份期间稳定。** tar 先写头再写内容，接口不支持回头改长度。
  写多或写少都会抛出 `std::runtime_error`，不能静默截断或补出假内容。
  组员当前 CLI 缺少统一异常捕获，因此这类异常的友好退出码与清理行为还需要后续联调；
  本次没有修改其异常处理。演示请使用不在持续写入的测试目录。
- 解包会检查头校验、截断、PAX 长度、CBK/标准字段一致性及相对路径。
  这不替代还原引擎对目标目录重解析点和覆盖策略的检查。

## 构建与验证

组里标准环境是 Visual Studio C++ Build Tools；在 `code/` 中执行：

```powershell
cmake --preset msvc
cmake --build --preset release
ctest --preset release
.\build\msvc\bin\Release\cbk.exe info
```

`info` 的 packers 应包含 `cbk-native` 和 `tar`。选择 tar 的参数是 `--packer tar`：

```powershell
cbk.exe backup --source <测试目录> --dest <备份路径.cbk> --packer tar --progress json
cbk.exe verify --archive <备份路径.cbk>
cbk.exe restore --archive <备份路径.cbk> --dest <新目录>
```

仓库根目录运行独立兼容性检查：

```powershell
.\scripts\test_tar_compat.ps1
# 如果可执行文件在其它位置：
.\scripts\test_tar_compat.ps1 -Cbk .\code\build\mingw\bin\cbk.exe
```

脚本创建独立的测试目录，包含中文长名、二进制、空文件和硬链接，备份后取出 tar 数据区，
分别由 CBK 和 CMake 的 `-E tar` 工具还原，再比较每个文件的 SHA256。
产物留在忽略的 build 目录中。这个工具是独立实现，只参与测试。

本机 Windows 自带的 bsdtar 3.8.8 会拒绝上述中文路径（empty or unreadable filename），
显式指定 UTF-8 也没有解决。同一份数据已由 CMake tar 正确识别，Python 标准库 tarfile
也已逐文件读取并比对内容成功，因此脚本使用 CMake tar，不能声称所有 Windows tar 版本
都已通过中文路径兼容性测试。

本次本机验证使用已有 MinGW 编译核心及测试，未验证 MSVC；组内 Windows CI 仍使用原配置。
全量测试共 187 个：181 个通过，6 个原有符号链接测试因本机权限条件跳过，0 个失败。
新增 26 个 TarPacker 测试全部通过。核心/CLI 的 cpplint、改动 C++ 文件的 clang-format
检查及 git diff --check 均通过。兼容性脚本验证了 5 个文件的两份还原结果，SHA256 一致。
本机没有找到 Visual Studio，直接运行 msvc 预设会遇到生成器错误。MinGW 测试使用独立
`code/build/mingw` 目录和 Windows 编译定义，CLI 额外用 `-municode` 链接 wmain；
没有为此更改组里的构建预设。


## 格式参考

- [GNU tar 官方格式说明](https://www.gnu.org/s/tar/manual/html_node/Standard.html)
- [POSIX pax 格式规范](https://pubs.opengroup.org/onlinepubs/9699919799/utilities/pax.html)

实现按格式说明手写；外部 tar 工具仅参与独立测试。
