# 统计注释率。
# 用法：在仓库根目录执行  .\scripts\comment_ratio.ps1
#
# 评分表里"程序注释是否达到 10% / 20%"是两档分。
# 每周跑一次，别等最后一周才发现不够——那时候补注释既痛苦又假。

$ErrorActionPreference = "Stop"

$files = Get-ChildItem -Path "code/core", "code/cli", "code/tests" `
                       -Include *.h, *.cpp -Recurse -File

$total = 0
$comment = 0
$blank = 0
$inBlock = $false

foreach ($f in $files) {
    foreach ($line in (Get-Content $f.FullName)) {
        $t = $line.Trim()
        $total++
        if ($t -eq "") { $blank++; continue }

        if ($inBlock) {
            $comment++
            if ($t -match '\*/') { $inBlock = $false }
        } elseif ($t.StartsWith("//")) {
            $comment++
        } elseif ($t.StartsWith("/*")) {
            $comment++
            if (-not ($t -match '\*/')) { $inBlock = $true }
        }
    }
}

$code = $total - $blank
$ratio = if ($code -gt 0) { [math]::Round($comment * 100.0 / $code, 1) } else { 0 }

Write-Host ""
Write-Host "文件数    $($files.Count)"
Write-Host "总行数    $total  （其中空行 $blank）"
Write-Host "注释行    $comment"
Write-Host "注释率    $ratio%   （注释行 / 非空行）"
Write-Host ""

if ($ratio -ge 20)    { Write-Host "已达 20% 档。" }
elseif ($ratio -ge 10) { Write-Host "已达 10% 档，离 20% 还差 $([math]::Round(20 - $ratio, 1)) 个百分点。" }
else                   { Write-Host "还没到 10% 档，先把头文件的 Doxygen 注释补齐。" }
