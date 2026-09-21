# 用 CMake 的归档工具独立检查 tar/cpio 数据区，不参与生产打包逻辑。
# 产物保留在被 gitignore 忽略的 code/build/<格式>-compat-<随机值>。
[CmdletBinding()]
param(
    [string]$Cbk = "$PSScriptRoot/../code/build/msvc/bin/Release/cbk.exe",
    [ValidateSet('tar', 'cpio')]
    [string]$Packer = 'tar'
)

$ErrorActionPreference = 'Stop'
$cbkPath = (Resolve-Path -LiteralPath $Cbk).Path
$cmakePath = (Get-Command cmake -ErrorAction Stop).Source
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$testRoot = Join-Path $workspace ("code/build/$Packer-compat-" + [guid]::NewGuid().ToString('N'))
$source = Join-Path $testRoot 'source'
$restored = Join-Path $testRoot 'restored'
$extracted = Join-Path $testRoot 'external'
foreach ($directory in @($source, $extracted, (Join-Path $source '子目录'))) {
    [IO.Directory]::CreateDirectory($directory) | Out-Null
}
$longName = ('长' * 60) + '.txt'
$utf8 = [Text.UTF8Encoding]::new($false)
[IO.File]::WriteAllText((Join-Path $source 'hello.txt'), 'hello tar', $utf8)
[IO.File]::WriteAllText((Join-Path $source "子目录/$longName"), '中文长路径内容', $utf8)
[IO.File]::WriteAllBytes((Join-Path $source 'empty.bin'), [byte[]]@())
$binary = [byte[]]::new(131075)
for ($i = 0; $i -lt $binary.Length; $i++) { $binary[$i] = [byte]($i % 256) }
[IO.File]::WriteAllBytes((Join-Path $source 'binary.bin'), $binary)
New-Item -ItemType HardLink -Path (Join-Path $source 'hardlink.txt') `
    -Target (Join-Path $source 'hello.txt') | Out-Null
New-Item -ItemType HardLink -Path (Join-Path $source 'third-link.txt') `
    -Target (Join-Path $source 'hello.txt') | Out-Null

$archive = Join-Path $testRoot 'sample.cbk'
& $cbkPath info
if ($LASTEXITCODE -ne 0) { throw 'cbk info 失败' }
& $cbkPath backup --source $source --dest $archive --packer $Packer --progress json
if ($LASTEXITCODE -ne 0) { throw "$Packer 备份失败" }
& $cbkPath verify --archive $archive
if ($LASTEXITCODE -ne 0) { throw '容器校验失败' }
& $cbkPath restore --archive $archive --dest $restored --no-metadata
if ($LASTEXITCODE -ne 0) { throw 'CBK 还原失败' }

# .cbk 有自己的外壳；未压缩/加密时数据区就是所选格式的字节流。
$tarFile = Join-Path $testRoot "data.$Packer"
$inputStream = [IO.File]::OpenRead($archive)
try {
    $reader = [IO.BinaryReader]::new($inputStream)
    $inputStream.Position = 32
    $offset = $reader.ReadUInt64()
    $remaining = $reader.ReadUInt64()
    $inputStream.Position = [long]$offset
    $outputStream = [IO.File]::Create($tarFile)
    try {
        $buffer = [byte[]]::new(65536)
        while ($remaining -gt 0) {
            $want = [int][Math]::Min([ulong]$buffer.Length, $remaining)
            $got = $inputStream.Read($buffer, 0, $want)
            if ($got -eq 0) { throw '容器数据区意外结束' }
            $outputStream.Write($buffer, 0, $got)
            $remaining -= [ulong]$got
        }
    } finally { $outputStream.Dispose() }
} finally { $inputStream.Dispose() }

& $cmakePath -E tar tf $tarFile
if ($LASTEXITCODE -ne 0) { throw 'CMake tar 列表检查失败' }
& $cmakePath -E chdir $extracted $cmakePath -E tar xf $tarFile
if ($LASTEXITCODE -ne 0) { throw 'CMake tar 解包失败' }
$files = @(Get-ChildItem -LiteralPath $source -Recurse -File)
foreach ($root in @($restored, $extracted)) {
    $actual = @(Get-ChildItem -LiteralPath $root -Recurse -File)
    if ($Packer -eq 'cpio' -and $root -eq $extracted) {
        $metadataRoot = Join-Path $root '.__cbk_metadata__'
        $metadataFiles = @(Get-ChildItem -LiteralPath $metadataRoot -File)
        $sourceEntries = @(Get-ChildItem -LiteralPath $source -Recurse)
        if ($metadataFiles.Count -ne $sourceEntries.Count) { throw 'CPIO 元数据记录数不符' }
        $actual = @($actual | Where-Object { $_.DirectoryName -ne $metadataRoot })
    }
    if ($actual.Count -ne $files.Count) { throw "文件数量不一致: $root" }
    foreach ($file in $files) {
        $relative = $file.FullName.Substring($source.Length + 1)
        $expectedHash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
        $actualHash = (Get-FileHash -LiteralPath (Join-Path $root $relative) -Algorithm SHA256).Hash
        if ($actualHash -ne $expectedHash) { throw "文件内容不一致: $relative" }
    }
    # 内容一致还不足以证明是硬链接；修改其中一个名字，另两个必须同步变化。
    $marker = 'hardlink identity verified'
    [IO.File]::WriteAllText((Join-Path $root 'hello.txt'), $marker, $utf8)
    foreach ($alias in @('hardlink.txt', 'third-link.txt')) {
        if ([IO.File]::ReadAllText((Join-Path $root $alias)) -ne $marker) {
            throw "硬链接关系丢失: $root/$alias"
        }
    }
}
Write-Host "通过：$Packer 的 $($files.Count) 个文件 SHA256 全部一致，三个名字的硬链接关系成立。"
Write-Host "检查产物：$testRoot"
