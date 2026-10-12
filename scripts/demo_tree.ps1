# 生成一棵调试 / 演示用的目录树，三块功能都能在里面看到：
#
#   基础备份还原   多层目录、中文名、空目录、空文件、跨好几个 64 KB 块的大文件
#   文件类型支持   硬链接、目录联接 junction、符号链接（有权限才建得出来）
#   元数据支持     只读 + 隐藏属性、一个明显是过去的修改时间
#
# 用法（在仓库根目录）：
#   .\scripts\demo_tree.ps1                  重建整棵演示树，并清掉上次的备份包和还原结果
#   .\scripts\demo_tree.ps1 -CleanRestore    只清掉还原目录，演示树不动
#
# 所有东西都放在 code\build\demo\ 下面。build\ 已经在 .gitignore 里，
# 不会被误提交。VS Code 里按 F5 调试时会自动调用这个脚本，一般不用手动跑。

param(
    [switch]$CleanRestore
)

$ErrorActionPreference = "Stop"

$demo = Join-Path $PSScriptRoot "..\code\build\demo"
$demo = [System.IO.Path]::GetFullPath($demo)
$src = Join-Path $demo "src"
$back = Join-Path $demo "back"
$archive = Join-Path $demo "out.cbk"

# 安全地删一棵目录树。
#
# 不能直接 Remove-Item -Recurse：Windows PowerShell 5.1 遇到 junction 会顺着
# 它钻进去，把链接**指向的那个目录**里的东西也删掉。所以先把树里所有的
# 链接本身摘掉（只删链接、不碰目标），再删剩下的普通文件和目录。
# 只读位也得先摘，不然删不掉。
function Remove-TreeSafely([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return }

    $links = Get-ChildItem -LiteralPath $path -Recurse -Force -Attributes ReparsePoint `
                           -ErrorAction SilentlyContinue
    foreach ($link in $links) {
        if ($link.PSIsContainer) {
            [System.IO.Directory]::Delete($link.FullName)   # 只删链接本身
        } else {
            [System.IO.File]::Delete($link.FullName)
        }
    }

    Get-ChildItem -LiteralPath $path -Recurse -Force -ErrorAction SilentlyContinue |
        ForEach-Object {
            try { $_.Attributes = [System.IO.FileAttributes]::Normal } catch {}
        }
    Remove-Item -LiteralPath $path -Recurse -Force
}

if ($CleanRestore) {
    Remove-TreeSafely $back
    Write-Host "已清空还原目录：$back"
    exit 0
}

Remove-TreeSafely $demo
New-Item -ItemType Directory -Force -Path $src | Out-Null

# ---------------------------------------------------------------- 基础备份还原

Set-Content -LiteralPath (Join-Path $src "你好.txt") -Value "你好，备份世界" -Encoding UTF8

$sub = Join-Path $src "子目录"
$deeper = Join-Path $sub "更深"
New-Item -ItemType Directory -Force -Path $deeper | Out-Null
Set-Content -LiteralPath (Join-Path $sub "note.md") -Value "# 标题`n正文" -Encoding UTF8

# 200 KB 的随机字节：大于 64 KB，备份时会分成四块读，能在调试器里看到读块循环。
$bytes = New-Object byte[] 200000
(New-Object System.Random 42).NextBytes($bytes)
[System.IO.File]::WriteAllBytes((Join-Path $deeper "大文件.bin"), $bytes)

New-Item -ItemType Directory -Force -Path (Join-Path $src "空目录") | Out-Null
New-Item -ItemType File -Force -Path (Join-Path $src "空文件.dat") | Out-Null

# ---------------------------------------------------------------- 文件类型支持

# 硬链接：两个名字、同一份数据。普通用户就能建。
$original = Join-Path $src "硬链接_原件.txt"
Set-Content -LiteralPath $original -Value "这两个名字指向同一份数据" -Encoding UTF8
New-Item -ItemType HardLink -Path (Join-Path $src "硬链接_别名.txt") -Target $original | Out-Null

# 目录联接：也不需要管理员权限。
New-Item -ItemType Junction -Path (Join-Path $src "联接_指向子目录") -Target $sub | Out-Null

# 符号链接：要管理员或开发者模式，建不出来就跳过，不影响其它东西。
try {
    New-Item -ItemType SymbolicLink -Path (Join-Path $src "符号链接.txt") `
             -Target (Join-Path $src "你好.txt") -ErrorAction Stop | Out-Null
    $symlinkNote = "已建"
} catch {
    $symlinkNote = "没建出来（需要管理员或开发者模式），跳过"
}

# ---------------------------------------------------------------- 元数据支持

# 只读 + 隐藏。还原时属性位要原样回来，而且必须最后设——先设了只读，后面
# 设时间戳会失败。这个文件就是用来盯这个顺序的。
$readonly = Join-Path $src "只读隐藏.txt"
Set-Content -LiteralPath $readonly -Value "我是只读的，也是隐藏的" -Encoding UTF8

# 一个明显是过去的修改时间，还原后一眼就能看出时间戳有没有被保住。
$oldTime = Get-Date "2020-01-01 08:00:00"
(Get-Item -LiteralPath (Join-Path $src "你好.txt")).LastWriteTime = $oldTime
(Get-Item -LiteralPath $readonly).LastWriteTime = $oldTime

# 属性位放在时间戳之后设——理由同上。
(Get-Item -LiteralPath $readonly).Attributes =
    [System.IO.FileAttributes]::ReadOnly -bor [System.IO.FileAttributes]::Hidden

Write-Host "演示目录已生成：$src"
Write-Host "  符号链接：$symlinkNote"
Write-Host "  备份包会写到：$archive"
Write-Host "  还原会写到：  $back"
