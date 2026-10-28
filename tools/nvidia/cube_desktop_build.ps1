[CmdletBinding()]
param(
    [string]$Compiler = 'x86_64-elf-gcc',
    [string]$Linker = 'x86_64-elf-ld',
    [string]$Make = 'make.exe',
    [string]$GlibcRoot = 'build-phase7/host-deps/glibc',
    [string]$P11RealTestLog = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'app-layout.ps1')
. (Join-Path $PSScriptRoot 'object-paths.ps1')
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$phase12Root = Join-Path $repoRoot 'build\nvidia\phase12'
$phase12Build = Join-Path $repoRoot 'tools\nvidia\phase12_build.ps1'
$phase12Esp = Join-Path $phase12Root 'esp'
$phase12LinkList = Join-Path $phase12Root 'phase12_link_objects.list'
$desktopRoot = Join-Path $repoRoot 'build\nvidia\cube-desktop'
$desktopSystemRoot = Join-Path $desktopRoot 'system'
$desktopKernelRoot = Join-Path $desktopSystemRoot 'kernel'
$desktopKernel = Join-Path $desktopRoot 'kernel.elf'
$desktopLinkList = Join-Path $desktopRoot 'cube-desktop_link_objects.list'
$desktopEsp = Join-Path $desktopRoot 'esp'
$desktopKernelBuildId = Join-Path $desktopEsp 'EFI\OS\kernel.build-id'
$desktopKernelTarget = Join-Path $desktopEsp 'EFI\OS\kernel.elf'
$desktopLoaderConfig = Join-Path $desktopEsp 'EFI\OS\loader.conf'
$cubeSource = Join-Path $repoRoot 'tools\nvidia\opengl_cube_demo.c'
$cubeHostRoot = Join-Path $desktopRoot 'cube-userspace'
$cubeObject = Join-Path $cubeHostRoot 'cube-interactive.o'
$cubeBinary = Join-Path $cubeHostRoot 'cube-interactive'
$linkerScript = Join-Path $PSScriptRoot 'phase4-kernel.ld'
$buildIdTool = Join-Path $repoRoot 'build\build-id.exe'

function Resolve-RepoPath([string]$Path) {
    if ([IO.Path]::IsPathRooted($Path)) { return [IO.Path]::GetFullPath($Path) }
    return [IO.Path]::GetFullPath((Join-Path $repoRoot $Path))
}

function Require-File([string]$Path, [string]$Description) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Missing $Description`: $Path"
    }
}

function Require-Directory([string]$Path, [string]$Description) {
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
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

function Invoke-PowerShellScript([string]$Script, [string[]]$Arguments) {
    $output = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $Script @Arguments 2>&1 |
        ForEach-Object { $_.ToString() })
    $exitCode = $LASTEXITCODE
    foreach ($line in $output) { Write-Output $line }
    if ($exitCode -ne 0) { throw "PowerShell prerequisite failed ($exitCode): $Script" }
}

function Invoke-CompileIfChanged([string]$Source, [string]$Output,
                                 [string[]]$Arguments,
                                 [string[]]$Dependencies = @()) {
    $signaturePath = $Output + '.cmd'
    $signature = $Source + [Environment]::NewLine +
                 (Get-FileHash -LiteralPath $Source -Algorithm SHA256).Hash +
                 [Environment]::NewLine +
                 (($Arguments | ForEach-Object { $_.ToString() }) -join
                  [Environment]::NewLine)
    foreach ($dependency in $Dependencies) {
        $signature += [Environment]::NewLine + $dependency +
                      [Environment]::NewLine +
                      (Get-FileHash -LiteralPath $dependency -Algorithm SHA256).Hash
    }
    if ((Test-Path -LiteralPath $Output -PathType Leaf) -and
        (Test-Path -LiteralPath $signaturePath -PathType Leaf) -and
        [IO.File]::ReadAllText($signaturePath) -eq $signature) {
        Write-Output ('DESKTOP_INCREMENTAL_SKIP={0}' -f $Source)
        return
    }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Output) |
        Out-Null
    Invoke-Native $Compiler (@($Arguments) + @(
        '-c', $Source.Replace('\', '/'), '-o', $Output.Replace('\', '/')))
    [IO.File]::WriteAllText($signaturePath, $signature, [Text.Encoding]::ASCII)
    Write-Output ('DESKTOP_COMPILE={0}' -f $Source)
}

function Copy-FileIfChanged([string]$Source, [string]$Target) {
    $same = $false
    if (Test-Path -LiteralPath $Target -PathType Leaf) {
        $same = (Get-FileHash -LiteralPath $Source -Algorithm SHA256).Hash -eq
                (Get-FileHash -LiteralPath $Target -Algorithm SHA256).Hash
    }
    if ($same) {
        $script:stageSkipped++
        return
    }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Target) |
        Out-Null
    Copy-Item -LiteralPath $Source -Destination $Target -Force
    $script:stageCopied++
}

function Write-TextIfChanged([string]$Path, [string]$Text) {
    if ((Test-Path -LiteralPath $Path -PathType Leaf) -and
        [IO.File]::ReadAllText($Path) -eq $Text) {
        $script:stageSkipped++
        return
    }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Path) |
        Out-Null
    [IO.File]::WriteAllText($Path, $Text, [Text.Encoding]::ASCII)
    $script:stageCopied++
}

function Copy-DirectoryContents([string]$SourceRoot, [string]$TargetRoot,
                                [System.Collections.Generic.HashSet[string]]$Skip) {
    Assert-AppGuestLayout $TargetRoot
    Require-Directory $SourceRoot 'Phase 12 source ESP'
    foreach ($source in @(Get-ChildItem -LiteralPath $SourceRoot -Recurse -File)) {
        $relative = $source.FullName.Substring($SourceRoot.Length).TrimStart('\', '/')
        if (Test-LegacyGuestPath $relative) { continue }
        $normalized = $relative.Replace('\', '/').ToLowerInvariant()
        if ($Skip.Contains($normalized)) { continue }
        Copy-FileIfChanged $source.FullName (Join-Path $TargetRoot $relative)
    }
}

Require-File $phase12Build 'incremental Phase 12 builder'
Require-File $cubeSource 'OpenGL cube demo source'
Require-File $linkerScript 'kernel linker script'
Require-File $buildIdTool 'kernel build-id tool'

$phase12Arguments = @(
    '-Compiler', $Compiler,
    '-Linker', $Linker,
    '-Make', $Make,
    '-GlibcRoot', $GlibcRoot)
if (-not [string]::IsNullOrWhiteSpace($P11RealTestLog)) {
    $phase12Arguments += @('-P11RealTestLog', $P11RealTestLog)
}
Invoke-PowerShellScript $phase12Build $phase12Arguments
Require-Directory $phase12Esp 'built Phase 12 ESP'
Require-File $phase12LinkList 'Phase 12 kernel link list'

$compilerPath = (Get-Command $Compiler -ErrorAction Stop).Source
$linkerPath = (Get-Command $Linker -ErrorAction Stop).Source
$makePath = (Get-Command $Make -ErrorAction Stop).Source

# Only these objects are rebuilt with OS_REALTEST=0.  Everything else,
# including the immutable 595.84 vendor objects, is reused from Phase 12.
$desktopObjectNames = @(
    'context.o',
    'entry.o',
    'runtime-init.o',
    'realtest.o',
    'sched-core.o',
    'debug-stage.o',
    'gpu-core.o',
    'display-core.o',
    'nvidia-port-mmio.o',
    'linux-abi.o',
    'graphics-window.o',
    'graphics-server.o',
    'graphics-damage.o',
    'graphics-present.o',
    'graphics-present-cursor.o',
    'graphics-publication-policy.o',
    'graphics-input-drag.o',
    'graphics-input-pump.o',
    'graphics-compositor.o',
    'graphics-input-router.o',
    'xhci-hid-runtime.o')
$desktopObjectMap = @{}
$desktopTargets = @()
foreach ($name in $desktopObjectNames) {
    $target = Join-Path $desktopKernelRoot $name
    $desktopObjectMap[$name] = $target.Replace('\', '/')
    $desktopTargets += ('build/nvidia/cube-desktop/system/kernel/{0}' -f $name)
}

$makeArguments = @(
    '-f', 'GNUmakefile',
    'BUILD=build/nvidia/cube-desktop/system',
    'TOOLPREFIX=x86_64-elf-',
    'ELFTOOLPREFIX=x86_64-elf-',
    'HOSTCC=gcc',
    'DEBUG=2',
    'OS_DEBUG_SERIAL=1',
    'OS_REALTEST=0',
    'OS_REALTEST_FAILURE_TEST=0',
    'OS_PHASE7=0',
    'OS_PHASE8=0',
    'OS_PHASE9=0',
    'OS_PHASE10=0',
    'KERNEL_EXTRA_CFLAGS=-DOS_NVIDIA_PHASE5=1 -DOS_NVIDIA_PHASE6=0 -DOS_PHASE12=1 -mcmodel=kernel') +
    $desktopTargets
Invoke-Native $makePath $makeArguments

$linkRows = New-Object 'System.Collections.Generic.List[string]'
$seenDesktopObjects = New-Object 'System.Collections.Generic.HashSet[string]'
foreach ($row in @(Get-Content -LiteralPath $phase12LinkList)) {
    $row = ConvertTo-ObjectListPath $row
    if (-not (Test-ObjectListData $row)) {
        [void]$linkRows.Add($row)
        continue
    }
    if ($row -match '(?i)[\/]build/nvidia/phase12/system/kernel/([^/]+\.o)$') {
        $name = $Matches[1]
        if ($desktopObjectMap.ContainsKey($name)) {
            [void]$linkRows.Add($desktopObjectMap[$name])
            [void]$seenDesktopObjects.Add($name)
        }
        else { [void]$linkRows.Add($row) }
    }
    else { [void]$linkRows.Add($row) }
}
foreach ($name in $desktopObjectNames) {
    if (-not $seenDesktopObjects.Contains($name)) {
        throw "Phase 12 link list does not contain required kernel object: $name"
    }
}
New-Item -ItemType Directory -Force -Path $desktopRoot | Out-Null
[IO.File]::WriteAllLines($desktopLinkList, [string[]]$linkRows,
                          [Text.Encoding]::ASCII)

$linkArguments = @(
    '--gc-sections', '--allow-multiple-definition', '-T',
    $linkerScript.Replace('\', '/'), '-o', $desktopKernel.Replace('\', '/'),
    ('@' + $desktopLinkList.Replace('\', '/')))
$linkSignaturePath = $desktopKernel + '.cmd'
$linkSignature = ($linkArguments | ForEach-Object { $_.ToString() }) -join
                 [Environment]::NewLine
$linkSignature += [Environment]::NewLine +
                  (Get-FileHash -LiteralPath $linkerScript -Algorithm SHA256).Hash
foreach ($row in @($linkRows | Where-Object { $_ -and $_.Trim() -and $_[0] -ne '#' })) {
    Require-File $row 'kernel link input'
    $linkSignature += [Environment]::NewLine + $row + [Environment]::NewLine +
                      (Get-FileHash -LiteralPath $row -Algorithm SHA256).Hash
}
if (-not ((Test-Path -LiteralPath $desktopKernel -PathType Leaf) -and
          (Test-Path -LiteralPath $linkSignaturePath -PathType Leaf) -and
          [IO.File]::ReadAllText($linkSignaturePath) -eq $linkSignature)) {
    Invoke-Native $linkerPath $linkArguments
    [IO.File]::WriteAllText($linkSignaturePath, $linkSignature,
                            [Text.Encoding]::ASCII)
    Write-Output 'DESKTOP_KERNEL_LINK=PASS'
}
else { Write-Output 'DESKTOP_KERNEL_LINK=INCREMENTAL_SKIP' }
Require-File $desktopKernel 'normal desktop kernel'

$nm = Get-Command x86_64-elf-nm -ErrorAction SilentlyContinue
if ($null -eq $nm) { $nm = Get-Command nm -ErrorAction Stop }
$undefined = @(& $nm.Source '-u' '-P' '--' $desktopKernel 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0) { throw 'normal desktop kernel undefined-symbol scan failed' }
$unexpected = @($undefined | Where-Object { $_.Trim() })
if ($unexpected.Count -ne 0) {
    throw ('normal desktop kernel has unresolved symbols: {0}' -f $unexpected[0])
}
Write-Output 'DESKTOP_KERNEL_UNDEFINED=PASS'

$kernelBuildIdSignaturePath = $desktopKernelBuildId + '.cmd'
$kernelBuildIdSignature =
    (Get-FileHash -LiteralPath $desktopKernel -Algorithm SHA256).Hash +
    [Environment]::NewLine +
    (Get-FileHash -LiteralPath $buildIdTool -Algorithm SHA256).Hash
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $desktopKernelBuildId) |
    Out-Null
if (-not ((Test-Path -LiteralPath $desktopKernelBuildId -PathType Leaf) -and
          (Test-Path -LiteralPath $kernelBuildIdSignaturePath -PathType Leaf) -and
          [IO.File]::ReadAllText($kernelBuildIdSignaturePath) -eq
          $kernelBuildIdSignature)) {
    Invoke-Native $buildIdTool @($desktopKernel, $desktopKernelBuildId)
    [IO.File]::WriteAllText($kernelBuildIdSignaturePath,
                            $kernelBuildIdSignature, [Text.Encoding]::ASCII)
    Write-Output 'DESKTOP_KERNEL_BUILD_ID=PASS'
}
else { Write-Output 'DESKTOP_KERNEL_BUILD_ID=INCREMENTAL_SKIP' }

$glibcRootPath = (Resolve-Path $GlibcRoot).Path
$glibcDev = Join-Path $glibcRootPath 'libc6-dev-extract'
$glibcRun = Join-Path $glibcRootPath 'libc6-extract'
$hostDevInclude = Join-Path $glibcDev 'usr\include'
$hostArchInclude = Join-Path $hostDevInclude 'x86_64-linux-gnu'
$hostDevLib = Join-Path $glibcDev 'usr\lib\x86_64-linux-gnu'
$hostRunLib = Join-Path $glibcRun 'lib\x86_64-linux-gnu'
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
    '-DCUBE_INTERACTIVE=1',
    '-isystem', $hostDevInclude.Replace('\', '/'),
    '-isystem', $hostArchInclude.Replace('\', '/'),
    '-I' + (Join-Path $repoRoot 'include').Replace('\', '/'))
$headerDependencies = @(
    (Join-Path $repoRoot 'include\uapi\linux_abi.h'),
    (Join-Path $repoRoot 'include\uapi\display.h'),
    (Join-Path $repoRoot 'include\uapi\window.h'),
    (Join-Path $repoRoot 'include\uapi\input.h'),
    (Join-Path $repoRoot 'include\uapi\abi.h'))
foreach ($header in $headerDependencies) { Require-File $header 'cube demo ABI header' }
Invoke-CompileIfChanged $cubeSource $cubeObject $hostFlags $headerDependencies

$hostLinkArguments = @(
    '-nostdlib', '-nodefaultlibs', '-nostartfiles',
    '-Wl,--build-id=none',
    '-Wl,-dynamic-linker,/usr/apps/cube/lib/ld-linux-x86-64.so.2',
    '-Wl,-rpath,/usr/apps/cube/lib',
    '-Wl,--disable-new-dtags',
    '-Wl,--allow-shlib-undefined',
    ('-Wl,-rpath-link,{0}' -f $hostRunLib.Replace('\', '/')),
    '-Wl,-z,now',
    (Join-Path $hostDevLib 'crt1.o').Replace('\', '/'),
    (Join-Path $hostDevLib 'crti.o').Replace('\', '/'),
    $cubeObject.Replace('\', '/'),
    (Join-Path $hostRunLib 'libc.so.6').Replace('\', '/'),
    (Join-Path $hostDevLib 'libc_nonshared.a').Replace('\', '/'),
    (Join-Path $hostDevLib 'crtn.o').Replace('\', '/'),
    '-o', $cubeBinary.Replace('\', '/'))
$cubeLinkSignaturePath = $cubeBinary + '.cmd'
$cubeLinkSignature = ($hostLinkArguments |
    ForEach-Object { $_.ToString() }) -join [Environment]::NewLine
foreach ($input in @(
        $cubeObject,
        (Join-Path $hostDevLib 'crt1.o'),
        (Join-Path $hostDevLib 'crti.o'),
        (Join-Path $hostRunLib 'libc.so.6'),
        (Join-Path $hostRunLib 'ld-linux-x86-64.so.2'),
        (Join-Path $hostDevLib 'libc_nonshared.a'),
        (Join-Path $hostDevLib 'crtn.o'))) {
    $cubeLinkSignature += [Environment]::NewLine + $input +
                           [Environment]::NewLine +
                           (Get-FileHash -LiteralPath $input -Algorithm SHA256).Hash
}
if (-not ((Test-Path -LiteralPath $cubeBinary -PathType Leaf) -and
          (Test-Path -LiteralPath $cubeLinkSignaturePath -PathType Leaf) -and
          [IO.File]::ReadAllText($cubeLinkSignaturePath) -eq $cubeLinkSignature)) {
    New-Item -ItemType Directory -Force -Path $cubeHostRoot | Out-Null
    Invoke-Native $compilerPath $hostLinkArguments
    [IO.File]::WriteAllText($cubeLinkSignaturePath, $cubeLinkSignature,
                            [Text.Encoding]::ASCII)
    Write-Output 'DESKTOP_CUBE_HOST_LINK=PASS'
}
else { Write-Output 'DESKTOP_CUBE_HOST_LINK=INCREMENTAL_SKIP' }
Require-File $cubeBinary 'interactive OpenGL cube binary'
Assert-AppInterpreter $cubeBinary 'cube'

$script:stageCopied = 0
$script:stageSkipped = 0
$skip = New-Object 'System.Collections.Generic.HashSet[string]'
foreach ($relative in @(
        'efi/system/kernel.elf',
        'efi/system/kernel.build-id',
        'efi/system/kernel.build-id.cmd',
        'efi/system/loader.conf',
        'usr/apps/cube/cube')) {
    [void]$skip.Add($relative)
}
Copy-DirectoryContents $phase12Esp $desktopEsp $skip
Install-NvidiaApp (Join-Path $repoRoot 'build\nvidia\userspace') $desktopEsp 'cube'
Copy-FileIfChanged $desktopKernel $desktopKernelTarget
Copy-FileIfChanged $desktopKernelBuildId (Join-Path $desktopEsp 'EFI\OS\kernel.build-id')
Write-TextIfChanged $desktopLoaderConfig "kernel=\EFI\OS\kernel.elf`ncmdline=console=framebuffer loglevel=info`n"
Copy-FileIfChanged $cubeBinary (Join-Path $desktopEsp 'usr\apps\cube\cube')

foreach ($required in @(
        (Join-Path $desktopEsp 'EFI\BOOT\BOOTX64.EFI'),
        $desktopKernelTarget,
        (Join-Path $desktopEsp 'EFI\OS\kernel.build-id'),
        $desktopLoaderConfig,
        (Join-Path $desktopEsp 'EFI\OS\firmware\gsp_ga10x.bin'),
        (Join-Path $desktopEsp 'usr\apps\gshell\gshell'),
        (Join-Path $desktopEsp 'usr\apps\cube\cube'),
        (Join-Path $desktopEsp 'usr\apps\cube\lib\ld-linux-x86-64.so.2'),
        (Join-Path $desktopEsp 'usr\apps\cube\lib\libc.so.6'),
        (Join-Path $desktopEsp 'usr\apps\cube\lib\libEGL.so.1'),
        (Join-Path $desktopEsp 'usr\apps\cube\lib\libEGL_nvidia.so.0'))) {
    Require-File $required 'normal desktop ESP asset'
}

Write-Output ('DESKTOP_STAGE_COPIED={0}' -f $script:stageCopied)
Write-Output ('DESKTOP_STAGE_SKIPPED={0}' -f $script:stageSkipped)
Write-Output ('DESKTOP_KERNEL={0}' -f $desktopKernel)
Write-Output ('DESKTOP_CUBE={0}' -f $cubeBinary)
Write-Output ('DESKTOP_ESP={0}' -f $desktopEsp)
Write-Output 'CUBE_DESKTOP_AUTOTEST=DISABLED'
Write-Output 'CUBE_DESKTOP_BUILD=PASS'
