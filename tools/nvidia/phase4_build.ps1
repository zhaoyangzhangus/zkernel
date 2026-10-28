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

# A Phase 4 build is downstream of the completed Phase 3 Gate.  The Gate
# performs the P2 Gate and revalidates the immutable vendor graph before this
# script is allowed to link any RM object into a kernel image.
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
    # Real-test images need serial evidence and the FAT32 log path.  Keep this
    # opt-in so the normal System build remains byte-for-byte out of scope for
    # the Phase 4 hardware preparation path.
    # OS_REALTEST changes compiled code, while the shared make dependency
    # stamps do not encode every CFLAGS value.  Force a clean target rebuild so
    # a reused Build directory cannot mix ordinary and real-test objects.
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
    # Windows PowerShell can surface native stderr warnings as terminating
    # NativeCommandError records under Stop.  Capture them and judge the
    # production build by make's exit code, as done for the ABI compilers.
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

# A native Windows build emits PE/COFF kernel objects.  They are suitable for
# the normal PE kernel link, but converting each relocatable object to ELF
# loses the section-relative addends used by local .bss/.rdata references.
# Build the Phase 4 System object graph directly as ELF instead; the native
# build above remains the source of the UEFI loader and ESP payload.
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
    # These objects are produced by x86_64-elf-gcc and already carry ELF
    # relocations.  Do not pass them through the PE objcopy backend.
    [void]$systemObjects.Add($file.FullName.Replace('\', '/'))
}

# The vendor source include graph intentionally selects src/nvidia's Unix
# nv.h.  The kernel-open OS interface header is reserved for the ABI objects;
# mixing its generated rs_access.h with the RM NVOC headers changes the type
# graph and is rejected by the compiler.
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

# Recompile the 11 linkable ABI adapters with the kernel code model.  Phase 3
# intentionally compiles them as a relocatable ABI probe; their absolute
# diagnostic-string relocations are not valid once the final Phase 4 image is
# placed at System' high-half address.
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
