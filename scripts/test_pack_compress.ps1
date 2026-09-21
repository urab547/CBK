# Windows CLI 端到端矩阵：tar/cpio × 无压缩/Huffman/LZ77。
# 日志、容器和还原目录保留在 gitignore 忽略的 code/build 下。
[CmdletBinding()]
param(
    [string]$Cbk = "$PSScriptRoot/../code/build/msvc/bin/Release/cbk.exe",
    [ValidateSet('none', 'xor', 'vigenere', 'xor-cbc')]
    [string]$Cipher = 'none'
)

$ErrorActionPreference = 'Stop'
$cbkPath = (Resolve-Path -LiteralPath $Cbk).Path
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$testRoot = Join-Path $workspace ('code/build/pack-compress-' + [guid]::NewGuid().ToString('N'))
$source = Join-Path $testRoot 'source'
$utf8 = [Text.UTF8Encoding]::new($false)
foreach ($name in @('', '中文目录', 'empty-directory', 'small')) {
    [IO.Directory]::CreateDirectory((Join-Path $source $name)) | Out-Null
}
$random = [Random]::new(142575)
foreach ($size in @(0, 1, 2, 3, 511, 512, 513, 65535, 65536, 65537, 4194307)) {
    $bytes = [byte[]]::new($size)
    $random.NextBytes($bytes)
    [IO.File]::WriteAllBytes((Join-Path $source "random-$size.bin"), $bytes)
}
[IO.File]::WriteAllBytes((Join-Path $source 'zeros.bin'), [byte[]]::new(4194305))
[IO.File]::WriteAllText((Join-Path $source 'text.txt'), ('repeated 中文 text ' * 100000), $utf8)
$longName = ('长' * 60) + '.txt'
[IO.File]::WriteAllText((Join-Path $source "中文目录/$longName"), '中文内容', $utf8)
foreach ($index in 1..200) {
    [IO.File]::WriteAllText((Join-Path $source "small/$index.txt"), "small file $index", $utf8)
}
$linkTarget = Join-Path $source 'link-a.txt'
[IO.File]::WriteAllText($linkTarget, 'hardlink content', $utf8)
foreach ($name in @('link-b.txt', 'link-c.txt')) {
    New-Item -ItemType HardLink -Path (Join-Path $source $name) -Target $linkTarget | Out-Null
}
$readOnly = Join-Path $source 'readonly.txt'
[IO.File]::WriteAllText($readOnly, 'readonly content', $utf8)
[IO.File]::SetAttributes($readOnly, [IO.FileAttributes]::ReadOnly)
$stamp = [DateTime]::new(2024, 2, 3, 4, 5, 6, [DateTimeKind]::Utc)
[IO.File]::SetLastWriteTimeUtc($readOnly, $stamp)
$files = @(Get-ChildItem -LiteralPath $source -Recurse -File)
$directories = @(Get-ChildItem -LiteralPath $source -Recurse -Directory)
$hashes = @{}
foreach ($file in $files) {
    $relative = $file.FullName.Substring($source.Length + 1)
    $hashes[$relative] = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
}

function Invoke-CbkChecked {
    param([string[]]$Arguments, [string]$Log)
    if ($Cipher -ne 'none' -and $Arguments[0] -in @('backup', 'list', 'restore')) {
        if ($Arguments[0] -eq 'backup') { $Arguments += @('--encrypt', $Cipher) }
        $Arguments += '--password-stdin'
        'cbk-integration-test-password' | & $cbkPath @Arguments | Out-File -LiteralPath $Log -Encoding utf8
    } else {
        & $cbkPath @Arguments | Out-File -LiteralPath $Log -Encoding utf8
    }
    if ($LASTEXITCODE -ne 0) { throw "cbk 失败，退出码 $LASTEXITCODE，日志 $Log" }
}

$results = @()
foreach ($packer in @('tar', 'cpio')) {
    foreach ($compressor in @('none', 'huffman', 'lz77')) {
        $label = "$packer-$compressor"
        $archive = Join-Path $testRoot "$label.cbk"
        $restored = Join-Path $testRoot "$label-restored"
        $arguments = @('backup', '--source', $source, '--dest', $archive, '--packer', $packer,
                       '--progress', 'json')
        if ($compressor -ne 'none') { $arguments += @('--compress', $compressor) }
        Invoke-CbkChecked $arguments (Join-Path $testRoot "$label-backup.log")
        $listingPath = Join-Path $testRoot "$label-list.log"
        Invoke-CbkChecked @('list', '--archive', $archive, '--json') $listingPath
        $listing = Get-Content -LiteralPath $listingPath -Raw | ConvertFrom-Json
        $expectedCount = $files.Count + $directories.Count
        $expectedStages = if ($compressor -eq 'none') { '' } else { $compressor }
        if ($Cipher -ne 'none') {
            $expectedStages = (@($compressor, $Cipher) | Where-Object { $_ -ne 'none' }) -join ','
        }
        if ($listing.packer -ne $packer -or ($listing.stages -join ',') -ne $expectedStages -or
            $listing.entryCount -ne $expectedCount -or $listing.entries.Count -ne $expectedCount) {
            throw "$label 列表算法或条目数量不符"
        }
        $listedPaths = @($listing.entries | ForEach-Object { $_.path.Replace('/', '\') })
        foreach ($entry in @($files) + @($directories)) {
            $relative = $entry.FullName.Substring($source.Length + 1)
            if ($relative -cnotin $listedPaths) { throw "$label 列表缺少路径：$relative" }
        }
        Invoke-CbkChecked @('verify', '--archive', $archive) (Join-Path $testRoot "$label-verify.log")
        Invoke-CbkChecked @('restore', '--archive', $archive, '--dest', $restored, '--progress', 'json') `
            (Join-Path $testRoot "$label-restore.log")
        $actualFiles = @(Get-ChildItem -LiteralPath $restored -Recurse -File)
        $actualDirectories = @(Get-ChildItem -LiteralPath $restored -Recurse -Directory)
        if ($actualFiles.Count -ne $files.Count -or $actualDirectories.Count -ne $directories.Count) {
            throw "$label 条目数量不一致"
        }
        foreach ($relative in $hashes.Keys) {
            $actual = (Get-FileHash -LiteralPath (Join-Path $restored $relative) -Algorithm SHA256).Hash
            if ($actual -ne $hashes[$relative]) { throw "$label 内容不一致：$relative" }
        }
        foreach ($directory in $directories) {
            $relative = $directory.FullName.Substring($source.Length + 1)
            if (-not [IO.Directory]::Exists((Join-Path $restored $relative))) {
                throw "$label 缺少目录：$relative"
            }
        }
        $restoredReadOnly = Join-Path $restored 'readonly.txt'
        if (-not ([IO.File]::GetAttributes($restoredReadOnly) -band [IO.FileAttributes]::ReadOnly)) {
            throw "$label 只读属性丢失"
        }
        if ([IO.File]::GetLastWriteTimeUtc($restoredReadOnly) -ne $stamp) { throw "$label 修改时间丢失" }
        [IO.File]::WriteAllText((Join-Path $restored 'link-a.txt'), 'modified alias', $utf8)
        foreach ($name in @('link-b.txt', 'link-c.txt')) {
            if ([IO.File]::ReadAllText((Join-Path $restored $name)) -ne 'modified alias') {
                throw "$label 硬链接关系丢失"
            }
        }
        # 破坏容器副本的数据区，verify 必须明确失败。仅修改本次新建的测试副本。
        $corrupt = Join-Path $testRoot "$label-corrupt.cbk"
        [IO.File]::Copy($archive, $corrupt)
        $stream = [IO.File]::Open($corrupt, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite)
        try {
            $reader = [IO.BinaryReader]::new($stream)
            $stream.Position = 32
            $offset = $reader.ReadUInt64()
            $stream.Position = [long]$offset + 30
            $value = $stream.ReadByte()
            $stream.Position -= 1
            $stream.WriteByte([byte]($value -bxor 1))
        } finally { $stream.Dispose() }
        & $cbkPath verify --archive $corrupt | Out-File (Join-Path $testRoot "$label-corrupt.log")
        if ($LASTEXITCODE -ne 2) { throw "$label 损坏容器未返回失败退出码 2" }
        $results += [pscustomobject]@{ combination = $label; cipher = $Cipher; files = $files.Count;
            directories = $directories.Count; archiveBytes = (Get-Item -LiteralPath $archive).Length }
        Write-Host "通过：$label，$($files.Count) 个文件 SHA256、目录、只读/时间、硬链接、损坏检测。"
        # 空源目录也必须经过完整流水线，验证各算法的结束标记。
        $emptySource = Join-Path $testRoot "$label-empty-source"
        $emptyArchive = Join-Path $testRoot "$label-empty.cbk"
        $emptyRestored = Join-Path $testRoot "$label-empty-restored"
        [IO.Directory]::CreateDirectory($emptySource) | Out-Null
        $emptyArguments = @('backup', '--source', $emptySource, '--dest', $emptyArchive,
                            '--packer', $packer)
        if ($compressor -ne 'none') { $emptyArguments += @('--compress', $compressor) }
        Invoke-CbkChecked $emptyArguments (Join-Path $testRoot "$label-empty-backup.log")
        Invoke-CbkChecked @('list', '--archive', $emptyArchive, '--json') `
            (Join-Path $testRoot "$label-empty-list.log")
        $emptyListing = Get-Content (Join-Path $testRoot "$label-empty-list.log") -Raw | ConvertFrom-Json
        if ($emptyListing.entryCount -ne 0) { throw "$label 空目录列表不为空" }
        Invoke-CbkChecked @('verify', '--archive', $emptyArchive) `
            (Join-Path $testRoot "$label-empty-verify.log")
        Invoke-CbkChecked @('restore', '--archive', $emptyArchive, '--dest', $emptyRestored) `
            (Join-Path $testRoot "$label-empty-restore.log")
        if (@(Get-ChildItem -LiteralPath $emptyRestored -Recurse -Force).Count -ne 0) {
            throw "$label 空目录还原不为空"
        }
        Write-Host "通过：$label 空源目录备份、列表、校验、还原。"
    }
}
$results | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $testRoot 'results.json') -Encoding utf8
Write-Host "检查产物：$testRoot"
