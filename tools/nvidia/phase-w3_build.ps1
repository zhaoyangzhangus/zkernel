[CmdletBinding()]
param(
    [string]$Make = 'make.exe',
    [string]$Linker = 'x86_64-elf-ld',
    [string]$Nm = 'x86_64-elf-nm',
    [string]$Phase12Build = 'build/nvidia/phase12',
    [string]$Phase12RealTestLog = 'build-realtest/real-hardware/20260909-121533/realtest.log',
    [switch]$RealTest
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'object-paths.ps1')

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$phase9Root = Join-Path $repoRoot 'build\nvidia\phase9'
$phase9LinkList = Join-Path $phase9Root 'phase9_link_objects.list'
$w3Root = Join-Path $repoRoot 'build\nvidia\phase-w3'
$w3SystemRoot = Join-Path $w3Root 'system'
$w3KernelRoot = Join-Path $w3SystemRoot 'kernel'
$w3Kernel = Join-Path $w3Root 'kernel.elf'
$w3EspKernel = Join-Path $w3Root 'esp\EFI\OS\kernel.elf'
$w3Closure = Join-Path $w3Root 'kernel-closure.o'
$w3LinkList = Join-Path $w3Root 'phase-w3_link_objects.list'
$linkerScript = Join-Path $PSScriptRoot 'phase4-kernel.ld'
$nvkmsCore = Join-Path $repoRoot 'build\nvidia\modeset-target\vkms-core.o'
$nvkmsObjects = Join-Path $repoRoot 'build\nvidia\modeset-target\objects.list'
$nvkmsBuild = Join-Path $repoRoot 'tools\nvidia\nvkms_build.ps1'

function Require-File([string]$Path, [string]$Description) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Missing $Description`: $Path"
    }
}

function Invoke-Native([string]$Tool, [string[]]$Arguments) {
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = @(& $Tool @Arguments 2>&1 |
            ForEach-Object { $_.ToString() })
        $exitCode = $LASTEXITCODE
    }
    finally { $ErrorActionPreference = $previous }
    foreach ($line in $output) { Write-Output $line }
    if ($exitCode -ne 0) { throw "native tool failed ($exitCode): $Tool" }
}

function Write-AsciiLines([string]$Path, [System.Collections.IEnumerable]$Lines) {
    $contents = if ($null -eq $Lines) { [string[]]@() } else { [string[]]@($Lines) }
    [IO.File]::WriteAllLines($Path, $contents, [Text.Encoding]::ASCII)
}

function Copy-FileIfChanged([string]$Source, [string]$Destination) {
    Require-File $Source 'generated W3 kernel'
    Require-File $Destination 'W3 ESP kernel'
    $sourceHash = (Get-FileHash -LiteralPath $Source -Algorithm SHA256).Hash
    $destinationHash = (Get-FileHash -LiteralPath $Destination -Algorithm SHA256).Hash
    if ($sourceHash -eq $destinationHash) {
        Write-Output 'W3_ESP_KERNEL_SKIPPED=1'
        return
    }
    Copy-Item -LiteralPath $Source -Destination $Destination -Force
    $destinationHash = (Get-FileHash -LiteralPath $Destination -Algorithm SHA256).Hash
    if ($sourceHash -ne $destinationHash) {
        throw 'W3 ESP kernel synchronization hash mismatch'
    }
    Write-Output 'W3_ESP_KERNEL_COPIED=1'
}

Require-File $phase9LinkList 'Phase 9 base link list'
Require-File (Join-Path $phase9Root 'kernel.elf') 'Phase 9 base kernel image'
Require-File $linkerScript 'kernel linker script'
Require-File $nvkmsBuild 'official NVKMS target builder'

# W3 建立在已正式通过的通用缓冲区和 CPU 读回门禁之后。此处会执行前一门禁，避免此目标
# 悄然跳过阶段。
$w2Gate = Join-Path $repoRoot 'tools\nvidia\phase-w2_gate.ps1'
$w2Output = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $w2Gate `
    -Phase12Build $Phase12Build -Phase12RealTestLog $Phase12RealTestLog 2>&1 |
    ForEach-Object { $_.ToString() })
foreach ($line in $w2Output) { Write-Output $line }
if ($LASTEXITCODE -ne 0 -or
    @($w2Output | Where-Object { $_ -eq 'GATE_W2=PASS' }).Count -ne 1) {
    throw 'W2 Gate is not passing; W3 build is refused.'
}
Write-Output 'W3_PREVIOUS_GATE=PASS'

# W3 链接会使用目标本地的 NVKMS 汇总对象。此处重新运行带签名检查的构建器，确保 System
# KAPI/OS 适配器更改在最终 W3 链接前反映到 vkms-core.o 中。官方 595.84 源码树仍只作为
# 输入；所有生成对象均保留在 build/nvidia/modeset-target 下。
$nvkmsArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
               $nvkmsBuild)
$nvkmsOutput = @(& powershell.exe @nvkmsArgs 2>&1 |
    ForEach-Object { $_.ToString() })
foreach ($line in $nvkmsOutput) { Write-Output $line }
if ($LASTEXITCODE -ne 0 -or
    @($nvkmsOutput | Where-Object {
        $_ -eq 'NVKMS_TARGET_BUILD=PASS'
    }).Count -ne 1) {
    throw 'NVKMS target build is not passing; W3 link is refused.'
}
Write-Output 'W3_NVKMS_BUILD=PASS'
Require-File $nvkmsCore 'linked official NVKMS core'
Require-File $nvkmsObjects 'official NVKMS object list'

$makePath = (Get-Command $Make -ErrorAction Stop).Source
$linkerPath = (Get-Command $Linker -ErrorAction Stop).Source
$nmPath = (Get-Command $Nm -ErrorAction Stop).Source
New-Item -ItemType Directory -Force -Path $w3KernelRoot | Out-Null

# 复用 Phase 9 的 System 侧源文件清单，但使用 W3 开关将其编译到此隔离根目录中。
# 这样依赖闭包中仍包含当前 W2 通用分配实现，同时不触碰调度器/MM 或普通启动目标。
$normalTargets = New-Object 'System.Collections.Generic.List[string]'
$normalLeaves = New-Object 'System.Collections.Generic.HashSet[string]'
foreach ($row in @(Get-Content -LiteralPath $phase9LinkList)) {
    $row = ConvertTo-ObjectListPath $row
    if (-not (Test-ObjectListData $row)) { continue }
    if ($row -match '(?i)/build/nvidia/phase9/system/kernel/([^/]+\.o)$') {
        $leaf = $Matches[1]
        if ($normalLeaves.Add($leaf)) {
            [void]$normalTargets.Add(
                ('build/nvidia/phase-w3/system/kernel/{0}' -f $leaf))
        }
    }
}

$w3ExtraFlags = '-DOS_NVIDIA_PHASE5=1 -DOS_NVIDIA_PHASE6=1 ' +
                '-DOS_NVKMS_W3=1 -mcmodel=kernel'
$realTestValue = if ($RealTest) { '1' } else { '0' }
$makeArgs = @(
    '-f', 'GNUmakefile',
    'BUILD=build/nvidia/phase-w3/system',
    'TOOLPREFIX=x86_64-elf-',
    'ELFTOOLPREFIX=x86_64-elf-',
    'HOSTCC=gcc',
    'DEBUG=2',
    'OS_DEBUG_SERIAL=1',
    ('OS_REALTEST={0}' -f $realTestValue),
    'OS_PHASE7=0',
    'OS_PHASE8=0',
    'OS_PHASE9=0',
    ('KERNEL_EXTRA_CFLAGS={0}' -f $w3ExtraFlags)
) + @($normalTargets)
Invoke-Native $makePath $makeArgs

$linkRows = New-Object 'System.Collections.Generic.List[string]'
foreach ($row in @(Get-Content -LiteralPath $phase9LinkList)) {
    $row = ConvertTo-ObjectListPath $row
    if (-not (Test-ObjectListData $row)) {
        [void]$linkRows.Add($row)
        continue
    }
    if ($row -match '(?i)/build/nvidia/phase9/system/kernel/([^/]+\.o)$') {
        [void]$linkRows.Add(
            (Join-Path $w3KernelRoot $Matches[1]).Replace('\', '/'))
        continue
    }
    # vkms-core.o 已包含经过验证的桥接对象。W3 链接中只保留一份定义，不依赖两个独立链接副本
    # 之间对重复定义的选择。
    if ($row -match '(?i)[\\/]build/nvidia/phase8/objects/nv_rm_vendor_bridge\.o$') {
        continue
    }
    [void]$linkRows.Add($row)
}
[void]$linkRows.Add($nvkmsCore.Replace('\', '/'))

foreach ($row in @($linkRows)) {
    if ($row -and $row[0] -ne '#') {
        Require-File $row 'W3 link input'
    }
}
Write-AsciiLines $w3LinkList @($linkRows)

# 首先将每个普通对象和完整的官方 NVKMS 对象链接到一个可重定位依赖闭包中。此扫描有意在
# 最终 --gc-sections 镜像链接之前执行，避免未引用的供应商对象掩盖缺失的 OS/RM 边界符号。
$closureArguments = @(
    '-r', '--allow-multiple-definition', '-z', 'noexecstack', '-o',
    $w3Closure.Replace('\', '/'),
    ('@' + $w3LinkList.Replace('\', '/')))
Invoke-Native $linkerPath $closureArguments

$undefined = @(& $nmPath '-u' '-P' '--' $w3Closure 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0) { throw 'W3 closure undefined-symbol scan failed' }
$undefinedNames = @($undefined | ForEach-Object {
    $parts = $_ -split '\s+'
    if ($parts.Count -gt 0 -and $parts[0]) { $parts[0] }
} | Sort-Object -Unique)
$undefinedReport = Join-Path $w3Root 'kernel-closure-undefined.list'
if ($undefinedNames.Count -eq 0) {
    Write-AsciiLines $undefinedReport @('# none')
} else {
    Write-AsciiLines $undefinedReport $undefinedNames
    Write-Output ('W3_CLOSURE_UNDEFINED={0}' -f $undefinedNames.Count)
    $undefinedNames | Select-Object -First 40 | ForEach-Object {
        Write-Output ('W3_UNDEFINED={0}' -f $_)
    }
    throw 'W3 complete object closure has unresolved symbols'
}
Write-Output 'W3_CLOSURE_UNDEFINED=0'

$kernelArguments = @(
    '--gc-sections', '--allow-multiple-definition', '-T',
    $linkerScript.Replace('\', '/'), '-o', $w3Kernel.Replace('\', '/'),
    ('@' + $w3LinkList.Replace('\', '/')))
Invoke-Native $linkerPath $kernelArguments

$finalUndefined = @(& $nmPath '-u' '-P' '--' $w3Kernel 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0) { throw 'W3 kernel undefined-symbol scan failed' }
$finalUnexpected = @($finalUndefined | Where-Object { $_.Trim() })
if ($finalUnexpected.Count -ne 0) {
    throw ('W3 kernel has unresolved symbols: {0}' -f $finalUnexpected[0])
}

# 真实硬件运行器会直接暂存此 ESP 目录。使其生成的内核与刚链接的镜像保持同步，避免增量部署
# 意外启动上一次的 W3 内核。
Copy-FileIfChanged $w3Kernel $w3EspKernel

Write-Output ('W3_LINK_OBJECTS={0}' -f $linkRows.Count)
Write-Output ('W3_KERNEL={0}' -f $w3Kernel)
Write-Output 'W3_STATIC_LINK=PASS'
Write-Output 'W3_BUILD=PASS'
