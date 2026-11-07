# 对比源目录和还原目录，逐项列出「是否一模一样」。
#
# 用法（仓库根目录）：
#   .\scripts\demo\compare_trees.ps1 04                  对比 D:\CBK-Demo\src\04-* 与 restore\04-*
#   .\scripts\demo\compare_trees.ps1 05 -ShowAll         条目多时也逐条列出
#   .\scripts\demo\compare_trees.ps1 -Source A -Restored B
#
# 逐项检查：
#   类型       文件 / 目录 / 文件符号链接 / 目录符号链接 / junction
#   链接目标   链接里存的路径字符串，以及是否是相对路径
#   内容       SHA-256 哈希
#   硬链接     还原后是否仍和同样的那几个名字共享同一份数据
#   时间戳     创建时间、修改时间，精确到 100 纳秒
#   属性       只读 / 隐藏 / 系统 / 存档 / 不索引 等可设置的位
#   属主       文件属于哪个账户
#   权限       属组、是否阻断继承、显式添加的每一条 ACE
#   访问时间   只作参考：Windows 读文件时可能会自动刷新它，不计入结果
#
# 读源文件内容算哈希本身也可能刷新访问时间，所以时间戳在算哈希之前先读好。

param(
    [Parameter(Position = 0)][string]$Name,
    [string]$Source,
    [string]$Restored,
    [string]$Root = "D:\CBK-Demo",
    [switch]$ShowAll
)

$ErrorActionPreference = "Stop"

if (-not ("CbkDemo" -as [type])) {
    Add-Type -Path (Join-Path $PSScriptRoot "CbkDemoNative.cs")
}

# ------------------------------------------------------------------ 定位两个目录

if ($Name) {
    $srcDir = Get-ChildItem -LiteralPath (Join-Path $Root "src") -Directory | Where-Object { $_.Name -like "$Name*" } | Select-Object -First 1
    if (-not $srcDir) { throw "在 $Root\src 下找不到以 $Name 开头的目录" }
    $Source = $srcDir.FullName
    $Restored = Join-Path (Join-Path $Root "restore") $srcDir.Name
}
if (-not $Source -or -not $Restored) { throw "请给出编号（如 04），或者同时给出 -Source 和 -Restored" }
if (-not (Test-Path -LiteralPath $Source)) { throw "源目录不存在：$Source" }
if (-not (Test-Path -LiteralPath $Restored)) { throw "还原目录不存在：$Restored（先在 GUI 里还原到这里）" }

Write-Host ""
Write-Host "源目录：  $Source" -ForegroundColor Cyan
Write-Host "还原目录：$Restored" -ForegroundColor Cyan
Write-Host "正在读取并计算哈希……"

$srcList = [CbkDemo]::Scan($Source, $true)
$dstList = [CbkDemo]::Scan($Restored, $true)

# ------------------------------------------------------------------ 工具函数

$settableMask = 0x3127   # 只读|隐藏|系统|存档|临时|脱机|不索引

# 标签用十进制：PowerShell 会把 0xA000000C 这种字面量当成负的 Int32，和 uint32 比不上。
# 2684354572 = 0xA000000C（符号链接），2684354563 = 0xA0000003（junction）。
function Kind($e) {
    if ($e.Error -and -not $e.Attributes) { return "读取失败" }
    if ($e.IsReparse) {
        if ($e.ReparseTag -eq 2684354572) { if ($e.IsDirectory) { return "目录符号链接" } else { return "文件符号链接" } }
        if ($e.ReparseTag -eq 2684354563) { return "junction" }
        return ("重解析点 0x{0:X8}" -f $e.ReparseTag)
    }
    if ($e.IsDirectory) { return "目录" }
    return "文件"
}

function TimeText([long]$ft) {
    if ($ft -le 0) { return "-" }
    return [DateTime]::FromFileTime($ft).ToString("yyyy-MM-dd HH:mm:ss.fffffff")
}

function AttrText([uint32]$a) {
    $s = ""
    if ($a -band 0x1) { $s += "只读 " }
    if ($a -band 0x2) { $s += "隐藏 " }
    if ($a -band 0x4) { $s += "系统 " }
    if ($a -band 0x20) { $s += "存档 " }
    if ($a -band 0x100) { $s += "临时 " }
    if ($a -band 0x1000) { $s += "脱机 " }
    if ($a -band 0x2000) { $s += "不索引 " }
    if ($s -eq "") { return "(无)" }
    return $s.Trim()
}

$nameCache = @{}
function SidName([string]$sid) {
    if (-not $sid) { return "-" }
    if (-not $nameCache.ContainsKey($sid)) {
        try {
            $nameCache[$sid] = (New-Object Security.Principal.SecurityIdentifier $sid).Translate([Security.Principal.NTAccount]).Value
        } catch {
            $nameCache[$sid] = $sid   # 解析不了（比如别的机器上的账户）就显示 SID 本身
        }
    }
    return $nameCache[$sid]
}

# 把 SDDL 拆成：属主、属组、是否阻断继承、显式 ACE 列表。
function ParseSddl([string]$sddl) {
    if (-not $sddl) { return $null }
    $rsd = New-Object Security.AccessControl.RawSecurityDescriptor $sddl
    $explicit = @()
    $inherited = 0
    if ($rsd.DiscretionaryAcl) {
        foreach ($ace in $rsd.DiscretionaryAcl) {
            if ($ace.IsInherited) { $inherited++; continue }
            $kind = if ($ace.AceQualifier -eq "AccessDenied") { "拒绝" } else { "允许" }
            $explicit += ("{0} {1} 0x{2:X}" -f $kind, (SidName $ace.SecurityIdentifier.Value), $ace.AccessMask)
        }
    }
    return [pscustomobject]@{
        Owner     = $rsd.Owner.Value
        Group     = if ($rsd.Group) { $rsd.Group.Value } else { $null }
        Protected = ([int]$rsd.ControlFlags -band [int][Security.AccessControl.ControlFlags]::DiscretionaryAclProtected) -ne 0
        Explicit  = (($explicit | Sort-Object) -join "；")
        Inherited = $inherited
    }
}

# 硬链接分组：同一棵树里，卷序列号 + 文件索引号相同的就是同一份数据。
# 返回 相对路径 → 同组其它成员（排好序、用 | 连起来）。
function HardlinkPeers($list) {
    $groups = @{}
    foreach ($e in $list) {
        if ($e.IsDirectory -or $e.IsReparse -or $e.LinkCount -le 1) { continue }
        $key = "{0}:{1}" -f $e.VolumeSerial, $e.FileIndex
        if (-not $groups.ContainsKey($key)) { $groups[$key] = New-Object System.Collections.Generic.List[string] }
        $groups[$key].Add($e.Rel)
    }
    $peers = @{}
    foreach ($members in $groups.Values) {
        foreach ($m in $members) {
            $peers[$m] = (($members | Where-Object { $_ -ne $m } | Sort-Object) -join " | ")
        }
    }
    return $peers
}

# ------------------------------------------------------------------ 逐条比较

$src = @{}; foreach ($e in $srcList) { $src[$e.Rel.ToLowerInvariant()] = $e }
$dst = @{}; foreach ($e in $dstList) { $dst[$e.Rel.ToLowerInvariant()] = $e }
$srcPeers = HardlinkPeers $srcList
$dstPeers = HardlinkPeers $dstList

# 给源树里的硬链接组编号 H1、H2……，表格里好认。
$groupLabel = @{}; $n = 0
foreach ($rel in ($srcPeers.Keys | Sort-Object)) {
    $members = @($rel) + ($srcPeers[$rel] -split " \| ") | Sort-Object
    $key = $members -join "|"
    if (-not $groupLabel.ContainsKey($key)) { $n++; $groupLabel[$key] = "H$n" }
    $groupLabel[$rel] = $groupLabel[$key]
}

$checkNames = @("类型", "链接目标", "内容", "硬链接", "创建时间", "修改时间", "属性", "属主", "权限")
$stats = @{}; foreach ($c in $checkNames) { $stats[$c] = @{ Pass = 0; Fail = 0 } }
$failures = New-Object System.Collections.Generic.List[object]
$rows = New-Object System.Collections.Generic.List[object]
$atimeDiff = 0

function Check($rel, $check, [bool]$ok, $a, $b) {
    if ($ok) { $stats[$check].Pass++ } else {
        $stats[$check].Fail++
        $failures.Add([pscustomobject]@{ 路径 = $rel; 检查项 = $check; 源 = "$a"; 还原 = "$b" })
    }
    return $ok
}

foreach ($key in ($src.Keys | Sort-Object)) {
    $s = $src[$key]
    if (-not $dst.ContainsKey($key)) {
        $failures.Add([pscustomobject]@{ 路径 = $s.Rel; 检查项 = "存在"; 源 = "有"; 还原 = "缺失" })
        $rows.Add([pscustomobject]@{ 结果 = "×"; 类型 = (Kind $s); 路径 = $s.Rel; 修改时间 = ""; 属性 = ""; 属主 = ""; 说明 = "还原目录里缺失" })
        continue
    }
    $d = $dst[$key]
    $ok = $true
    $ks = Kind $s; $kd = Kind $d
    $ok = (Check $s.Rel "类型" ($ks -eq $kd) $ks $kd) -and $ok

    $note = ""
    if ($s.IsReparse) {
        $ts = "$($s.LinkTarget)" + $(if ($s.LinkRelative) { "（相对）" } else { "" })
        $td = "$($d.LinkTarget)" + $(if ($d.LinkRelative) { "（相对）" } else { "" })
        $ok = (Check $s.Rel "链接目标" ($ts -eq $td) $ts $td) -and $ok
        $note = "→ $ts"
    } elseif (-not $s.IsDirectory) {
        $ok = (Check $s.Rel "内容" ($s.Hash -eq $d.Hash -and $s.Size -eq $d.Size) "$($s.Size) 字节 $($s.Hash.Substring(0,12))…" "$($d.Size) 字节 $(if ($d.Hash) { $d.Hash.Substring(0,12) })…") -and $ok
        $note = if ($s.Size -ge 1MB) { "{0:N1} MB" -f ($s.Size / 1MB) } else { "$($s.Size) 字节" }
        if ($srcPeers.ContainsKey($s.Rel)) {
            $ps = $srcPeers[$s.Rel]; $pd = if ($dstPeers.ContainsKey($d.Rel)) { $dstPeers[$d.Rel] } else { "（不是硬链接）" }
            $ok = (Check $s.Rel "硬链接" ($ps -eq $pd) $ps $pd) -and $ok
            $note = "硬链接组 $($groupLabel[$s.Rel])，与 $ps 共享数据"
        }
    }

    $ok = (Check $s.Rel "创建时间" ($s.CreationTime -eq $d.CreationTime) (TimeText $s.CreationTime) (TimeText $d.CreationTime)) -and $ok
    $ok = (Check $s.Rel "修改时间" ($s.LastWriteTime -eq $d.LastWriteTime) (TimeText $s.LastWriteTime) (TimeText $d.LastWriteTime)) -and $ok
    if ($s.LastAccessTime -ne $d.LastAccessTime) { $atimeDiff++ }

    $as = $s.Attributes -band $settableMask; $ad = $d.Attributes -band $settableMask
    $ok = (Check $s.Rel "属性" ($as -eq $ad) (AttrText $as) (AttrText $ad)) -and $ok

    $owner = "-"
    $ss = ParseSddl $s.Sddl; $sd = ParseSddl $d.Sddl
    if ($ss -or $sd) {
        $os = if ($ss) { SidName $ss.Owner } else { "读不到" }
        $od = if ($sd) { SidName $sd.Owner } else { "读不到" }
        $ok = (Check $s.Rel "属主" ($os -eq $od) $os $od) -and $ok
        $owner = $os
        $permS = if ($ss) { "属组 $(SidName $ss.Group)；$(if ($ss.Protected) { '阻断继承' } else { '继承' })；$($ss.Explicit)" } else { "读不到" }
        $permD = if ($sd) { "属组 $(SidName $sd.Group)；$(if ($sd.Protected) { '阻断继承' } else { '继承' })；$($sd.Explicit)" } else { "读不到" }
        $ok = (Check $s.Rel "权限" ($permS -eq $permD) $permS $permD) -and $ok
        if ($ss -and ($ss.Protected -or $ss.Explicit)) {
            $extra = if ($ss.Protected) { "阻断继承" } else { "显式授权" }
            $note = if ($note) { "$note；$extra" } else { $extra }
        }
    }

    $rows.Add([pscustomobject]@{
        结果 = $(if ($ok) { "√" } else { "×" }); 类型 = $ks; 路径 = $s.Rel
        修改时间 = (TimeText $s.LastWriteTime); 属性 = (AttrText $as); 属主 = $owner; 说明 = $note
    })
}

foreach ($key in ($dst.Keys | Sort-Object)) {
    if (-not $src.ContainsKey($key)) {
        $failures.Add([pscustomobject]@{ 路径 = $dst[$key].Rel; 检查项 = "存在"; 源 = "无"; 还原 = "多出来的" })
    }
}

# ------------------------------------------------------------------ 输出

$list = @($rows.ToArray())
if (-not $ShowAll -and $rows.Count -gt 60) {
    $list = @($rows.ToArray()) | Where-Object { $_.结果 -ne "√" } | Select-Object -First 60
    Write-Host ""
    Write-Host "条目较多（$($rows.Count) 个），只列出不一致的；加 -ShowAll 可逐条列出。" -ForegroundColor DarkGray
}
if ($list -and @($list).Count -gt 0) {
    $list | Format-Table -AutoSize -Wrap 结果, 类型, 路径, 修改时间, 属性, 属主, 说明 | Out-Host
}

Write-Host "逐项统计（源 $($srcList.Count) 个条目，还原 $($dstList.Count) 个条目）：" -ForegroundColor Cyan
foreach ($c in $checkNames) {
    $p = $stats[$c].Pass; $f = $stats[$c].Fail
    if ($p + $f -eq 0) { continue }
    $color = if ($f -eq 0) { "Green" } else { "Red" }
    Write-Host ("   {0} {1,-6} {2,5} / {3}" -f $(if ($f -eq 0) { "√" } else { "×" }), $c, $p, ($p + $f)) -ForegroundColor $color
}
Write-Host ("   · 访问时间   {0} 个不同（仅供参考：Windows 读文件时会自动刷新访问时间）" -f $atimeDiff) -ForegroundColor DarkGray

if ($failures.Count -gt 0) {
    Write-Host ""
    Write-Host "不一致的地方：" -ForegroundColor Red
    # 值可能很长（权限列表），表格会把"还原"一列挤出屏幕，所以每处分三行列出。
    foreach ($f in (@($failures.ToArray()) | Select-Object -First 40)) {
        Write-Host ("   × {0}  [{1}]" -f $f.路径, $f.检查项) -ForegroundColor Red
        Write-Host ("       源：  {0}" -f $f.源)
        Write-Host ("       还原：{0}" -f $f.还原)
    }
    if ($failures.Count -gt 40) { Write-Host "   ……共 $($failures.Count) 处，只显示前 40 处" }
    if ((@($failures.ToArray()) | Where-Object { $_.检查项 -eq "属主" }).Count -gt 0) {
        Write-Host "   提示：还原属主需要管理员权限（SeRestorePrivilege）。非管理员还原时属主会保持为当前用户，cbk 会发出告警。" -ForegroundColor Yellow
    }
    Write-Host ""
    Write-Host "结论：有 $($failures.Count) 处不一致" -ForegroundColor Red
    exit 1
}
Write-Host ""
Write-Host "结论：全部一致 √" -ForegroundColor Green
if ($stats["链接目标"].Pass -gt 0) {
    Write-Host "   说明：链接里存的是路径字符串。绝对路径链接和 junction 还原后仍指向原来的位置，这是正确的行为。" -ForegroundColor DarkGray
}
exit 0
