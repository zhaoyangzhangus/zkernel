[CmdletBinding()]
param(
    [string]$Compiler = 'x86_64-elf-gcc',
    [string]$Linker = 'x86_64-elf-ld',
    [string]$Make = 'make.exe',
    [string]$GlibcRoot = 'build-phase7/host-deps/glibc',
    [string]$PackageRoot = 'build/nvidia/official-595.84/extracted-gitbash',
    [string]$PackageRun = 'build/nvidia/official-595.84/NVIDIA-Linux-x86_64-595.84.run',
    [string]$P10RealTestLog = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'object-paths.ps1')
. (Join-Path $PSScriptRoot 'log-records.ps1')
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$phase9Root = Join-Path $repoRoot 'build\nvidia\phase9'
$phase9LinkList = Join-Path $phase9Root 'phase9_link_objects.list'
$phase10Root = Join-Path $repoRoot 'build\nvidia\phase10'
$phase11Root = Join-Path $repoRoot 'build\nvidia\phase11'
$phase11SystemRoot = Join-Path $phase11Root 'system'
$phase11Kernel = Join-Path $phase11Root 'kernel.elf'
$phase11ObjectRoot = Join-Path $phase11Root 'objects'
$phase11BridgeObject = Join-Path $phase11ObjectRoot 'nv_rm_vendor_bridge.o'
$linkerScript = Join-Path $PSScriptRoot 'phase4-kernel.ld'

function Resolve-RepoPath([string]$Path) {
    if ([IO.Path]::IsPathRooted($Path)) { return [IO.Path]::GetFullPath($Path) }
    return [IO.Path]::GetFullPath((Join-Path $repoRoot $Path))
}

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
        Write-Output ('P11_INCREMENTAL_SKIP={0}' -f $Source)
        return
    }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Output) |
        Out-Null
    Invoke-Native $Compiler (@($Arguments) + @(
        '-c', $Source.Replace('\', '/'), '-o', $Output.Replace('\', '/')))
    [IO.File]::WriteAllText($signaturePath, $signature, [Text.Encoding]::ASCII)
    Write-Output ('P11_COMPILE={0}' -f $Source)
}

Require-File (Join-Path $repoRoot 'tools\nvidia\phase10_gate.ps1') `
    'Phase 10 Gate script'
if ([string]::IsNullOrWhiteSpace($P10RealTestLog)) {
    $hardwareRoot = Join-Path $repoRoot 'build-realtest\real-hardware'
    $p10Candidates = @(Get-ChildItem -LiteralPath $hardwareRoot -Recurse -File `
        -Filter 'realtest.log' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending)
    foreach ($candidate in $p10Candidates) {
        $candidateText = Get-Content -LiteralPath $candidate.FullName -Raw
        $candidateResult = Join-Path (Split-Path -Parent $candidate.FullName) `
            'realtest.result'
        if ((Test-LogRecord $candidateText 'OS_P10_PASS') -and
            (Test-Path -LiteralPath $candidateResult -PathType Leaf) -and
            (Get-Content -LiteralPath $candidateResult -Raw) -match 'status=pass') {
            $P10RealTestLog = $candidate.FullName
            break
        }
    }
}
Require-File (Resolve-RepoPath $P10RealTestLog) 'completed Phase 10 real-test log'
$p10GateOutput = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
    (Join-Path $repoRoot 'tools\nvidia\phase10_gate.ps1') `
    -Build build/nvidia/phase10 -RealTestLog (Resolve-RepoPath $P10RealTestLog) 2>&1 |
    ForEach-Object { $_.ToString() })
foreach ($line in $p10GateOutput) { Write-Output $line }
if ($LASTEXITCODE -ne 0 -or
    @($p10GateOutput | Where-Object { $_ -match 'GATE_P10=PASS' }).Count -ne 1) {
    throw 'Phase 10 Gate is not passing; Phase 11 build is refused.'
}

Require-File $phase9LinkList 'Phase 9 link list'
Require-File (Join-Path $phase9Root 'kernel.elf') 'Phase 9 kernel image'
Require-File $linkerScript 'Phase 4 linker script'
$phase8BridgeSource = Join-Path $repoRoot 'kernel\drivers\gpu\nvidia\rm\nv_rm_vendor_bridge.c'
$phase8BridgeCmd = Join-Path $repoRoot 'build\nvidia\phase8\objects\nv_rm_vendor_bridge.o.cmd'
Require-File $phase8BridgeCmd 'verified Phase 8 vendor bridge compile signature'

$compilerPath = (Get-Command $Compiler -ErrorAction Stop).Source
$linkerPath = (Get-Command $Linker -ErrorAction Stop).Source
$makePath = (Get-Command $Make -ErrorAction Stop).Source
$phase11KernelRoot = Join-Path $phase11SystemRoot 'kernel'
New-Item -ItemType Directory -Force -Path $phase11KernelRoot | Out-Null

$normalTargets = New-Object 'System.Collections.Generic.List[string]'
$normalLeaves = New-Object 'System.Collections.Generic.HashSet[string]'
foreach ($row in @(Get-Content -LiteralPath $phase9LinkList)) {
    $row = ConvertTo-ObjectListPath $row
    if (-not (Test-ObjectListData $row)) { continue }
    if ($row -match '(?i)/build/nvidia/phase9/system/kernel/([^/]+\.o)$') {
        $leaf = $Matches[1]
        if ($normalLeaves.Add($leaf)) {
            [void]$normalTargets.Add(
                ('build/nvidia/phase11/system/kernel/{0}' -f $leaf))
        }
    }
}
foreach ($leaf in @('runtime-init.o', 'linux-abi.o',
                    'linux-phase7-test.o', 'linux-phase8-test.o',
                    'linux-phase9-test.o', 'linux-phase11-test.o',
                    'nvidia-rm-frontend.o')) {
    if ($normalLeaves.Add($leaf)) {
        [void]$normalTargets.Add(
            ('build/nvidia/phase11/system/kernel/{0}' -f $leaf))
    }
}

$makeArgs = @(
    '-f', 'GNUmakefile',
    'BUILD=build/nvidia/phase11/system',
    'TOOLPREFIX=x86_64-elf-',
    'ELFTOOLPREFIX=x86_64-elf-',
    'HOSTCC=gcc',
    'DEBUG=2',
    'OS_DEBUG_SERIAL=1',
    'OS_REALTEST=1',
    'OS_PHASE7=0',
    'OS_PHASE8=0',
    'OS_PHASE9=0',
    'OS_PHASE10=0',
    'KERNEL_EXTRA_CFLAGS=-DOS_NVIDIA_PHASE5=1 -DOS_NVIDIA_PHASE6=0 -DOS_PHASE11=1 -mcmodel=kernel'
) + @($normalTargets)
Invoke-Native $makePath $makeArgs

$bridgeCommandLines = @(Get-Content -LiteralPath $phase8BridgeCmd)
if ($bridgeCommandLines.Count -lt 3) {
    throw 'Phase 8 vendor bridge compile signature is incomplete'
}
if ((Get-FileHash -LiteralPath $phase8BridgeSource -Algorithm SHA256).Hash -ne
    $bridgeCommandLines[1]) {
    throw 'Phase 8 vendor bridge compile signature is stale'
}
$bridgeArguments = New-Object 'System.Collections.Generic.List[string]'
foreach ($argument in @($bridgeCommandLines | Select-Object -Skip 2)) {
    if ([string]::IsNullOrEmpty($argument)) { continue }
    if ($argument -eq '-DOS_NVIDIA_PHASE6=1') {
        [void]$bridgeArguments.Add('-DOS_NVIDIA_PHASE6=0')
        continue
    }
    if ($argument -eq '-DOS_NVIDIA_PHASE6_FULL_GATE=1') { continue }
    [void]$bridgeArguments.Add($argument)
}
Invoke-CompileIfChanged $phase8BridgeSource $phase11BridgeObject `
    ([string[]]$bridgeArguments)

$linkRows = New-Object 'System.Collections.Generic.List[string]'
foreach ($row in @(Get-Content -LiteralPath $phase9LinkList)) {
    $row = ConvertTo-ObjectListPath $row
    if (-not (Test-ObjectListData $row)) {
        [void]$linkRows.Add($row)
        continue
    }
    if ($row -match '(?i)/build/nvidia/phase9/system/kernel/([^/]+\.o)$') {
        [void]$linkRows.Add(
            (Join-Path $phase11KernelRoot $Matches[1]).Replace('\', '/'))
    }
    elseif ($row -match '(?i)[\\/]build/nvidia/phase8/objects/nv_rm_vendor_bridge\.o$') {
        [void]$linkRows.Add($phase11BridgeObject.Replace('\', '/'))
    }
    elseif ($row -match '(?i)[\\/]build/nvidia/phase6/objects/nv_rm_phase6\.o$') {
        continue
    }
    else { [void]$linkRows.Add($row) }
}
$phase11RunnerObject =
    (Join-Path $phase11KernelRoot 'linux-phase11-test.o').Replace('\', '/')
if (@($linkRows | Where-Object { $_ -eq $phase11RunnerObject }).Count -eq 0) {
    [void]$linkRows.Add($phase11RunnerObject)
}
$phase11LinkList = Join-Path $phase11Root 'phase11_link_objects.list'
New-Item -ItemType Directory -Force -Path $phase11Root | Out-Null
[IO.File]::WriteAllLines($phase11LinkList, [string[]]$linkRows,
                          [Text.Encoding]::ASCII)

$linkArgs = @(
    '--gc-sections', '--allow-multiple-definition', '-T',
    $linkerScript.Replace('\', '/'), '-o',
    $phase11Kernel.Replace('\', '/'),
    ('@' + $phase11LinkList.Replace('\', '/')))
Invoke-Native $linkerPath $linkArgs

$nm = Get-Command x86_64-elf-nm -ErrorAction SilentlyContinue
if ($null -eq $nm) { $nm = Get-Command nm -ErrorAction Stop }
$undefined = @(& $nm.Source '-u' '-P' '--' $phase11Kernel 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0) { throw 'Phase 11 kernel undefined-symbol scan failed' }
$unexpected = @($undefined | Where-Object { $_.Trim() })
if ($unexpected.Count -ne 0) {
    throw ('Phase 11 kernel has unresolved symbols: {0}' -f $unexpected[0])
}

$glibcRootPath = (Resolve-Path $GlibcRoot).Path
$glibcDev = Join-Path $glibcRootPath 'libc6-dev-extract'
$glibcRun = Join-Path $glibcRootPath 'libc6-extract'
$hostDevInclude = Join-Path $glibcDev 'usr\include'
$hostArchInclude = Join-Path $hostDevInclude 'x86_64-linux-gnu'
$hostDevLib = Join-Path $glibcDev 'usr\lib\x86_64-linux-gnu'
$hostRunLib = Join-Path $glibcRun 'lib\x86_64-linux-gnu'
$hostRoot = Join-Path $phase11Root 'phase11-userspace'
$hostObject = Join-Path $hostRoot 'p11-triangle.o'
$hostBinary = Join-Path $hostRoot 'p11-triangle'
foreach ($required in @(
        (Join-Path $hostDevInclude 'stdint.h'),
        (Join-Path $hostDevInclude 'string.h'),
        (Join-Path $hostDevInclude 'unistd.h'),
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
    '-I' + (Join-Path $repoRoot 'include').Replace('\', '/'))
$testSource = Join-Path $repoRoot 'tools\nvidia\phase11_triangle_test.c'
Require-File $testSource 'Phase 11 triangle probe source'
Invoke-CompileIfChanged $testSource $hostObject $hostFlags

$hostLinkArgs = @(
    '-nostdlib', '-nodefaultlibs', '-nostartfiles',
    '-Wl,--build-id=none',
    '-Wl,-dynamic-linker,/usr/apps/p11-triangle/lib/ld-linux-x86-64.so.2',
    '-Wl,-rpath,/usr/apps/p11-triangle/lib',
    '-Wl,--disable-new-dtags',
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
    $hostLinkText += [Environment]::NewLine + $input + [Environment]::NewLine +
                     (Get-FileHash -LiteralPath $input -Algorithm SHA256).Hash
}
if (-not ((Test-Path -LiteralPath $hostBinary -PathType Leaf) -and
          (Test-Path -LiteralPath $hostLinkSignature -PathType Leaf) -and
          [IO.File]::ReadAllText($hostLinkSignature) -eq $hostLinkText)) {
    New-Item -ItemType Directory -Force -Path $hostRoot | Out-Null
    Invoke-Native $compilerPath $hostLinkArgs
    [IO.File]::WriteAllText($hostLinkSignature, $hostLinkText,
                            [Text.Encoding]::ASCII)
    Write-Output 'P11_HOST_LINK=PASS'
}
else { Write-Output 'P11_HOST_LINK=INCREMENTAL_SKIP' }

& powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
    (Join-Path $repoRoot 'tools\nvidia\phase11_stage.ps1') `
    -Build build/nvidia/phase11 `
    -TestBinary build/nvidia/phase11/phase11-userspace/p11-triangle
if ($LASTEXITCODE -ne 0) { throw "P11 staging failed ($LASTEXITCODE)." }

Write-Output ('P11_KERNEL={0}' -f $phase11Kernel)
Write-Output ('P11_ESP={0}' -f (Join-Path $phase11Root 'esp'))
Write-Output ('P11_LINK_OBJECTS={0}' -f $linkRows.Count)
Write-Output 'P11_BUILD=PASS'
