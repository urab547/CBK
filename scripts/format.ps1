# 按 .clang-format 格式化 core 和 cli 下的所有 C++ 源文件。
# 用法：在仓库根目录执行  .\scripts\format.ps1
#
# clang-format 随 Visual Studio 一起装了，一般在
#   C:\Program Files\Microsoft Visual Studio\2022\<版本>\VC\Tools\Llvm\bin\
# 如果提示找不到命令，把那个目录加进 PATH，或者 winget install LLVM.LLVM。

$ErrorActionPreference = "Stop"

if (-not (Get-Command clang-format -ErrorAction SilentlyContinue)) {
    Write-Error "找不到 clang-format。装 LLVM 或把 VS 自带的那个加进 PATH。"
}

$files = Get-ChildItem -Path "code/core", "code/cli", "code/tests" `
                       -Include *.h, *.cpp -Recurse -File

if ($files.Count -eq 0) {
    Write-Host "没有找到源文件。确认你在仓库根目录执行。"
    exit 0
}

foreach ($f in $files) {
    clang-format -i --style=file $f.FullName
    Write-Host "  已格式化  $($f.FullName.Replace($PWD.Path + '\', ''))"
}

Write-Host ""
Write-Host "共格式化 $($files.Count) 个文件。用 git diff 看看改了什么。"
