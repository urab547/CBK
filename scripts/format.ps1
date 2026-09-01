# 按 .clang-format 格式化 core / cli / tests 下的所有 C++ 源文件。
#
# 用法（在仓库根目录执行）：
#   .\scripts\format.ps1           就地格式化
#   .\scripts\format.ps1 -Check    只检查不改动，有文件不合规就以退出码 1 结束
#
# 找 clang-format 的顺序：先看 PATH，再问 vswhere 要 Visual Studio 自带的那份。
# 之所以要第二步：VS 生成工具确实自带 clang-format，但装在
#   <VS安装路径>\VC\Tools\Llvm\bin\
# 而且**不会加进 PATH**。只查 PATH 的话，全组三个人照着 README 跑这个脚本
# 都会在第一步被拦下，然后干脆跳过格式化——这个坑本项目已经踩过一次。

[CmdletBinding()]
param(
    # 只检查不改动。适合提交前自查，也适合将来挂进 CI。
    [switch]$Check
)

$ErrorActionPreference = "Stop"

function Find-ClangFormat {
    # 1. PATH 上有就直接用（自己装了 LLVM 的情况）
    $onPath = Get-Command clang-format -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }

    # 2. 问 vswhere 要 VS 的安装路径。
    #    注意必须带 -products *：默认只返回 IDE 版本，
    #    「Visual Studio 生成工具」这类产品会被漏掉。
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $roots = & $vswhere -products * -all -format value -property installationPath
        foreach ($root in $roots) {
            if ([string]::IsNullOrWhiteSpace($root)) { continue }
            $candidate = Join-Path $root "VC\Tools\Llvm\bin\clang-format.exe"
            if (Test-Path $candidate) { return $candidate }
        }
    }

    return $null
}

$clangFormat = Find-ClangFormat
if (-not $clangFormat) {
    Write-Error @"
找不到 clang-format。三个办法任选其一：
  1. 用 Visual Studio 安装器勾上「适用于 Windows 的 C++ Clang 工具」组件（推荐，不用额外下东西）
  2. 单独装 LLVM：winget install LLVM.LLVM
  3. 手动把 <VS安装路径>\VC\Tools\Llvm\bin 加进 PATH
"@
}

Write-Host "clang-format: $clangFormat"
Write-Host ""

$files = Get-ChildItem -Path "code/core", "code/cli", "code/tests" `
                       -Include *.h, *.cpp -Recurse -File

if ($files.Count -eq 0) {
    Write-Host "没有找到源文件。确认你在仓库根目录执行。"
    exit 0
}

# clang-format 把诊断打到 stderr。Windows PowerShell 5.1 有个坑：
# 对原生命令做 stderr 重定向时，它会把每一行包成 ErrorRecord，
# 在 $ErrorActionPreference = "Stop" 下直接当成异常抛出来，脚本就断了。
# 所以这里临时把偏好降成 Continue，只靠退出码判断结果。
$previousPreference = $ErrorActionPreference
$ErrorActionPreference = "Continue"

try {
    if ($Check) {
        $bad = @()
        foreach ($f in $files) {
            # --dry-run 不改文件；配上 -Werror，有违规就返回非 0。
            & $clangFormat --style=file --dry-run -Werror $f.FullName 2>$null
            if ($LASTEXITCODE -ne 0) { $bad += $f }
        }

        if ($bad.Count -eq 0) {
            Write-Host "检查通过：$($files.Count) 个文件都符合 .clang-format。"
            exit 0
        }

        Write-Host "以下 $($bad.Count) 个文件不符合 .clang-format：" -ForegroundColor Yellow
        foreach ($f in $bad) {
            Write-Host "  $($f.FullName.Replace($PWD.Path + '\', ''))"
        }
        Write-Host ""
        Write-Host "跑一次 .\scripts\format.ps1（不带 -Check）就能修好。"
        exit 1
    }

    foreach ($f in $files) {
        & $clangFormat -i --style=file $f.FullName
        if ($LASTEXITCODE -ne 0) {
            Write-Host "  失败      $($f.FullName.Replace($PWD.Path + '\', ''))" -ForegroundColor Red
        } else {
            Write-Host "  已格式化  $($f.FullName.Replace($PWD.Path + '\', ''))"
        }
    }
} finally {
    $ErrorActionPreference = $previousPreference
}

Write-Host ""
Write-Host "共处理 $($files.Count) 个文件。用 git diff 看看改了什么。"
