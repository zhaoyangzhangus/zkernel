[CmdletBinding()]
param(
    [string]$Compiler = 'x86_64-elf-gcc',
    [string]$Linker = 'x86_64-elf-ld',
    [switch]$FullGate
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'app-layout.ps1')

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$vendorRoot = Join-Path $repoRoot 'third_party\nvidia\595.84'
$vendorSourceRoot = Join-Path $vendorRoot 'open-gpu-kernel-modules'
$rmSourceRoot = Join-Path $repoRoot 'kernel\drivers\gpu\nvidia\rm'
$phase5Root = Join-Path $repoRoot 'build\nvidia\phase5'
$phase5Esp = Join-Path $phase5Root 'esp'
$phase5LinkList = Join-Path $phase5Root 'phase5_link_objects.list'
$phase6Root = Join-Path $repoRoot 'build\nvidia\phase6'
$phase6ObjectRoot = Join-Path $phase6Root 'objects'
$phase6LinkList = Join-Path $phase6Root 'phase6_link_objects.list'
$phase6Kernel = Join-Path $phase6Root 'kernel.elf'
$phase6Esp = Join-Path $phase6Root 'esp'
$linkerScript = Join-Path $PSScriptRoot 'phase4-kernel.ld'
$cpuOpsysHeader = Join-Path $vendorSourceRoot 'src\common\sdk\nvidia\inc\cpuopsys.h'

function Require-File([string]$Path, [string]$Description) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "missing $Description`: $Path"
    }
}

function Require-Directory([string]$Path, [string]$Description) {
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        throw "missing $Description`: $Path"
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
    finally {
        $ErrorActionPreference = $previous
    }
    foreach ($line in $output) { Write-Output $line }
    if ($exitCode -ne 0) {
        throw "native tool failed ($exitCode): $Tool"
    }
}

function Invoke-CompileIfChanged([string]$Source, [string]$Output,
                                 [string[]]$Arguments) {
    $signaturePath = $Output + '.cmd'
    $sourceHash = (Get-FileHash -LiteralPath $Source -Algorithm SHA256).Hash
    $signature = $Source + "`n" + $sourceHash + "`n" +
                 (($Arguments | ForEach-Object { $_.ToString() }) -join "`n")
    if ((Test-Path -LiteralPath $Output -PathType Leaf) -and
        (Test-Path -LiteralPath $signaturePath -PathType Leaf) -and
        [IO.File]::ReadAllText($signaturePath) -eq $signature) {
        Write-Output ('PHASE6_INCREMENTAL_SKIP={0}' -f $Source)
        return
    }
    New-Item -ItemType Directory -Path (Split-Path -Parent $Output) -Force |
        Out-Null
    Invoke-Native $Compiler (@($Arguments) + @('-c', $Source, '-o', $Output))
    [IO.File]::WriteAllText($signaturePath, $signature,
                             [Text.Encoding]::ASCII)
    Write-Output ('PHASE6_COMPILE={0}' -f $Source)
}

function Copy-FileIfChanged([string]$Source, [string]$Target) {
    $same = $false
    if (Test-Path -LiteralPath $Target -PathType Leaf) {
        $sourceHash = (Get-FileHash -LiteralPath $Source -Algorithm SHA256).Hash
        $targetHash = (Get-FileHash -LiteralPath $Target -Algorithm SHA256).Hash
        $same = $sourceHash -eq $targetHash
    }
    if ($same) {
        $script:stageSkipped++
        return
    }
    New-Item -ItemType Directory -Path (Split-Path -Parent $Target) -Force |
        Out-Null
    Copy-Item -LiteralPath $Source -Destination $Target -Force
    $script:stageCopied++
}

function Copy-DirectoryContents([string]$SourceRoot, [string]$TargetRoot) {
    Assert-AppGuestLayout $TargetRoot
    if (-not (Test-Path -LiteralPath $SourceRoot -PathType Container)) {
        throw "missing Phase 5 ESP: $SourceRoot"
    }
    foreach ($source in @(Get-ChildItem -LiteralPath $SourceRoot -Recurse -File)) {
        $relative = $source.FullName.Substring($SourceRoot.Length).TrimStart('\', '/')
        if (Test-LegacyGuestPath $relative) { continue }
        Copy-FileIfChanged $source.FullName (Join-Path $TargetRoot $relative)
    }
}

Require-File $phase5LinkList 'Phase 5 link list'
Require-Directory $phase5Esp 'Phase 5 ESP'
Require-File $linkerScript 'Phase 4 linker script'
Require-File $cpuOpsysHeader 'vendor cpuopsys header'
Require-File (Join-Path $rmSourceRoot 'nv_rm_vendor_bridge.c') 'NVIDIA vendor bridge'
Require-File (Join-Path $rmSourceRoot 'nv_rm_phase6.c') 'Phase 6 RM/CE probe'

$vendorRelative = 'third_party/nvidia/595.84/open-gpu-kernel-modules'
$vendorDiff = @(& git -C $repoRoot diff --name-only -- $vendorRelative 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0) { throw 'git vendor diff check failed' }
if (@($vendorDiff | Where-Object { $_.Trim() }).Count -ne 0) {
    throw "vendor source tree is modified: $($vendorDiff[0])"
}

$compilerCommand = Get-Command $Compiler -ErrorAction Stop
$linkerCommand = Get-Command $Linker -ErrorAction Stop
$compilerPath = $compilerCommand.Source
$linkerPath = $linkerCommand.Source

$rmIncludesRelative = @(
    'src\nvidia\kernel\inc',
    'src\nvidia\interface',
    'src\common\sdk\nvidia\inc',
    'src\common\sdk\nvidia\inc\hw',
    'src\nvidia\arch\nvalloc\common\inc',
    'src\nvidia\arch\nvalloc\common\inc\gsp',
    'src\nvidia\arch\nvalloc\common\inc\deprecated',
    'src\nvidia\arch\nvalloc\unix\include',
    'src\nvidia\inc',
    'src\nvidia\inc\os',
    'src\common\shared\inc',
    'src\common\inc',
    'src\common\uproc\os\libos-v2.0.0\include',
    'src\common\uproc\os\common\include',
    'src\common\inc\swref',
    'src\common\inc\swref\published',
    'src\nvidia\generated',
    'src\common\nvswitch\kernel\inc',
    'src\common\nvswitch\interface',
    'src\common\nvswitch\common\inc',
    'src\common\inc\displayport',
    'src\common\nvlink\interface',
    'src\common\nvlink\inband\interface',
    'src\nvidia\src\mm\uvm\interface',
    'src\nvidia\inc\libraries',
    'src\nvidia\src\libraries',
    'src\nvidia\inc\kernel',
    'src\nvidia\src\libraries\libspdm\3.5.0\include',
    'src\nvidia\src\libraries\libspdm\3.5.0\include\hal',
    'src\nvidia\src\libraries\libspdm\3.5.0\os_stub\include',
    'src\nvidia\src\libraries\libspdm\3.5.0\os_stub',
    'src\nvidia\src\libraries\libspdm\3.5.0\os_stub\cryptlib_null',
    'src\nvidia\src\libraries\libspdm\nvidia',
    'src\common\mbedtls\3.6.2\include',
    'src\common\mbedtls\3.6.2\nvidia'
)
$rmIncludeArgs = @($rmIncludesRelative | ForEach-Object {
    ('-I' + (Join-Path $vendorSourceRoot $_).Replace('\', '/'))
})

$bridgeIncludes = @(
    ('-I' + (Join-Path $repoRoot 'include').Replace('\', '/')),
    ('-I' + (Join-Path $rmSourceRoot 'include').Replace('\', '/'))
) + $rmIncludeArgs
$bridgeDefines = @(
    '-DNVRM', '-DNV_LINUX', '-DNV_UNIX', '-DNV_X86_64', '-DNV_ARCH_BITS=64',
    '-D_LANGUAGE_C', '-D__NO_CTYPE', '-DLOCK_VAL_ENABLED=0',
    '-DPORT_ATOMIC_64_BIT_SUPPORTED=1', '-DPORT_IS_KERNEL_BUILD=1',
    '-DPORT_IS_CHECKED_BUILD=0', '-DPORT_MODULE_atomic=1', '-DPORT_MODULE_core=1',
    '-DPORT_MODULE_cpu=1', '-DPORT_MODULE_crypto=1', '-DPORT_MODULE_debug=1',
    '-DPORT_MODULE_memory=1', '-DPORT_MODULE_safe=1', '-DPORT_MODULE_string=1',
    '-DPORT_MODULE_sync=1', '-DPORT_MODULE_thread=1', '-DPORT_MODULE_util=1',
    '-DPORT_MODULE_example=0', '-DPORT_MODULE_mmio=0', '-DPORT_MODULE_time=0',
    '-DRS_STANDALONE=0', '-DRS_STANDALONE_TEST=0', '-DRS_COMPATABILITY_MODE=1',
    '-DRS_PROVIDES_API_STATE=0', '-DNV_CONTAINERS_NO_TEMPLATES',
    '-DINCLUDE_NVLINK_LIB', '-DINCLUDE_NVSWITCH_LIB',
    '-DNV_PRINTF_STRINGS_ALLOWED=0', '-DNV_ASSERT_FAILED_USES_STRINGS=0',
    '-DPORT_ASSERT_FAILED_USES_STRINGS=0', '-DOS_KERNEL_BUILD',
    '-DOS_NVIDIA_PHASE5=1', '-DOS_NVIDIA_PHASE6=1'
)
if ($FullGate) {
    $bridgeDefines += '-DOS_NVIDIA_PHASE6_FULL_GATE=1'
}
$bridgeFlags = @('-include', $cpuOpsysHeader.Replace('\', '/')) +
               $bridgeIncludes + $bridgeDefines + @(
    '-fno-strict-aliasing', '-fno-omit-frame-pointer', '-Wformat=2',
    '-Wno-unused-parameter', '-O2', '-ffreestanding', '-fno-stack-protector',
    '-fno-common', '-fno-pic', '-ffunction-sections', '-fdata-sections',
    '-msoft-float', '-mno-red-zone', '-mno-mmx', '-mno-sse', '-mno-sse2',
    '-mno-3dnow', '-Werror-implicit-function-declaration', '-Wwrite-strings',
    '-Wundef', '--std=gnu11', '-mcmodel=kernel')

New-Item -ItemType Directory -Path $phase6Root -Force | Out-Null
New-Item -ItemType Directory -Path $phase6ObjectRoot -Force | Out-Null

$bridgeObject = Join-Path $phase6ObjectRoot 'nv_rm_vendor_bridge.o'
$phase6Object = Join-Path $phase6ObjectRoot 'nv_rm_phase6.o'
Invoke-CompileIfChanged (Join-Path $rmSourceRoot 'nv_rm_vendor_bridge.c') `
    $bridgeObject $bridgeFlags
Invoke-CompileIfChanged (Join-Path $rmSourceRoot 'nv_rm_phase6.c') `
    $phase6Object $bridgeFlags

$linkRows = New-Object 'System.Collections.Generic.List[string]'
foreach ($row in @(Get-Content -LiteralPath $phase5LinkList)) {
    if (-not $row -or $row[0] -eq '#') {
        [void]$linkRows.Add($row)
        continue
    }
    if ($row -match '(?i)[\\/]phase5[\\/]objects[\\/]nv_rm_vendor_bridge\.o$') {
        [void]$linkRows.Add($bridgeObject.Replace('\', '/'))
    } else {
        [void]$linkRows.Add($row)
    }
}
[void]$linkRows.Add($phase6Object.Replace('\', '/'))
[IO.File]::WriteAllLines($phase6LinkList, [string[]]$linkRows,
                          [Text.Encoding]::ASCII)

$linkArgs = @(
    '--gc-sections', '--allow-multiple-definition', '-T',
    $linkerScript.Replace('\', '/'), '-o', $phase6Kernel.Replace('\', '/'),
    ('@' + $phase6LinkList.Replace('\', '/')))
Invoke-Native $linkerPath $linkArgs

$nm = Get-Command x86_64-elf-nm -ErrorAction SilentlyContinue
if ($null -eq $nm) { $nm = Get-Command nm -ErrorAction Stop }
$undefined = @(& $nm.Source '-u' '-P' '--' $phase6Kernel 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0) { throw 'Phase 6 undefined-symbol scan failed' }
if (@($undefined | Where-Object { $_.Trim() }).Count -ne 0) {
    throw "Phase 6 kernel has unresolved symbols: $($undefined[0])"
}

$script:stageCopied = 0
$script:stageSkipped = 0
Copy-DirectoryContents $phase5Esp $phase6Esp
Copy-FileIfChanged $phase6Kernel (Join-Path $phase6Esp 'EFI\OS\kernel.elf')

Write-Output ('PHASE6_KERNEL={0}' -f $phase6Kernel)
Write-Output ('PHASE6_ESP={0}' -f $phase6Esp)
Write-Output ('PHASE6_LINK_OBJECTS={0}' -f $linkRows.Count)
Write-Output ('PHASE6_STAGE_COPIED={0}' -f $script:stageCopied)
Write-Output ('PHASE6_STAGE_SKIPPED={0}' -f $script:stageSkipped)
Write-Output 'PHASE6_BUILD=PASS'
