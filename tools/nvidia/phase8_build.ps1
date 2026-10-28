[CmdletBinding()]
param(
    [string]$Compiler = 'x86_64-elf-gcc',
    [string]$Linker = 'x86_64-elf-ld',
    [string]$Make = 'make.exe',
    [string]$GlibcRoot = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'app-layout.ps1')
. (Join-Path $PSScriptRoot 'object-paths.ps1')

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$vendorSourceRoot = Join-Path $repoRoot 'third_party\nvidia\595.84\open-gpu-kernel-modules'
$rmSourceRoot = Join-Path $repoRoot 'kernel\drivers\gpu\nvidia\rm'
$phase6Root = Join-Path $repoRoot 'build\nvidia\phase6'
$phase6Esp = Join-Path $phase6Root 'esp'
$phase6LinkList = Join-Path $phase6Root 'phase6_link_objects.list'
$phase8Root = Join-Path $repoRoot 'build\nvidia\phase8'
$phase8ObjectRoot = Join-Path $phase8Root 'objects'
$phase8SystemRoot = Join-Path $phase8Root 'system'
$phase8Kernel = Join-Path $phase8Root 'kernel.elf'
$phase8Esp = Join-Path $phase8Root 'esp'
$linkerScript = Join-Path $PSScriptRoot 'phase4-kernel.ld'
$cpuOpsysHeader = Join-Path $vendorSourceRoot 'src\common\sdk\nvidia\inc\cpuopsys.h'

function Require-File([string]$Path, [string]$Description) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw ('missing {0}: {1}' -f $Description, $Path)
    }
}

function Require-Directory([string]$Path, [string]$Description) {
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        throw ('missing {0}: {1}' -f $Description, $Path)
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
        throw ('native tool failed ({0}): {1}' -f $exitCode, $Tool)
    }
}

function Invoke-CompileIfChanged([string]$Source, [string]$Output,
                                 [string[]]$Arguments) {
    $signaturePath = $Output + '.cmd'
    $sourceHash = (Get-FileHash -LiteralPath $Source -Algorithm SHA256).Hash
    $signature = $Source + [Environment]::NewLine + $sourceHash +
                 [Environment]::NewLine +
                 (($Arguments | ForEach-Object { $_.ToString() }) -join
                  [Environment]::NewLine)
    if ((Test-Path -LiteralPath $Output -PathType Leaf) -and
        (Test-Path -LiteralPath $signaturePath -PathType Leaf) -and
        [IO.File]::ReadAllText($signaturePath) -eq $signature) {
        Write-Output ('PHASE8_INCREMENTAL_SKIP={0}' -f $Source)
        return $false
    }
    New-Item -ItemType Directory -Path (Split-Path -Parent $Output) -Force |
        Out-Null
    Invoke-Native $Compiler (@($Arguments) + @(
        '-c', $Source.Replace('\', '/'), '-o', $Output.Replace('\', '/')))
    [IO.File]::WriteAllText($signaturePath, $signature,
                             [Text.Encoding]::ASCII)
    Write-Output ('PHASE8_COMPILE={0}' -f $Source)
    return $true
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
    Require-Directory $SourceRoot 'Phase 6 ESP'
    foreach ($source in @(Get-ChildItem -LiteralPath $SourceRoot -Recurse -File)) {
        $relative = $source.FullName.Substring($SourceRoot.Length).TrimStart('\', '/')
        if (Test-LegacyGuestPath $relative) { continue }
        Copy-FileIfChanged $source.FullName (Join-Path $TargetRoot $relative)
    }
}

Require-File $phase6LinkList 'Phase 6 link list'
Require-File (Join-Path $phase6Root 'kernel.elf') 'Phase 6 kernel image'
Require-Directory $phase6Esp 'Phase 6 ESP'
Require-File $linkerScript 'Phase 4 linker script'
Require-File $cpuOpsysHeader 'vendor cpuopsys header'
Require-File (Join-Path $rmSourceRoot 'nv_rm_vendor_bridge.c') 'NVIDIA vendor bridge'
Require-File (Join-Path $rmSourceRoot 'nv_rm_frontend.c') 'System NVIDIA frontend'

$vendorRelative = 'third_party/nvidia/595.84/open-gpu-kernel-modules'
$vendorDiff = @(& git -C $repoRoot diff --name-only -- $vendorRelative 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0) { throw 'git vendor diff check failed' }
if (@($vendorDiff | Where-Object { $_.Trim() }).Count -ne 0) {
    throw ('vendor source tree is modified: {0}' -f $vendorDiff[0])
}

$compilerPath = (Get-Command $Compiler -ErrorAction Stop).Source
$linkerPath = (Get-Command $Linker -ErrorAction Stop).Source
$makePath = (Get-Command $Make -ErrorAction Stop).Source

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
    '-DPORT_IS_CHECKED_BUILD=0', '-DPORT_MODULE_atomic=1',
    '-DPORT_MODULE_core=1', '-DPORT_MODULE_cpu=1',
    '-DPORT_MODULE_crypto=1', '-DPORT_MODULE_debug=1',
    '-DPORT_MODULE_memory=1', '-DPORT_MODULE_safe=1',
    '-DPORT_MODULE_string=1', '-DPORT_MODULE_sync=1',
    '-DPORT_MODULE_thread=1', '-DPORT_MODULE_util=1',
    '-DPORT_MODULE_example=0', '-DPORT_MODULE_mmio=0',
    '-DPORT_MODULE_time=0', '-DRS_STANDALONE=0',
    '-DRS_STANDALONE_TEST=0', '-DRS_COMPATABILITY_MODE=1',
    '-DRS_PROVIDES_API_STATE=0', '-DNV_CONTAINERS_NO_TEMPLATES',
    '-DINCLUDE_NVLINK_LIB', '-DINCLUDE_NVSWITCH_LIB',
    '-DNV_PRINTF_STRINGS_ALLOWED=0', '-DNV_ASSERT_FAILED_USES_STRINGS=0',
    '-DPORT_ASSERT_FAILED_USES_STRINGS=0', '-DOS_KERNEL_BUILD',
    '-DOS_REALTEST=1',
    '-DOS_NVIDIA_PHASE5=1', '-DOS_NVIDIA_PHASE6=1'
)
$bridgeFlags = @('-include', $cpuOpsysHeader.Replace('\', '/')) +
               $bridgeIncludes + $bridgeDefines + @(
    '-fno-strict-aliasing', '-fno-omit-frame-pointer', '-Wformat=2',
    '-Wno-unused-parameter', '-O2', '-ffreestanding',
    '-fno-stack-protector', '-fno-common', '-fno-pic',
    '-ffunction-sections', '-fdata-sections', '-msoft-float',
    '-mno-red-zone', '-mno-mmx', '-mno-sse', '-mno-sse2',
    '-mno-3dnow', '-Werror-implicit-function-declaration',
    '-Wwrite-strings', '-Wundef', '--std=gnu11', '-mcmodel=kernel')

New-Item -ItemType Directory -Path $phase8ObjectRoot, $phase8SystemRoot -Force |
    Out-Null
$bridgeObject = Join-Path $phase8ObjectRoot 'nv_rm_vendor_bridge.o'
Invoke-CompileIfChanged (Join-Path $rmSourceRoot 'nv_rm_vendor_bridge.c') $bridgeObject $bridgeFlags |
    Write-Output

# Phase 7 changed process/thread layout. Rebuild every System object named by
# the Phase 6 link list in an isolated ELF directory to avoid stale ABI mixes.
$normalTargets = New-Object 'System.Collections.Generic.List[string]'
$normalLeaves = New-Object 'System.Collections.Generic.HashSet[string]'
foreach ($row in @(Get-Content -LiteralPath $phase6LinkList)) {
    $row = ConvertTo-ObjectListPath $row
    if (-not (Test-ObjectListData $row)) { continue }
    if ($row -match '(?i)/build-realtest-elf/kernel/([^/]+\.o)$') {
        $leaf = $Matches[1]
        if ($normalLeaves.Add($leaf)) {
            [void]$normalTargets.Add(('build/nvidia/phase8/system/kernel/{0}' -f $leaf))
        }
    }
}
foreach ($leaf in @(
        'runtime-init.o',
        'nvidia-rm-driver.o',
        'linux-abi.o',
        'linux-phase7-test.o',
        'linux-phase8-test.o',
        'nvidia-rm-frontend.o')) {
    if ($normalLeaves.Add($leaf)) {
        [void]$normalTargets.Add(('build/nvidia/phase8/system/kernel/{0}' -f $leaf))
    }
}

$makeArgs = @(
    '-f', 'GNUmakefile',
    'BUILD=build/nvidia/phase8/system',
    'TOOLPREFIX=x86_64-elf-',
    'ELFTOOLPREFIX=x86_64-elf-',
    'HOSTCC=gcc',
    'DEBUG=2',
    'OS_DEBUG_SERIAL=1',
    'OS_REALTEST=1',
    'OS_PHASE7=0',
    'OS_PHASE8=1',
    'KERNEL_EXTRA_CFLAGS=-DOS_NVIDIA_PHASE5=1 -DOS_NVIDIA_PHASE6=1 -mcmodel=kernel'
) + @($normalTargets)
Invoke-Native $makePath $makeArgs | Write-Output

$systemKernelRoot = Join-Path $phase8SystemRoot 'kernel'
$newObjectPaths = @{}
foreach ($leaf in @(
        'runtime-init.o',
        'nvidia-rm-driver.o',
        'linux-abi.o',
        'linux-phase7-test.o',
        'linux-phase8-test.o',
        'nvidia-rm-frontend.o')) {
    $newObjectPaths[$leaf] =
        (Join-Path $systemKernelRoot $leaf).Replace('\', '/')
}

$linkRows = New-Object 'System.Collections.Generic.List[string]'
foreach ($row in @(Get-Content -LiteralPath $phase6LinkList)) {
    $row = ConvertTo-ObjectListPath $row
    if (-not (Test-ObjectListData $row)) {
        [void]$linkRows.Add($row)
        continue
    }
    $leaf = Split-Path -Leaf $row
    if ($row -match '(?i)/build-realtest-elf/kernel/([^/]+\.o)$') {
        [void]$linkRows.Add(
            (Join-Path $systemKernelRoot $Matches[1]).Replace('\', '/'))
    }
    elseif ($newObjectPaths.ContainsKey($leaf)) {
        [void]$linkRows.Add($newObjectPaths[$leaf])
    }
    elseif ($row -match '(?i)/nv_rm_vendor_bridge\.o$') {
        [void]$linkRows.Add($bridgeObject.Replace('\', '/'))
    }
    else {
        [void]$linkRows.Add($row)
    }
}
foreach ($leaf in @(
        'linux-abi.o',
        'linux-phase7-test.o',
        'linux-phase8-test.o',
        'nvidia-rm-frontend.o')) {
    $path = $newObjectPaths[$leaf]
    if (@($linkRows | Where-Object { $_ -eq $path }).Count -eq 0) {
        [void]$linkRows.Add($path)
    }
}
$phase8LinkList = Join-Path $phase8Root 'phase8_link_objects.list'
[IO.File]::WriteAllLines($phase8LinkList, [string[]]$linkRows,
                          [Text.Encoding]::ASCII)

$linkArgs = @(
    '--gc-sections', '--allow-multiple-definition', '-T',
    $linkerScript.Replace('\', '/'), '-o',
    $phase8Kernel.Replace('\', '/'),
    ('@' + $phase8LinkList.Replace('\', '/')))
Invoke-Native $linkerPath $linkArgs | Write-Output

$nm = Get-Command x86_64-elf-nm -ErrorAction SilentlyContinue
if ($null -eq $nm) { $nm = Get-Command nm -ErrorAction Stop }
$undefined = @(& $nm.Source '-u' '-P' '--' $phase8Kernel 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0) { throw 'Phase 8 undefined-symbol scan failed' }
if (@($undefined | Where-Object { $_.Trim() }).Count -ne 0) {
    throw ('Phase 8 kernel has unresolved symbols: {0}' -f $undefined[0])
}

# Build the real Linux/glibc probe with the pinned Debian development files.
if ([string]::IsNullOrWhiteSpace($GlibcRoot)) {
    $GlibcRoot = Join-Path $repoRoot 'build-phase7\host-deps\glibc'
}
$glibcRootPath = (Resolve-Path $GlibcRoot).Path
$glibcDev = Join-Path $glibcRootPath 'libc6-dev-extract'
$glibcRun = Join-Path $glibcRootPath 'libc6-extract'
$hostDevInclude = Join-Path $glibcDev 'usr\include'
$hostArchInclude = Join-Path $hostDevInclude 'x86_64-linux-gnu'
$hostDevLib = Join-Path $glibcDev 'usr\lib\x86_64-linux-gnu'
$hostRunLib = Join-Path $glibcRun 'lib\x86_64-linux-gnu'
$hostRoot = Join-Path $repoRoot 'build-phase8\phase8-frontend'
$hostObject = Join-Path $hostRoot 'p8-frontend.o'
$hostBinary = Join-Path $hostRoot 'p8-frontend'
foreach ($required in @(
        (Join-Path $hostDevInclude 'stdint.h'),
        (Join-Path $hostArchInclude 'bits\types.h'),
        (Join-Path $hostDevLib 'crt1.o'),
        (Join-Path $hostDevLib 'crti.o'),
        (Join-Path $hostDevLib 'crtn.o'),
        (Join-Path $hostDevLib 'libc_nonshared.a'),
        (Join-Path $hostRunLib 'libc.so.6'),
        (Join-Path $hostRunLib 'ld-linux-x86-64.so.2'))) {
    Require-File $required 'pinned glibc host dependency'
}
$hostFlags = @(
    '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
    '-D_GNU_SOURCE', '-D__linux__', '-D__x86_64__',
    '-isystem', $hostDevInclude.Replace('\', '/'),
    '-isystem', $hostArchInclude.Replace('\', '/'),
    '-I' + (Join-Path $repoRoot 'include').Replace('\', '/')
)
Invoke-CompileIfChanged (Join-Path $repoRoot 'tools\nvidia\phase8_frontend_test.c') $hostObject $hostFlags |
    Write-Output

$hostLinkArgs = @(
    '-nostdlib', '-nodefaultlibs', '-nostartfiles',
    '-Wl,--build-id=none',
    '-Wl,-dynamic-linker,/usr/apps/p8-frontend/lib/ld-linux-x86-64.so.2',
    '-Wl,-rpath,/usr/apps/p8-frontend/lib',
    '-Wl,--allow-shlib-undefined',
    ('-Wl,-rpath-link,{0}' -f $hostRunLib.Replace('\', '/')),
    '-Wl,-z,now',
    (Join-Path $hostDevLib 'crt1.o').Replace('\', '/'),
    (Join-Path $hostDevLib 'crti.o').Replace('\', '/'),
    $hostObject.Replace('\', '/'),
    (Join-Path $hostRunLib 'libc.so.6').Replace('\', '/'),
    (Join-Path $hostDevLib 'libc_nonshared.a').Replace('\', '/'),
    (Join-Path $hostDevLib 'crtn.o').Replace('\', '/'),
    '-o', $hostBinary.Replace('\', '/'))
$hostLinkSignature = $hostBinary + '.cmd'
$hostLinkText = ($hostLinkArgs -join [Environment]::NewLine)
foreach ($input in @(
        $hostObject,
        (Join-Path $hostDevLib 'crt1.o'),
        (Join-Path $hostDevLib 'crti.o'),
        (Join-Path $hostRunLib 'libc.so.6'),
        (Join-Path $hostRunLib 'ld-linux-x86-64.so.2'),
        (Join-Path $hostDevLib 'libc_nonshared.a'),
        (Join-Path $hostDevLib 'crtn.o'))) {
    $hostLinkText += [Environment]::NewLine + $input +
                     [Environment]::NewLine +
                     (Get-FileHash -LiteralPath $input -Algorithm SHA256).Hash
}
if (-not ((Test-Path -LiteralPath $hostBinary -PathType Leaf) -and
          (Test-Path -LiteralPath $hostLinkSignature -PathType Leaf) -and
          [IO.File]::ReadAllText($hostLinkSignature) -eq $hostLinkText)) {
    New-Item -ItemType Directory -Path $hostRoot -Force | Out-Null
    Invoke-Native $compilerPath $hostLinkArgs | Write-Output
    [IO.File]::WriteAllText($hostLinkSignature, $hostLinkText,
                             [Text.Encoding]::ASCII)
    Write-Output 'PHASE8_HOST_LINK=PASS'
}
else {
    Write-Output 'PHASE8_HOST_LINK=INCREMENTAL_SKIP'
}

$script:stageCopied = 0
$script:stageSkipped = 0
Copy-DirectoryContents $phase6Esp $phase8Esp
Copy-FileIfChanged $phase8Kernel (Join-Path $phase8Esp 'EFI\OS\kernel.elf')

Write-Output ('PHASE8_KERNEL={0}' -f $phase8Kernel)
Write-Output ('PHASE8_ESP={0}' -f $phase8Esp)
Write-Output ('PHASE8_LINK_OBJECTS={0}' -f $linkRows.Count)
Write-Output ('PHASE8_STAGE_COPIED={0}' -f $script:stageCopied)
Write-Output ('PHASE8_STAGE_SKIPPED={0}' -f $script:stageSkipped)
Write-Output 'PHASE8_BUILD=PASS'
