[CmdletBinding()]
param(
    [string]$Compiler = 'x86_64-elf-gcc',
    [string]$Linker = 'x86_64-elf-ld'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'app-layout.ps1')

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$vendorRoot = Join-Path $repoRoot 'third_party\nvidia\595.84'
$vendorSourceRoot = Join-Path $vendorRoot 'open-gpu-kernel-modules'
$abiSourceRoot = Join-Path $repoRoot 'kernel\drivers\gpu\nvidia\vendor_abi'
$rmSourceRoot = Join-Path $repoRoot 'kernel\drivers\gpu\nvidia\rm'
$portSourceRoot = Join-Path $repoRoot 'kernel\drivers\gpu\nvidia\port'
$mmSourceRoot = Join-Path $repoRoot 'kernel\mm'
$phase3Root = Join-Path $repoRoot 'build\nvidia\phase3'
$phase4Root = Join-Path $repoRoot 'build\nvidia\phase4'
$phase5Root = Join-Path $repoRoot 'build\nvidia\phase5'
$phase5ObjectRoot = Join-Path $phase5Root 'objects'
$phase5SystemRoot = Join-Path $phase5ObjectRoot 'system'
$phase5AbiRoot = Join-Path $phase5ObjectRoot 'vendor_abi'
$phase5LinkList = Join-Path $phase5Root 'phase5_link_objects.list'
$phase5Kernel = Join-Path $phase5Root 'kernel.elf'
$phase5Esp = Join-Path $phase5Root 'esp'
$phase4LinkList = Join-Path $phase4Root 'phase4_link_objects.list'
$phase4Kernel = Join-Path $phase4Root 'kernel.elf'
$linkerScript = Join-Path $PSScriptRoot 'phase4-kernel.ld'
$firmwareRoot = Join-Path $repoRoot 'build\nvidia\official-595.84\extracted-gitbash\firmware'
$firmwareNames = @('gsp_ga10x.bin', 'gsp_tu10x.bin')

function Require-File([string]$Path, [string]$Description) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
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
        Write-Output ('PHASE5_INCREMENTAL_SKIP={0}' -f $Source)
        return
    }
    New-Item -ItemType Directory -Path (Split-Path -Parent $Output) -Force |
        Out-Null
    Invoke-Native $Compiler (@($Arguments) + @('-c', $Source, '-o', $Output))
    [IO.File]::WriteAllText($signaturePath, $signature,
                             [Text.Encoding]::ASCII)
    Write-Output ('PHASE5_COMPILE={0}' -f $Source)
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
        throw "missing Phase 4 ESP: $SourceRoot"
    }
    foreach ($source in @(Get-ChildItem -LiteralPath $SourceRoot -Recurse -File)) {
        $relative = $source.FullName.Substring($SourceRoot.Length).TrimStart('\', '/')
        if (Test-LegacyGuestPath $relative) { continue }
        Copy-FileIfChanged $source.FullName (Join-Path $TargetRoot $relative)
    }
}

Require-File $phase4Kernel 'Phase 4 kernel image'
Require-File $phase4LinkList 'Phase 4 link list'
Require-File $linkerScript 'Phase 4 linker script'
foreach ($firmwareName in $firmwareNames) {
    Require-File (Join-Path $firmwareRoot $firmwareName) "matching 595.84 firmware $firmwareName"
}

$vendorRelative = 'third_party/nvidia/595.84/open-gpu-kernel-modules'
$vendorDiff = @(& git -C $repoRoot diff --name-only -- $vendorRelative 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0) { throw 'git vendor diff check failed' }
if (@($vendorDiff | Where-Object { $_.Trim() }).Count -ne 0) {
    throw "vendor source tree is modified: $($vendorDiff[0])"
}

# Phase 5 is downstream of the already passed Phase 4 hardware gate.  Reuse
# its recorded real-machine log instead of invoking the full Phase 4/P3
# build chain again.
$priorLogsRoot = Join-Path $repoRoot 'build-realtest\real-hardware'
$priorLogs = @()
if (Test-Path -LiteralPath $priorLogsRoot -PathType Container) {
    $priorLogs = @(Get-ChildItem -LiteralPath $priorLogsRoot -Recurse -File -Filter 'realtest.log' |
        Sort-Object LastWriteTime -Descending)
}
$priorGateLog = $null
foreach ($candidate in $priorLogs) {
    $candidateText = Get-Content -LiteralPath $candidate.FullName -Raw
    if ($candidateText -match 'OS_NVIDIA_PHASE4_GATE_READY' -and
        $candidateText -match 'OS_REALTEST_PASS') {
        $priorGateLog = $candidate.FullName
        break
    }
}
if ($null -eq $priorGateLog) {
    throw 'no recorded Phase 4 real-hardware Gate evidence is available'
}
Write-Output ('P4_EVIDENCE={0}' -f $priorGateLog)

$compilerCommand = Get-Command $Compiler -ErrorAction Stop
$linkerCommand = Get-Command $Linker -ErrorAction Stop
$compilerPath = $compilerCommand.Source
$linkerPath = $linkerCommand.Source
$repoSlash = $repoRoot.Replace('\', '/')
$vendorSlash = $vendorSourceRoot.Replace('\', '/')
$abiSlash = $abiSourceRoot.Replace('\', '/')
$rmSlash = $rmSourceRoot.Replace('\', '/')
$cpuOpsysHeader = Join-Path $vendorSourceRoot 'src\common\sdk\nvidia\inc\cpuopsys.h'
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
$abiIncludeArgs = @(
    ('-I' + (Join-Path $repoRoot 'include').Replace('\', '/')),
    ('-I' + (Join-Path $repoRoot 'kernel\drivers\gpu\nvidia\port\include').Replace('\', '/')),
    ('-I' + (Join-Path $abiSourceRoot 'include').Replace('\', '/')),
    ('-I' + (Join-Path $vendorSourceRoot 'kernel-open').Replace('\', '/')),
    ('-I' + (Join-Path $vendorSourceRoot 'kernel-open\common\inc').Replace('\', '/'))) +
    $rmIncludeArgs
$abiDefines = @(
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
    '-DPORT_ASSERT_FAILED_USES_STRINGS=0', '-DOS_KERNEL_BUILD')
$abiCommonFlags = @(
    '-fno-strict-aliasing', '-fno-omit-frame-pointer', '-Wformat=2',
    '-Wno-unused-parameter', '-O2', '-ffreestanding', '-fno-stack-protector',
    '-fno-common', '-fno-pic', '-ffunction-sections', '-fdata-sections',
    '-msoft-float', '-mno-red-zone', '-mno-mmx', '-mno-sse', '-mno-sse2',
    '-mno-3dnow', '-Werror-implicit-function-declaration', '-Wwrite-strings',
    '-Wundef')
$abiCFlags = @('-include', $cpuOpsysHeader.Replace('\', '/')) +
             $abiIncludeArgs + $abiDefines + $abiCommonFlags +
             @('--std=gnu11', '-mcmodel=kernel')
$abiAsmFlags = @(
    '-ffreestanding', '-fno-stack-protector', '-fno-common', '-fno-pic',
    '-mno-red-zone', '-mno-mmx', '-mno-sse', '-mno-sse2', '-mno-3dnow',
    '-mcmodel=kernel', '-DOS_NVIDIA_PHASE5=1')
$bridgeIncludes = @(
    ('-I' + (Join-Path $repoRoot 'include').Replace('\', '/')),
    ('-I' + (Join-Path $rmSourceRoot 'include').Replace('\', '/'))) +
    $rmIncludeArgs
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
    '-DOS_NVIDIA_PHASE5=1')
$bridgeFlags = @('-include', $cpuOpsysHeader.Replace('\', '/')) +
               $bridgeIncludes + $bridgeDefines + @(
    '-fno-strict-aliasing', '-fno-omit-frame-pointer', '-Wformat=2',
    '-Wno-unused-parameter', '-O2', '-ffreestanding', '-fno-stack-protector',
    '-fno-common', '-fno-pic', '-ffunction-sections', '-fdata-sections',
    '-msoft-float', '-mno-red-zone', '-mno-mmx', '-mno-sse', '-mno-sse2',
    '-mno-3dnow', '-Werror-implicit-function-declaration', '-Wwrite-strings',
    '-Wundef', '--std=gnu11', '-mcmodel=kernel')

$systemFlags = @(
    '-O2', '-g3', '-fno-omit-frame-pointer', '-Wall', '-Wextra', '-Werror',
    '-Wno-error=unused-function', '-ffreestanding', '-fno-builtin',
    '-fno-stack-protector', '-fno-stack-check', '-fno-pic', '-mno-red-zone',
    '-mno-sse', '-mno-mmx', '-mno-stack-arg-probe', '-fshort-wchar',
    ('-I' + (Join-Path $repoRoot 'include').Replace('\', '/')), '-MMD', '-MP',
    '-DOS_REALTEST=1', '-mabi=sysv', '-DOS_KERNEL_BUILD',
    '-DOS_DEBUG_SERIAL=1', '-DOS_REALTEST_FAILURE_TEST=0',
    '-DOS_XHCI_DIAGNOSTIC_BOT=0', '-DOS_NVIDIA_REALTEST_NO_DESKTOP=1',
    '-DOS_NVIDIA_PHASE5=1', '-mcmodel=kernel')
$driverSystemFlags = $systemFlags + @(
    ('-I' + (Join-Path $repoRoot 'kernel\drivers\gpu\nvidia\port\include').Replace('\', '/')),
    ('-I' + (Join-Path $rmSourceRoot 'include').Replace('\', '/')))

New-Item -ItemType Directory -Path $phase5Root -Force | Out-Null
New-Item -ItemType Directory -Path $phase5SystemRoot -Force | Out-Null
New-Item -ItemType Directory -Path $phase5AbiRoot -Force | Out-Null

Invoke-CompileIfChanged (Join-Path $repoRoot 'kernel\init\devices.c') `
    (Join-Path $phase5SystemRoot 'devices-init.o') $systemFlags
Invoke-CompileIfChanged (Join-Path $repoRoot 'kernel\init\runtime.c') `
    (Join-Path $phase5SystemRoot 'runtime-init.o') $systemFlags
Invoke-CompileIfChanged (Join-Path $rmSourceRoot 'nv_rm_driver.c') `
    (Join-Path $phase5SystemRoot 'nvidia-rm-driver.o') $driverSystemFlags
Invoke-CompileIfChanged (Join-Path $rmSourceRoot 'nv_rm_frontend.c') `
    (Join-Path $phase5SystemRoot 'nvidia-rm-frontend.o') $driverSystemFlags
Invoke-CompileIfChanged (Join-Path $portSourceRoot 'nv_port_memory.c') `
    (Join-Path $phase5ObjectRoot 'nvidia-port-memory.o') $driverSystemFlags
Invoke-CompileIfChanged (Join-Path $portSourceRoot 'nv_port_vm.c') `
    (Join-Path $phase5ObjectRoot 'nvidia-port-vm.o') $driverSystemFlags
Invoke-CompileIfChanged (Join-Path $mmSourceRoot 'vmalloc.c') `
    (Join-Path $phase5ObjectRoot 'vmalloc.o') $systemFlags
Invoke-CompileIfChanged (Join-Path $abiSourceRoot 'nvabi_nv_unsupported.S') `
    (Join-Path $phase5AbiRoot '0006.o') $abiAsmFlags
Invoke-CompileIfChanged (Join-Path $abiSourceRoot 'nvabi_memory.c') `
    (Join-Path $phase5AbiRoot '0003.o') $abiCFlags
Invoke-CompileIfChanged (Join-Path $abiSourceRoot 'nvabi_os.c') `
    (Join-Path $phase5AbiRoot '0009.o') $abiCFlags
Invoke-CompileIfChanged (Join-Path $abiSourceRoot 'nvabi_pci.c') `
    (Join-Path $phase5AbiRoot '0011.o') $abiCFlags
Invoke-CompileIfChanged (Join-Path $abiSourceRoot 'nvabi_firmware.c') `
    (Join-Path $phase5AbiRoot 'nvabi_firmware.o') $abiCFlags
Invoke-CompileIfChanged (Join-Path $rmSourceRoot 'nv_rm_vendor_bridge.c') `
    (Join-Path $phase5ObjectRoot 'nv_rm_vendor_bridge.o') $bridgeFlags
Invoke-CompileIfChanged (Join-Path $repoRoot 'boot\uefi\sha256.c') `
    (Join-Path $phase5ObjectRoot 'sha256.o') $systemFlags

$replacementObjects = @{
    'devices-init.o' = (Join-Path $phase5SystemRoot 'devices-init.o').Replace('\', '/')
    'runtime-init.o' = (Join-Path $phase5SystemRoot 'runtime-init.o').Replace('\', '/')
    'nvidia-rm-driver.o' = (Join-Path $phase5SystemRoot 'nvidia-rm-driver.o').Replace('\', '/')
    'nvidia-rm-frontend.o' = (Join-Path $phase5SystemRoot 'nvidia-rm-frontend.o').Replace('\', '/')
    'nvidia-port-memory.o' = (Join-Path $phase5ObjectRoot 'nvidia-port-memory.o').Replace('\', '/')
    'nvidia-port-vm.o' = (Join-Path $phase5ObjectRoot 'nvidia-port-vm.o').Replace('\', '/')
    'vmalloc.o' = (Join-Path $phase5ObjectRoot 'vmalloc.o').Replace('\', '/')
    '0011.o' = (Join-Path $phase5AbiRoot '0011.o').Replace('\', '/')
    '0009.o' = (Join-Path $phase5AbiRoot '0009.o').Replace('\', '/')
}
$phase4Rows = @(Get-Content -LiteralPath $phase4LinkList |
    Where-Object { $_ -and $_[0] -ne '#' })
$linkRows = New-Object 'System.Collections.Generic.List[string]'
foreach ($row in $phase4Rows) {
    $leaf = Split-Path -Leaf $row
    if ($replacementObjects.ContainsKey($leaf)) {
        [void]$linkRows.Add($replacementObjects[$leaf])
    }
    elseif ($row -match '/phase4/objects/nv_rm_vendor_bridge\.o$') {
        [void]$linkRows.Add((Join-Path $phase5ObjectRoot 'nv_rm_vendor_bridge.o').Replace('\', '/'))
    }
    elseif ($row -match '/phase4/objects/vendor_abi/0006\.o$') {
        [void]$linkRows.Add((Join-Path $phase5AbiRoot '0006.o').Replace('\', '/'))
    }
    elseif ($row -match '/phase4/objects/vendor_abi/0003\.o$') {
        [void]$linkRows.Add((Join-Path $phase5AbiRoot '0003.o').Replace('\', '/'))
    }
    else {
        [void]$linkRows.Add($row)
    }
}
[void]$linkRows.Add((Join-Path $phase5SystemRoot 'nvidia-rm-frontend.o').Replace('\', '/'))
[void]$linkRows.Add((Join-Path $phase5AbiRoot 'nvabi_firmware.o').Replace('\', '/'))
[void]$linkRows.Add((Join-Path $phase5ObjectRoot 'sha256.o').Replace('\', '/'))
[IO.File]::WriteAllLines($phase5LinkList, [string[]]$linkRows,
                          [Text.Encoding]::ASCII)

$linkArgs = @(
    '--gc-sections', '--allow-multiple-definition', '-T',
    $linkerScript.Replace('\', '/'), '-o', $phase5Kernel.Replace('\', '/'),
    ('@' + $phase5LinkList.Replace('\', '/')))
Invoke-Native $linkerPath $linkArgs

$nm = Get-Command x86_64-elf-nm -ErrorAction SilentlyContinue
if ($null -eq $nm) { $nm = Get-Command nm -ErrorAction Stop }
$undefined = @(& $nm.Source '-u' '-P' '--' $phase5Kernel 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0) { throw 'Phase 5 undefined-symbol scan failed' }
if (@($undefined | Where-Object { $_.Trim() }).Count -ne 0) {
    throw "Phase 5 kernel has unresolved symbols: $($undefined[0])"
}

# Build the Phase 5 ESP from the already-built Phase 4 ESP using the same
# content-addressed copy rule used for the physical boot-disk deployment.
$script:stageCopied = 0
$script:stageSkipped = 0
Copy-DirectoryContents (Join-Path $phase4Root 'esp') $phase5Esp
Copy-FileIfChanged $phase5Kernel (Join-Path $phase5Esp 'EFI\OS\kernel.elf')
foreach ($firmwareName in $firmwareNames) {
    Copy-FileIfChanged (Join-Path $firmwareRoot $firmwareName) `
        (Join-Path $phase5Esp ('EFI\OS\firmware\{0}' -f $firmwareName))
}

$firmwareManifest = New-Object 'System.Collections.Generic.List[string]'
foreach ($firmwareName in $firmwareNames) {
    $firmwarePath = Join-Path $firmwareRoot $firmwareName
    $firmwareInfo = Get-Item -LiteralPath $firmwarePath
    $firmwareHash = (Get-FileHash -LiteralPath $firmwarePath -Algorithm SHA256).Hash
    [void]$firmwareManifest.Add(('file={0} size={1} sha256={2} baseline=595.84' -f
        $firmwareName, $firmwareInfo.Length, $firmwareHash))
}
[IO.File]::WriteAllLines((Join-Path $phase5Root 'firmware.manifest'),
                          [string[]]$firmwareManifest, [Text.Encoding]::ASCII)

Write-Output ('PHASE5_KERNEL={0}' -f $phase5Kernel)
Write-Output ('PHASE5_ESP={0}' -f $phase5Esp)
Write-Output ('PHASE5_LINK_OBJECTS={0}' -f $linkRows.Count)
Write-Output ('PHASE5_STAGE_COPIED={0}' -f $script:stageCopied)
Write-Output ('PHASE5_STAGE_SKIPPED={0}' -f $script:stageSkipped)
Write-Output 'PHASE5_BUILD=PASS'
