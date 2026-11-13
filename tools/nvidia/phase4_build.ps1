[CmdletBinding()]
param(
    [string]$Make = 'make',
    [string]$Build = 'build',
    [switch]$RealTest
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'app-layout.ps1')

function Assert-RelativeBuild([string]$Name) {
    if ([string]::IsNullOrWhiteSpace($Name) -or
        [IO.Path]::IsPathRooted($Name) -or
        $Name -match '(^|[\\/])\.\.(?:[\\/]|$)') {
        throw "Build directory must be relative to the project: $Name"
    }
}

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Assert-RelativeBuild $Build
$elfBuildName = $Build + '-elf'
Assert-RelativeBuild $elfBuildName
$vendorRoot = Join-Path $repoRoot 'third_party\nvidia\595.84'
$vendorSourceRoot = Join-Path $vendorRoot 'open-gpu-kernel-modules'
$abiSourceRoot = Join-Path $repoRoot 'kernel\drivers\gpu\nvidia\vendor_abi'
$rmSourceRoot = Join-Path $repoRoot 'kernel\drivers\gpu\nvidia\rm'
$phase3Root = Join-Path $repoRoot 'build\nvidia\phase3'
$outputRoot = Join-Path $repoRoot 'build\nvidia\phase4'
$objectRoot = Join-Path $outputRoot 'objects'
$systemObjectRoot = Join-Path $objectRoot 'system'
$abiObjectRoot = Join-Path $objectRoot 'vendor_abi'
$strongBridgeObject = Join-Path $objectRoot 'nv_rm_vendor_bridge.o'
$workQueueObject = Join-Path $objectRoot 'nv_rm_workqueue.o'
$workQueueStdoutLog = Join-Path $outputRoot 'workqueue_compile.stdout.log'
$workQueueStderrLog = Join-Path $outputRoot 'workqueue_compile.stderr.log'
$linkList = Join-Path $outputRoot 'phase4_link_objects.list'
$abiLinkList = Join-Path $outputRoot 'phase4_abi_link_objects.list'
$kernelElf = Join-Path $outputRoot 'kernel.elf'
$phase4Esp = Join-Path $outputRoot 'esp'
$linkerScript = Join-Path $PSScriptRoot 'phase4-kernel.ld'
$phase3Gate = Join-Path $PSScriptRoot 'phase3_gate.ps1'
$tab = [char]9

function Write-AsciiLines([string]$Path, [System.Collections.IEnumerable]$Lines) {
    [System.IO.File]::WriteAllLines($Path, [string[]]$Lines,
                                    [System.Text.Encoding]::ASCII)
}

function Require-File([string]$Path, [string]$Description) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "missing $Description`: $Path"
    }
}

if (-not (Test-Path -LiteralPath $phase3Gate -PathType Leaf)) {
    throw "P3 Gate is missing: $phase3Gate"
}

# 阶段 4 构建依赖已完成的阶段 3 Gate。该 Gate 会执行 P2 Gate 并重新校验
# 不可变的供应商依赖图，之后本脚本才能将 RM 对象链接进内核映像。
$gateArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $phase3Gate,
              '-Make', $Make)
$gateOutput = @(& powershell.exe @gateArgs 2>&1 |
    ForEach-Object { $_.ToString() })
$gateExitCode = $LASTEXITCODE
foreach ($line in $gateOutput) { Write-Output $line }
if ($gateExitCode -ne 0 -or
    @($gateOutput | Where-Object { $_ -eq 'GATE_P3=PASS' }).Count -ne 1) {
    throw 'P3 Gate did not pass; Phase 4 build is not permitted'
}

Require-File (Join-Path $phase3Root 'vendor-rm.o') 'Phase 3 RM object'
Require-File (Join-Path $phase3Root 'vendor_abi_link_objects.list') 'Phase 3 ABI link list'
Require-File (Join-Path $phase3Root 'system_probe_objects.list') 'Phase 3 System object list'
Require-File $linkerScript 'Phase 4 linker script'

$makeArguments = @("BUILD=$Build")
if ($RealTest) {
    # 实机测试映像需要串口证据和 FAT32 日志路径。将其设为显式选项，
    # 这样阶段 4 硬件准备流程就不会逐字节改变常规 System 构建。
    # OS_REALTEST 会改变编译出的代码，而共享 make 依赖戳不会记录每个 CFLAGS 值。
    # 因此强制干净地重建目标，避免复用 Build 目录时混入普通对象和实机测试对象。
    $makeArguments = @('-B') + $makeArguments
    $makeArguments += @(
        'DEBUG=2',
        'OS_DEBUG_SERIAL=1',
        'OS_REALTEST=1',
        'KERNEL_EXTRA_CFLAGS=-DOS_NVIDIA_REALTEST_NO_DESKTOP=1'
    )
    $makeArguments += 'esp'
}
else {
    $makeArguments += @('loader', 'kernel')
}
$previousMakeErrorActionPreference = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
try {
    # 在 Stop 模式下，Windows PowerShell 可能会将原生命令的 stderr 警告
    # 作为终止性的 NativeCommandError 记录处理。捕获这些记录，并像 ABI 编译器
    # 那样根据 make 的退出码判断生产构建结果。
    $makeOutput = @(& $Make @makeArguments 2>&1 |
        ForEach-Object { $_.ToString() })
    $makeExitCode = $LASTEXITCODE
}
finally {
    $ErrorActionPreference = $previousMakeErrorActionPreference
}
foreach ($line in $makeOutput) { Write-Output $line }
if ($makeExitCode -ne 0) {
    $buildKind = if ($RealTest) { 'real-test ESP' } else { 'production loader/kernel' }
    throw "$buildKind build failed"
}

# Windows 原生构建生成 PE/COFF 内核对象，可用于常规 PE 内核链接；但将每个
# 可重定位对象转换为 ELF 会丢失本地 .bss/.rdata 引用所用的段相对加数。
# 因此直接以 ELF 构建阶段 4 的 System 对象图；上面的原生构建仍用于生成
# UEFI 加载器和 ESP 内容。
$elfKernelExtraCFlags = '-mcmodel=kernel'
if ($RealTest) {
    $elfKernelExtraCFlags += ' -DOS_NVIDIA_REALTEST_NO_DESKTOP=1'
}
$elfMakeArguments = @(
    '-B', ('BUILD={0}' -f $elfBuildName),
    'TOOLPREFIX=x86_64-elf-',
    'LD=x86_64-elf-gcc',
    ('KERNEL_EXTRA_CFLAGS={0}' -f $elfKernelExtraCFlags),
    ('KERNEL_LDFLAGS=-nostdlib -Wl,-T,{0}' -f $linkerScript.Replace('\', '/'))
)
if ($RealTest) {
    $elfMakeArguments += @('DEBUG=2', 'OS_DEBUG_SERIAL=1', 'OS_REALTEST=1')
}
$elfMakeArguments += 'kernel'
$previousMakeErrorActionPreference = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
try {
    $elfMakeOutput = @(& $Make @elfMakeArguments 2>&1 |
        ForEach-Object { $_.ToString() })
    $elfMakeExitCode = $LASTEXITCODE
}
finally {
    $ErrorActionPreference = $previousMakeErrorActionPreference
}
foreach ($line in $elfMakeOutput) { Write-Output $line }
if ($elfMakeExitCode -ne 0) {
    throw 'Phase 4 ELF System kernel build failed'
}

$gccCommand = Get-Command x86_64-elf-gcc -ErrorAction Stop
$linkerCommand = Get-Command x86_64-elf-ld -ErrorAction Stop
$gcc = $gccCommand.Source
$linker = $linkerCommand.Source

New-Item -ItemType Directory -Path $systemObjectRoot -Force | Out-Null
New-Item -ItemType Directory -Path $abiObjectRoot -Force | Out-Null
New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null

$systemBuildRoot = Join-Path $repoRoot $elfBuildName
$systemInputRoot = Join-Path $systemBuildRoot 'kernel'
$systemEspRoot = Join-Path (Join-Path $repoRoot $Build) 'esp'
$systemBuildDescription = if ($RealTest) { 'real-test' } else { 'production' }
Require-File (Join-Path $systemInputRoot 'entry.o') "$systemBuildDescription kernel objects"
$systemFiles = @(Get-ChildItem -LiteralPath $systemInputRoot -File -Filter '*.o' |
    Sort-Object Name)
if ($systemFiles.Count -eq 0) { throw 'production kernel object directory is empty' }

$systemObjects = New-Object 'System.Collections.Generic.List[string]'
foreach ($file in $systemFiles) {
    # 这些对象由 x86_64-elf-gcc 生成，已经包含 ELF 重定位。不要使用 PE objcopy
    # 后端处理它们。
    [void]$systemObjects.Add($file.FullName.Replace('\', '/'))
}

# 供应商源码的包含依赖图有意选择 src/nvidia 中的 Unix 版 nv.h。kernel-open
# 操作系统接口头文件专供 ABI 对象使用；将其生成的 rs_access.h 与 RM NVOC
# 头文件混用会改变类型依赖图，并被编译器拒绝。
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
$bridgeIncludes = @(
    ('-I' + (Join-Path $repoRoot 'include')),
    ('-I' + (Join-Path $rmSourceRoot 'include'))
)
$bridgeIncludes += @($rmIncludesRelative | ForEach-Object {
    '-I' + (Join-Path $vendorSourceRoot $_)
})
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
    '-DPORT_ASSERT_FAILED_USES_STRINGS=0', '-DOS_KERNEL_BUILD'
)
$bridgeFlags = @(
    '-include', $cpuOpsysHeader
) + $bridgeIncludes + $bridgeDefines + @(
    '-fno-strict-aliasing', '-fno-omit-frame-pointer', '-Wformat=2',
    '-Wno-unused-parameter', '-O2', '-ffreestanding', '-fno-stack-protector',
    '-fno-common', '-fno-pic', '-ffunction-sections', '-fdata-sections',
    '-msoft-float', '-mno-red-zone', '-mno-mmx', '-mno-sse', '-mno-sse2',
    '-mno-3dnow', '-Werror-implicit-function-declaration', '-Wwrite-strings',
    '-Wundef', '--std=gnu11', '-mcmodel=kernel'
)

$bridgeArgs = @($bridgeFlags) + @(
    '-c', (Join-Path $rmSourceRoot 'nv_rm_vendor_bridge.c'),
    '-o', $strongBridgeObject
)
$bridgeDiagnostics = @(& $gcc @bridgeArgs 2>&1 |
    ForEach-Object { $_.ToString() })
foreach ($line in $bridgeDiagnostics) { Write-Output $line }
if ($LASTEXITCODE -ne 0 -or
    -not (Test-Path -LiteralPath $strongBridgeObject -PathType Leaf)) {
    throw 'strong Phase 4 vendor bridge compilation failed'
}

$workQueueArgs = @($bridgeFlags) + @(
    '-c', (Join-Path $rmSourceRoot 'nv_rm_workqueue.c'),
    '-o', $workQueueObject
)
$workQueueProcess = Start-Process -FilePath $gcc `
    -ArgumentList ([string[]]$workQueueArgs) `
    -RedirectStandardOutput $workQueueStdoutLog `
    -RedirectStandardError $workQueueStderrLog `
    -Wait -PassThru
$workQueueExitCode = $workQueueProcess.ExitCode
$workQueueDiagnostics = @()
if (Test-Path -LiteralPath $workQueueStdoutLog -PathType Leaf) {
    $workQueueDiagnostics += @(Get-Content -LiteralPath $workQueueStdoutLog |
        ForEach-Object { $_.ToString() })
}
if (Test-Path -LiteralPath $workQueueStderrLog -PathType Leaf) {
    $workQueueDiagnostics += @(Get-Content -LiteralPath $workQueueStderrLog |
        ForEach-Object { $_.ToString() })
}
foreach ($line in $workQueueDiagnostics) { Write-Output $line }
if ($workQueueExitCode -ne 0 -or
    -not (Test-Path -LiteralPath $workQueueObject -PathType Leaf)) {
    throw 'Phase 4 System workqueue adapter compilation failed'
}

# 使用内核代码模型重新编译这 11 个可链接 ABI 适配器。阶段 3 有意将它们编译为
# 可重定位 ABI 探测程序；最终阶段 4 映像放置在 System 高半区地址后，
# 其中绝对地址的诊断字符串重定位将不再有效。
$abiIncludeArgs = @(
    ('-I' + (Join-Path $repoRoot 'include')),
    ('-I' + (Join-Path $repoRoot 'kernel\drivers\gpu\nvidia\port\include')),
    ('-I' + (Join-Path $abiSourceRoot 'include')),
    ('-I' + (Join-Path $vendorSourceRoot 'kernel-open')),
    ('-I' + (Join-Path $vendorSourceRoot 'kernel-open\common\inc'))
)
$abiIncludeArgs += @($rmIncludesRelative | ForEach-Object {
    '-I' + (Join-Path $vendorSourceRoot $_)
})
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
    '-DPORT_ASSERT_FAILED_USES_STRINGS=0', '-DOS_KERNEL_BUILD'
)
$abiCommonFlags = @(
    '-fno-strict-aliasing', '-fno-omit-frame-pointer', '-Wformat=2',
    '-Wno-unused-parameter', '-O2', '-ffreestanding', '-fno-stack-protector',
    '-fno-common', '-fno-pic', '-ffunction-sections', '-fdata-sections',
    '-msoft-float', '-mno-red-zone', '-mno-mmx', '-mno-sse', '-mno-sse2',
    '-mno-3dnow', '-Werror-implicit-function-declaration', '-Wwrite-strings',
    '-Wundef'
)
$abiCFlags = @('-include', $cpuOpsysHeader) + $abiIncludeArgs +
             $abiDefines + $abiCommonFlags + @('--std=gnu11', '-mcmodel=kernel')
$abiAsmFlags = @(
    '-ffreestanding', '-fno-stack-protector', '-fno-common', '-fno-pic',
    '-mno-red-zone', '-mno-mmx', '-mno-sse', '-mno-sse2', '-mno-3dnow',
    '-mcmodel=kernel'
)
$abiObjects = New-Object 'System.Collections.Generic.List[string]'
$abiObjectRows = @(Get-Content -LiteralPath (Join-Path $phase3Root 'vendor_abi_objects.txt') |
    Where-Object { $_ -and $_[0] -ne '#' })
foreach ($row in $abiObjectRows) {
    $fields = $row -split "`t", -1
    if ($fields.Count -ne 8 -or $fields[0] -ne 'compiled') {
        throw "Phase 3 ABI object report is malformed: $row"
    }
    if ($fields[4] -like '*vendor_abi/nvabi_contracts.c') { continue }

    $abiSource = Join-Path $repoRoot ($fields[4].Replace('/', '\'))
    $abiObject = Join-Path $abiObjectRoot ((Split-Path -Leaf $fields[5]))
    $abiArgs = if ($fields[2] -eq 'asm') { @($abiAsmFlags) } else { @($abiCFlags) }
    $abiArgs += @('-c', $abiSource, '-o', $abiObject)
    $previousErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $abiDiagnostics = @(& $gcc @abiArgs 2>&1 |
            ForEach-Object { $_.ToString() })
        $abiExitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }
    foreach ($line in $abiDiagnostics) { Write-Output $line }
    if ($abiExitCode -ne 0 -or
        -not (Test-Path -LiteralPath $abiObject -PathType Leaf)) {
        throw "Phase 4 ABI object compilation failed: $abiSource"
    }
    [void]$abiObjects.Add($abiObject.Replace('\', '/'))
}
if ($abiObjects.Count -ne 11) {
    throw "Phase 4 ABI link object count is $($abiObjects.Count), expected 11"
}
Write-AsciiLines $abiLinkList $abiObjects

$linkObjects = New-Object 'System.Collections.Generic.List[string]'
foreach ($object in $systemObjects) { [void]$linkObjects.Add($object) }
[void]$linkObjects.Add((Join-Path $phase3Root 'vendor-rm.o').Replace('\', '/'))
[void]$linkObjects.Add($workQueueObject.Replace('\', '/'))
foreach ($object in $abiObjects) { [void]$linkObjects.Add($object) }
[void]$linkObjects.Add($strongBridgeObject.Replace('\', '/'))
Write-AsciiLines $linkList $linkObjects

$linkArgs = @(
    '--gc-sections', '--allow-multiple-definition', '-T', $linkerScript,
    '-o', $kernelElf, ('@' + $linkList)
)
$linkDiagnostics = @(& $linker @linkArgs 2>&1 |
    ForEach-Object { $_.ToString() })
foreach ($line in $linkDiagnostics) { Write-Output $line }
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $kernelElf -PathType Leaf)) {
    throw 'Phase 4 RM/System final link failed'
}

New-Item -ItemType Directory -Path $phase4Esp -Force | Out-Null
if (-not (Test-Path -LiteralPath $systemEspRoot -PathType Container)) {
    throw "$systemBuildDescription ESP directory is missing: $systemEspRoot"
}
Assert-AppGuestLayout $phase4Esp
foreach ($source in @(Get-ChildItem -LiteralPath $systemEspRoot -Recurse -File)) {
    $relative = $source.FullName.Substring($systemEspRoot.Length).TrimStart('\', '/')
    if (Test-LegacyGuestPath $relative) { continue }
    $target = Join-Path $phase4Esp $relative
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
    Copy-Item -LiteralPath $source.FullName -Destination $target -Force
}
New-Item -ItemType Directory -Path (Join-Path $phase4Esp 'EFI\OS') -Force |
    Out-Null
Copy-Item -LiteralPath $kernelElf -Destination (Join-Path $phase4Esp 'EFI\OS\kernel.elf') -Force

Write-Output ('PHASE4_KERNEL={0}' -f $kernelElf)
Write-Output ('PHASE4_ESP={0}' -f $phase4Esp)
Write-Output ('PHASE4_OS_OBJECTS={0}' -f $systemObjects.Count)
Write-Output ('PHASE4_LINK_OBJECTS={0}' -f $linkObjects.Count)
Write-Output ('PHASE4_OS_BUILD={0}' -f $Build)
Write-Output ('PHASE4_OS_ELF_BUILD={0}' -f $elfBuildName)
Write-Output ('PHASE4_REALTEST={0}' -f ($(if ($RealTest) { '1' } else { '0' })))
Write-Output 'PHASE4_BUILD=PASS'
