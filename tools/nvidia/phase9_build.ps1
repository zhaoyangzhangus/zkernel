[CmdletBinding()]
param(
    [string]$Compiler = 'x86_64-elf-gcc',
    [string]$Linker = 'x86_64-elf-ld',
    [string]$Make = 'make.exe',
    [string]$GlibcRoot = 'build-phase7/host-deps/glibc',
    [string]$PackageRoot = 'build/nvidia/official-595.84/extracted-gitbash',
    [string]$PackageRun = 'build/nvidia/official-595.84/NVIDIA-Linux-x86_64-595.84.run'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'app-layout.ps1')
. (Join-Path $PSScriptRoot 'object-paths.ps1')
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$phase8Root = Join-Path $repoRoot 'build\nvidia\phase8'
$phase8LinkList = Join-Path $phase8Root 'phase8_link_objects.list'
$phase9Root = Join-Path $repoRoot 'build\nvidia\phase9'
$phase9SystemRoot = Join-Path $phase9Root 'system'
$phase9Kernel = Join-Path $phase9Root 'kernel.elf'
$phase9Esp = Join-Path $phase9Root 'esp'
$linkerScript = Join-Path $PSScriptRoot 'phase4-kernel.ld'

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
        Write-Output ('PHASE9_INCREMENTAL_SKIP={0}' -f $Source)
        return
    }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Output) |
        Out-Null
    Invoke-Native $Compiler (@($Arguments) + @(
        '-c', $Source.Replace('\', '/'), '-o', $Output.Replace('\', '/')))
    [IO.File]::WriteAllText($signaturePath, $signature, [Text.Encoding]::ASCII)
    Write-Output ('PHASE9_COMPILE={0}' -f $Source)
}

function Copy-FileIfChanged([string]$Source, [string]$Target) {
    $same = $false
    if (Test-Path -LiteralPath $Target -PathType Leaf) {
        $same = (Get-FileHash -LiteralPath $Source -Algorithm SHA256).Hash -eq
                (Get-FileHash -LiteralPath $Target -Algorithm SHA256).Hash
    }
    if ($same) { $script:stageSkipped++; return }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Target) |
        Out-Null
    Copy-Item -LiteralPath $Source -Destination $Target -Force
    $script:stageCopied++
}

function Copy-DirectoryContents([string]$SourceRoot, [string]$TargetRoot) {
    Assert-AppGuestLayout $TargetRoot
    foreach ($source in @(Get-ChildItem -LiteralPath $SourceRoot -Recurse -File)) {
        $relative = $source.FullName.Substring($SourceRoot.Length).TrimStart('\', '/')
        if (Test-LegacyGuestPath $relative) { continue }
        Copy-FileIfChanged $source.FullName (Join-Path $TargetRoot $relative)
    }
}

Require-File (Join-Path $repoRoot 'tools\nvidia\phase8_build.ps1') `
    'Phase 8 incremental builder'
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
    (Join-Path $repoRoot 'tools\nvidia\phase8_build.ps1') `
    -GlibcRoot $GlibcRoot
if ($LASTEXITCODE -ne 0) { throw "Phase 8 prerequisite build failed ($LASTEXITCODE)." }

$phase8LinkList = Join-Path $phase8Root 'phase8_link_objects.list'
Require-File $phase8LinkList 'Phase 8 link list'
Require-File (Join-Path $phase8Root 'kernel.elf') 'Phase 8 kernel image'
Require-File $linkerScript 'Phase 4 linker script'

$compilerPath = (Get-Command $Compiler -ErrorAction Stop).Source
$linkerPath = (Get-Command $Linker -ErrorAction Stop).Source
$makePath = (Get-Command $Make -ErrorAction Stop).Source
$phase9ObjectRoot = Join-Path $phase9Root 'objects'
$phase9KernelRoot = Join-Path $phase9SystemRoot 'kernel'
New-Item -ItemType Directory -Force -Path $phase9ObjectRoot, $phase9KernelRoot |
    Out-Null

$normalTargets = New-Object 'System.Collections.Generic.List[string]'
$normalLeaves = New-Object 'System.Collections.Generic.HashSet[string]'
foreach ($row in @(Get-Content -LiteralPath $phase8LinkList)) {
    $row = ConvertTo-ObjectListPath $row
    if (-not (Test-ObjectListData $row)) { continue }
    if ($row -match '(?i)/build/nvidia/phase8/system/kernel/([^/]+\.o)$') {
        $leaf = $Matches[1]
        if ($normalLeaves.Add($leaf)) {
            [void]$normalTargets.Add(
                ('build/nvidia/phase9/system/kernel/{0}' -f $leaf))
        }
    }
}
foreach ($leaf in @('runtime-init.o', 'linux-abi.o', 'linux-phase7-test.o',
                    'linux-phase8-test.o', 'linux-phase9-test.o',
                    'nvidia-rm-frontend.o')) {
    if ($normalLeaves.Add($leaf)) {
        [void]$normalTargets.Add(
            ('build/nvidia/phase9/system/kernel/{0}' -f $leaf))
    }
}

$makeArgs = @(
    '-f', 'GNUmakefile',
    'BUILD=build/nvidia/phase9/system',
    'TOOLPREFIX=x86_64-elf-',
    'ELFTOOLPREFIX=x86_64-elf-',
    'HOSTCC=gcc',
    'DEBUG=2',
    'OS_DEBUG_SERIAL=1',
    'OS_REALTEST=1',
    'OS_PHASE7=0',
    'OS_PHASE8=0',
    'OS_PHASE9=1',
    'KERNEL_EXTRA_CFLAGS=-DOS_NVIDIA_PHASE5=1 -DOS_NVIDIA_PHASE6=1 -mcmodel=kernel'
) + @($normalTargets)
Invoke-Native $makePath $makeArgs

$newObjectPaths = @{}
foreach ($leaf in @('runtime-init.o', 'linux-abi.o', 'linux-phase7-test.o',
                    'linux-phase8-test.o', 'linux-phase9-test.o',
                    'nvidia-rm-frontend.o')) {
    $newObjectPaths[$leaf] =
        (Join-Path $phase9KernelRoot $leaf).Replace('\', '/')
}

$linkRows = New-Object 'System.Collections.Generic.List[string]'
foreach ($row in @(Get-Content -LiteralPath $phase8LinkList)) {
    $row = ConvertTo-ObjectListPath $row
    if (-not (Test-ObjectListData $row)) {
        [void]$linkRows.Add($row)
        continue
    }
    if ($row -match '(?i)/build/nvidia/phase8/system/kernel/([^/]+\.o)$') {
        [void]$linkRows.Add(
            (Join-Path $phase9KernelRoot $Matches[1]).Replace('\', '/'))
    }
    else { [void]$linkRows.Add($row) }
}
$p9ObjectPath = $newObjectPaths['linux-phase9-test.o']
if (@($linkRows | Where-Object { $_ -eq $p9ObjectPath }).Count -eq 0) {
    [void]$linkRows.Add($p9ObjectPath)
}
$phase9LinkList = Join-Path $phase9Root 'phase9_link_objects.list'
[IO.File]::WriteAllLines($phase9LinkList, [string[]]$linkRows,
                          [Text.Encoding]::ASCII)

$linkArgs = @(
    '--gc-sections', '--allow-multiple-definition', '-T',
    $linkerScript.Replace('\', '/'), '-o',
    $phase9Kernel.Replace('\', '/'),
    ('@' + $phase9LinkList.Replace('\', '/')))
Invoke-Native $linkerPath $linkArgs

$nm = Get-Command x86_64-elf-nm -ErrorAction SilentlyContinue
if ($null -eq $nm) { $nm = Get-Command nm -ErrorAction Stop }
$undefined = @(& $nm.Source '-u' '-P' '--' $phase9Kernel 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0) { throw 'Phase 9 kernel undefined-symbol scan failed' }
$unexpected = @($undefined | Where-Object { $_.Trim() })
if ($unexpected.Count -ne 0) {
    throw ('Phase 9 kernel has unresolved symbols: {0}' -f $unexpected[0])
}

$glibcRootPath = (Resolve-Path $GlibcRoot).Path
$glibcDev = Join-Path $glibcRootPath 'libc6-dev-extract'
$glibcRun = Join-Path $glibcRootPath 'libc6-extract'
$hostDevInclude = Join-Path $glibcDev 'usr\include'
$hostArchInclude = Join-Path $hostDevInclude 'x86_64-linux-gnu'
$hostDevLib = Join-Path $glibcDev 'usr\lib\x86_64-linux-gnu'
$hostRunLib = Join-Path $glibcRun 'lib\x86_64-linux-gnu'
$hostRoot = Join-Path $phase9Root 'phase9-userspace'
$hostObject = Join-Path $hostRoot 'p9-userspace-load.o'
$hostBinary = Join-Path $hostRoot 'p9-userspace-load'
foreach ($required in @(
        (Join-Path $hostDevInclude 'stdint.h'),
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
    '-I' + (Join-Path $repoRoot 'include').Replace('\', '/')
)
Invoke-CompileIfChanged (Join-Path $repoRoot 'tools\nvidia\phase9_userspace_load_test.c') `
    $hostObject $hostFlags

$hostLinkArgs = @(
    '-nostdlib', '-nodefaultlibs', '-nostartfiles',
    '-Wl,--build-id=none',
    '-Wl,-dynamic-linker,/usr/apps/p9-userspace-load/lib/ld-linux-x86-64.so.2',
    '-Wl,-rpath,/usr/apps/p9-userspace-load/lib',
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
    Write-Output 'P9_HOST_LINK=PASS'
}
else { Write-Output 'P9_HOST_LINK=INCREMENTAL_SKIP' }

& powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
    (Join-Path $repoRoot 'tools\nvidia\import_userspace_59584.ps1') `
    -PackageRoot $PackageRoot -PackageRun $PackageRun -GlibcRoot $GlibcRoot
if ($LASTEXITCODE -ne 0) { throw "P9 userspace import failed ($LASTEXITCODE)." }
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
    (Join-Path $repoRoot 'tools\nvidia\check_userspace_59584.ps1') `
    -Root build/nvidia/userspace `
    -Manifest build/nvidia/userspace-595.84.json `
    -PackageRoot $PackageRoot
if ($LASTEXITCODE -ne 0) { throw "P9 userspace static check failed ($LASTEXITCODE)." }

$script:stageCopied = 0
$script:stageSkipped = 0
Copy-DirectoryContents (Join-Path $repoRoot 'build\nvidia\phase6\esp') $phase9Esp
Copy-FileIfChanged $phase9Kernel (Join-Path $phase9Esp 'EFI\OS\kernel.elf')

& powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
    (Join-Path $repoRoot 'tools\nvidia\phase9_stage.ps1') `
    -Build build/nvidia/phase9 -GlibcRoot $GlibcRoot `
    -TestBinary build/nvidia/phase9/phase9-userspace/p9-userspace-load `
    -UserspaceRoot build/nvidia/userspace
if ($LASTEXITCODE -ne 0) { throw "P9 staging failed ($LASTEXITCODE)." }

Write-Output ('P9_KERNEL={0}' -f $phase9Kernel)
Write-Output ('P9_ESP={0}' -f $phase9Esp)
Write-Output ('P9_LINK_OBJECTS={0}' -f $linkRows.Count)
Write-Output 'P9_BUILD=PASS'
