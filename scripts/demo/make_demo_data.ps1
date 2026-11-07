# 生成答辩演示数据。
#
# 用法（建议在「以管理员身份运行」的 PowerShell 里，于仓库根目录执行）：
#   .\scripts\demo\make_demo_data.ps1                     默认生成到 D:\CBK-Demo，大文件 1 GB
#   .\scripts\demo\make_demo_data.ps1 -BigSizeGB 2        大文件改成 2 GB
#   .\scripts\demo\make_demo_data.ps1 -SkipBig            不生成大文件（快速重建其它几组）
#   .\scripts\demo\make_demo_data.ps1 -Root E:\Demo       换个位置
#
# 生成的目录：
#   <Root>\src\01-简单         几个文件，演示最基本的备份还原
#   <Root>\src\02-复杂         深层嵌套、长路径(>260)、各种奇怪文件名、2000 个小文件、空目录
#   <Root>\src\03-大文件       GB 级随机数据
#   <Root>\src\04-特殊文件     符号链接(文件/目录/相对/绝对/悬空)、junction、硬链接
#   <Root>\src\05-元数据       精确到 100ns 的时间戳、各种属性位、自定义权限、属主
#   <Root>\backup              放备份包（空）
#   <Root>\restore             放还原结果（空）
#
# 重复运行会先删掉 <Root> 下的 src / backup / restore 再重新生成。
# 为防误删，<Root> 必须是本脚本建的（里面有 .cbk-demo 标记文件）或者是空目录。
#
# 不是管理员时：符号链接、属主两项会跳过并提示，其余照常生成。

param(
    [string]$Root = "D:\CBK-Demo",
    [double]$BigSizeGB = 1,
    [int]$SmallFiles = 2000,
    [switch]$SkipBig
)

$ErrorActionPreference = "Stop"

if (-not ("CbkDemo" -as [type])) {
    Add-Type -Path (Join-Path $PSScriptRoot "CbkDemoNative.cs")
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$isAdmin = ([Security.Principal.WindowsPrincipal]$identity).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
$userSid = $identity.User.Value
$notes = New-Object System.Collections.Generic.List[string]

function Section([string]$title) { Write-Host ""; Write-Host "== $title" -ForegroundColor Cyan }
function Step([string]$text) { Write-Host "   $text" }
function Note([string]$text) { Write-Host "   ! $text" -ForegroundColor Yellow; $notes.Add($text) }

# 生成一个 FILETIME（UTC），可以精确到 100 纳秒（ticks）。
function MakeTime([int]$y, [int]$mo, [int]$d, [int]$h, [int]$mi, [int]$s, [int]$ticks = 0) {
    $t = New-Object DateTime $y, $mo, $d, $h, $mi, $s, ([DateTimeKind]::Utc)
    return $t.AddTicks($ticks).ToFileTimeUtc()
}

function Write-Text([string]$path, [string]$text) {
    [CbkDemo]::EnsureDir([IO.Path]::GetDirectoryName($path))
    [CbkDemo]::WriteText($path, $text)
}

# 调 icacls，失败就抛出。参数里的账户一律写成 *SID，不受系统语言影响。
function Icacls([string]$path) {
    $output = & icacls.exe $path @args 2>&1
    if ($LASTEXITCODE -ne 0) { throw "icacls 失败（$path）：$output" }
}

# ------------------------------------------------------------------ 准备根目录

$Root = [IO.Path]::GetFullPath($Root)
$marker = Join-Path $Root ".cbk-demo"
if (Test-Path -LiteralPath $Root) {
    $hasMarker = Test-Path -LiteralPath $marker
    $isEmpty = -not (Get-ChildItem -LiteralPath $Root -Force | Select-Object -First 1)
    if (-not $hasMarker -and -not $isEmpty) {
        throw "$Root 已存在且不是本脚本建的目录，为防误删已停止。请换一个 -Root。"
    }
}

$src = Join-Path $Root "src"
$backup = Join-Path $Root "backup"
$restore = Join-Path $Root "restore"

Section "清理旧数据：$Root"
foreach ($dir in @($src, $backup, $restore)) { [CbkDemo]::DeleteTree($dir) }
foreach ($dir in @($src, $backup, $restore)) { [CbkDemo]::EnsureDir($dir) }
Set-Content -LiteralPath $marker -Value "CBK 演示数据目录，由 make_demo_data.ps1 生成" -Encoding UTF8
Step ("当前身份：" + $identity.Name + $(if ($isAdmin) { "（管理员）" } else { "（非管理员）" }))
if (-not $isAdmin) { Note "不是管理员：符号链接、属主两项会跳过。答辩前请用「以管理员身份运行」重新生成。" }

# ------------------------------------------------------------------ 01 简单

Section "01-简单"
$d = Join-Path $src "01-简单"
Write-Text (Join-Path $d "readme.txt") "这是最简单的一组演示数据。`r`n备份后再还原，内容应当一字不差。`r`n"
Write-Text (Join-Path $d "笔记.md") "# 课堂笔记`r`n`r`n- 打包 → 压缩 → 加密`r`n- 还原时反过来`r`n"
Write-Text (Join-Path $d "成绩.csv") "学号,姓名,分数`r`n001,张三,95`r`n002,李四,88`r`n"
Write-Text (Join-Path $d "子目录\说明.txt") "子目录里的文件。`r`n"
Step "4 个文件、1 个子目录"

# ------------------------------------------------------------------ 02 复杂

Section "02-复杂"
$d = Join-Path $src "02-复杂"

# 深层嵌套：30 层
$deep = Join-Path $d "深层嵌套"
for ($i = 1; $i -le 30; $i++) { $deep = Join-Path $deep ("第{0:D2}层" -f $i) }
Write-Text (Join-Path $deep "最底层.txt") "我在第 30 层。`r`n"
Step "深层嵌套 30 层"

# 长路径：超过 Windows 传统的 260 字符上限
$long = Join-Path $d "长路径"
for ($i = 1; $i -le 6; $i++) { $long = Join-Path $long (("长目录名{0}_" -f $i) + ("x" * 50)) }
$longFile = Join-Path $long "超过260字符的文件.txt"
Write-Text $longFile "这个文件的完整路径有 $($longFile.Length) 个字符。`r`n"
Step "长路径文件，完整路径 $($longFile.Length) 个字符"

# 各种文件名
$names = @("带 空格 的 文件.txt", "😀表情符号.txt", "日本語ファイル.txt", "Ελληνικά.txt",
           "报告(最终版)[2].txt", "名字.有.很多.点.txt", ".以点开头.txt", "没有扩展名")
foreach ($n in $names) { Write-Text (Join-Path $d "文件名测试\$n") "文件名：$n`r`n" }
Step "$($names.Count) 个特殊文件名（空格、emoji、日文、希腊文、括号、多个点、无扩展名）"

# 大量小文件
$rng = New-Object System.Random 2026
$perDir = 100
$dirs = [Math]::Ceiling($SmallFiles / $perDir)
$count = 0
for ($i = 1; $i -le $dirs; $i++) {
    $sub = Join-Path $d ("大量小文件\批次{0:D2}" -f $i)
    [CbkDemo]::EnsureDir($sub)
    for ($j = 1; $j -le $perDir -and $count -lt $SmallFiles; $j++) {
        $count++
        $lines = $rng.Next(0, 120)
        $text = ("第 {0} 号小文件，这一行会重复，便于压缩。`r`n" -f $count) * $lines
        [CbkDemo]::WriteText((Join-Path $sub ("小文件{0:D4}.txt" -f $count)), $text)
    }
}
Step "$count 个小文件，分在 $dirs 个子目录"

# 空目录、空文件、中等大小文件
[CbkDemo]::EnsureDir((Join-Path $d "空目录"))
[CbkDemo]::EnsureDir((Join-Path $d "嵌套空目录\a\b\c"))
Write-Text (Join-Path $d "空文件.txt") ""
[CbkDemo]::WriteRandomFile((Join-Path $d "中等文件-3MB.bin"), 3MB, 7)
Step "空目录、嵌套空目录、空文件、3 MB 文件"

# ------------------------------------------------------------------ 03 大文件

Section "03-大文件"
$d = Join-Path $src "03-大文件"
[CbkDemo]::EnsureDir($d)
if ($SkipBig) {
    Note "按 -SkipBig 跳过了大文件"
} else {
    $bytes = [long]($BigSizeGB * 1GB)
    $label = if ($BigSizeGB -eq [Math]::Floor($BigSizeGB)) { "{0}GB" -f $BigSizeGB } else { "{0}MB" -f [long]($BigSizeGB * 1024) }
    $big = Join-Path $d "大文件-$label.bin"
    Step "正在写 $label 随机数据……"
    $sw = [Diagnostics.Stopwatch]::StartNew()
    [CbkDemo]::WriteRandomFile($big, $bytes, 42)
    Step ("完成，用时 {0:N1} 秒" -f $sw.Elapsed.TotalSeconds)
    Write-Text (Join-Path $d "说明.txt") "大文件是随机数据，压缩不动；演示 GB 级备份时建议不选压缩。`r`n"
}

# ------------------------------------------------------------------ 04 特殊文件

Section "04-特殊文件"
$d = Join-Path $src "04-特殊文件"
Write-Text (Join-Path $d "目标\真文件.txt") "我是真文件，好几个链接都指向我。`r`n"
Write-Text (Join-Path $d "目标\真目录\目录里的文件.txt") "我在真目录里。`r`n"

# 硬链接：普通用户就能建
$orig = Join-Path $d "硬链接\原件.txt"
Write-Text $orig "两个名字，同一份数据。改其中一个，另一个也跟着变。`r`n"
[CbkDemo]::HardLink((Join-Path $d "硬链接\别名.txt"), $orig)
$a = Join-Path $d "硬链接\三个名字-A.txt"
Write-Text $a "三个名字共享这份数据，其中一个还在别的目录里。`r`n"
[CbkDemo]::HardLink((Join-Path $d "硬链接\三个名字-B.txt"), $a)
[CbkDemo]::EnsureDir((Join-Path $d "硬链接\另一个目录"))
[CbkDemo]::HardLink((Join-Path $d "硬链接\另一个目录\三个名字-C.txt"), $a)
Step "硬链接：一组 2 个名字、一组 3 个名字（跨目录）"

# junction：普通用户就能建
$jTarget = Join-Path $d "目标\真目录"
$output = & cmd.exe /c mklink /J "$(Join-Path $d '联接-junction')" "$jTarget" 2>&1
if ($LASTEXITCODE -ne 0) { throw "建 junction 失败：$output" }
Step "junction：联接-junction → 目标\真目录"

# 符号链接：需要管理员或开发者模式
$links = @(
    @{ Name = "链接-相对路径.txt"; Target = "目标\真文件.txt"; Dir = $false },
    @{ Name = "链接-绝对路径.txt"; Target = (Join-Path $d "目标\真文件.txt"); Dir = $false },
    @{ Name = "链接-目录"; Target = "目标\真目录"; Dir = $true },
    @{ Name = "链接-悬空.txt"; Target = "已经删掉的文件.txt"; Dir = $false }
)
$made = 0
foreach ($l in $links) {
    $err = [CbkDemo]::SymLink((Join-Path $d $l.Name), $l.Target, $l.Dir)
    if ($err -eq 0) { $made++ } else { $lastErr = $err }
}
if ($made -eq $links.Count) {
    Step "符号链接：相对文件、绝对文件、目录、悬空 各 1 个"
} else {
    Note "符号链接只建出 $made / $($links.Count) 个（错误码 $lastErr，需要管理员或开发者模式）"
}

Write-Text (Join-Path $d "说明-管道与设备文件.txt") (
    "为什么这里没有管道文件和设备文件？`r`n`r`n" +
    "Windows 的命名管道位于 \\.\pipe\ 命名空间，设备位于 \\.\ 命名空间，`r`n" +
    "它们不在任何磁盘的目录树里，遍历目录时根本遇不到。这是与 Linux 的平台差异。`r`n" +
    "本软件遇到其它不认识的重解析点（如 OneDrive 占位符）时，会记录类型并发出告警，不会递归进去。`r`n")

# ------------------------------------------------------------------ 05 元数据

Section "05-元数据"
$d = Join-Path $src "05-元数据"
$files = @{}
foreach ($n in @("时间-2001年.txt", "时间-2035年.txt", "属性-只读.txt", "属性-隐藏.txt",
                 "属性-系统和隐藏.txt", "属性-不索引.txt", "属性-无存档位.txt",
                 "权限-阻断继承.txt", "权限-额外授权给Guests.txt", "属主-SYSTEM.txt",
                 "只读目录\里面的文件.txt", "目录时间-1999年\里面的文件.txt",
                 "权限目录-阻断继承\继承来的权限.txt")) {
    $p = Join-Path $d $n
    Write-Text $p "演示文件：$n`r`n"
    $files[$n] = $p
}

# ① 权限（先于时间戳和属性：改权限不影响修改时间，但只读位设上之后就不好改了）
$sidSystem = "S-1-5-18"; $sidAdmins = "S-1-5-32-544"; $sidUsers = "S-1-5-32-545"
$sidGuests = "S-1-5-32-546"; $sidEveryone = "S-1-1-0"
Icacls $files["权限-阻断继承.txt"] /inheritance:r /grant:r "*${userSid}:(F)" "*${sidAdmins}:(F)" "*${sidSystem}:(F)" "*${sidEveryone}:(R)"
Icacls $files["权限-额外授权给Guests.txt"] /grant "*${sidGuests}:(RX)"
Icacls (Join-Path $d "权限目录-阻断继承") /inheritance:r /grant:r "*${userSid}:(OI)(CI)(F)" "*${sidAdmins}:(OI)(CI)(F)" "*${sidSystem}:(OI)(CI)(F)" "*${sidUsers}:(OI)(CI)(RX)"
Icacls $files["权限目录-阻断继承\继承来的权限.txt"] /reset
Step "权限：阻断继承 + 自定义 ACE、额外授权 Guests、阻断继承的目录及其子文件"
if ($isAdmin) {
    Icacls $files["属主-SYSTEM.txt"] /setowner "*$sidSystem"
    Step "属主：属主-SYSTEM.txt 的属主改成 SYSTEM"
} else {
    Note "非管理员不能把属主改成别人，属主-SYSTEM.txt 的属主仍是当前用户"
}

# ② 时间戳（精确到 100 纳秒，还原后应一个 tick 都不差）
[CbkDemo]::SetTimes($files["时间-2001年.txt"], (MakeTime 2001 2 3 4 5 6 1234567), (MakeTime 2002 3 4 5 6 7 2345678), (MakeTime 2003 4 5 6 7 8 3456789))
[CbkDemo]::SetTimes($files["时间-2035年.txt"], (MakeTime 2035 1 1 0 0 0 1), (MakeTime 2035 6 1 12 0 0 2), (MakeTime 2035 12 31 23 59 59 9999999))
$old = MakeTime 2010 10 10 10 10 10 5555555
foreach ($n in @("属性-只读.txt", "属性-隐藏.txt", "属性-系统和隐藏.txt", "属性-不索引.txt", "属性-无存档位.txt")) {
    [CbkDemo]::SetTimes($files[$n], $old, $old, $old)
}
Step "时间戳：2001 年、2035 年两组，均带 7 位小数秒"

# ③ 属性位（必须最后设，只读设上之后再改别的会失败）
[CbkDemo]::SetAttributes($files["属性-只读.txt"], 0x21)          # 只读 + 存档
[CbkDemo]::SetAttributes($files["属性-隐藏.txt"], 0x22)          # 隐藏 + 存档
[CbkDemo]::SetAttributes($files["属性-系统和隐藏.txt"], 0x26)    # 系统 + 隐藏 + 存档
[CbkDemo]::SetAttributes($files["属性-不索引.txt"], 0x2020)      # 不索引 + 存档
[CbkDemo]::SetAttributes($files["属性-无存档位.txt"], 0x80)      # NORMAL：清掉存档位
Step "属性：只读、隐藏、系统+隐藏、不索引、清掉存档位"

# ④ 目录的时间戳和属性放在最后（往目录里写东西会改它的修改时间）
$dir1999 = Join-Path $d "目录时间-1999年"
[CbkDemo]::SetTimes($dir1999, (MakeTime 1999 9 9 9 9 9 1111111), (MakeTime 1999 9 9 9 9 9 1111111), (MakeTime 1999 12 31 23 59 59 9999999))
[CbkDemo]::SetAttributes((Join-Path $d "只读目录"), 0x11)        # 目录 + 只读
Step "目录：一个 1999 年的目录时间戳、一个只读目录"

# ------------------------------------------------------------------ 汇总

Section "完成"
foreach ($dir in Get-ChildItem -LiteralPath $src -Directory) {
    $entries = [CbkDemo]::Scan($dir.FullName, $false)
    $bytes = ($entries | Where-Object { -not $_.IsDirectory -and -not $_.IsReparse } |
              Measure-Object -Property Size -Sum).Sum
    Step ("{0,-10} {1,6} 个条目  {2,10:N1} MB" -f $dir.Name, $entries.Count, ($bytes / 1MB))
}
Write-Host ""
Write-Host "   演示数据：$src"
Write-Host "   备份包放：$backup"
Write-Host "   还原到：  $restore\<同名目录>"
Write-Host "   对比命令：.\scripts\demo\compare_trees.ps1 04      （04 换成要对比的编号）"
if ($notes.Count -gt 0) {
    Write-Host ""
    Write-Host "   注意：" -ForegroundColor Yellow
    foreach ($n in $notes) { Write-Host "   - $n" -ForegroundColor Yellow }
}
