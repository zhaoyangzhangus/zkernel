[CmdletBinding()]
param(
    [string]$PackageRoot = 'build/nvidia/official-595.84/extracted-gitbash',
    [string]$PackageRun = 'build/nvidia/official-595.84/NVIDIA-Linux-x86_64-595.84.run',
    [string]$GlibcRoot = 'build-phase7/host-deps/glibc',
    [string]$Output = 'build/nvidia/userspace',
    [string]$Manifest = 'build/nvidia/userspace-595.84.json'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

function Resolve-RepoPath([string]$Path) {
    if ([IO.Path]::IsPathRooted($Path)) {
        return [IO.Path]::GetFullPath($Path)
    }
    return [IO.Path]::GetFullPath((Join-Path $repoRoot $Path))
}

function Require-File([string]$Path, [string]$Description) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Missing $Description`: $Path"
    }
}

function Copy-FileIfChanged([string]$Source, [string]$Target) {
    $same = $false
    if (Test-Path -LiteralPath $Target -PathType Leaf) {
        $same = (Get-FileHash -LiteralPath $Source -Algorithm SHA256).Hash -eq
                (Get-FileHash -LiteralPath $Target -Algorithm SHA256).Hash
    }
    if ($same) {
        $script:copiedSkipped++
        return
    }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Target) |
        Out-Null
    Copy-Item -LiteralPath $Source -Destination $Target -Force
    $script:copiedFiles++
}

function Add-FileEntry([string]$Source, [string]$Target, [string]$Kind,
                       [string]$Root, [System.Collections.ArrayList]$Entries) {
    Require-File $Source "595.84 userspace input"
    $destination = Join-Path $Root ($Target.Replace('/', '\'))
    Copy-FileIfChanged $Source $destination
    [void]$Entries.Add([PSCustomObject]@{
        source = (Split-Path -Leaf $Source)
        source_kind = $Kind
        target = $Target.Replace('\', '/')
        size = (Get-Item -LiteralPath $Source).Length
        sha256 = (Get-FileHash -LiteralPath $Source -Algorithm SHA256).Hash
    })
}

function Add-Alias([string]$Alias, [string]$Target, [string]$Root,
                   [System.Collections.ArrayList]$Aliases) {
    $targetPath = Join-Path $Root ($Target.Replace('/', '\'))
    $aliasPath = Join-Path $Root ($Alias.Replace('/', '\'))
    Require-File $targetPath "symlink target $Target"
    Copy-FileIfChanged $targetPath $aliasPath
    [void]$Aliases.Add([PSCustomObject]@{
        alias = $Alias.Replace('\', '/')
        target = $Target.Replace('\', '/')
        materialized_on_fat32 = $true
    })
}

$packageRoot = Resolve-RepoPath $PackageRoot
$packageRunPath = Resolve-RepoPath $PackageRun
$glibcRootPath = Resolve-RepoPath $GlibcRoot
$outputRoot = Resolve-RepoPath $Output
$runtimeRoot = Join-Path $glibcRootPath 'libc6-extract\lib\x86_64-linux-gnu'
$script:copiedFiles = 0
$script:copiedSkipped = 0
Require-File (Join-Path $packageRoot '.manifest') '595.84 package manifest'
Require-File $packageRunPath '595.84 official Linux package'
Require-File ($packageRunPath + '.sha256sum') '595.84 package checksum'

$expectedHash = (Get-Content -LiteralPath ($packageRunPath + '.sha256sum') -Raw) -match
                '(?im)^\s*([0-9a-f]{64})\s+'
if (-not $expectedHash) { throw '595.84 package checksum file is malformed.' }
$actualPackageHash = (Get-FileHash -LiteralPath $packageRunPath -Algorithm SHA256).Hash
if ($actualPackageHash -ne $Matches[1].ToUpperInvariant()) {
    throw '595.84 official package checksum does not match the recorded hash.'
}

$files = New-Object 'System.Collections.ArrayList'
$aliases = New-Object 'System.Collections.ArrayList'
$nvidiaNames = @(
    'libEGL.so.1.1.0',
    'libGLdispatch.so.0',
    'libOpenGL.so.0',
    'libEGL_nvidia.so.595.84',
    'libnvidia-eglcore.so.595.84',
    'libnvidia-glcore.so.595.84',
    'libnvidia-glsi.so.595.84',
    'libnvidia-gpucomp.so.595.84',
    'libnvidia-tls.so.595.84',
    'libnvidia-allocator.so.595.84'
)
foreach ($name in $nvidiaNames) {
    Add-FileEntry (Join-Path $packageRoot $name) `
        ('lib/x86_64-linux-gnu/{0}' -f $name) 'nvidia' $outputRoot $files
}

$glibcNames = @(
    'ld-linux-x86-64.so.2',
    'libc.so.6',
    'libdl.so.2',
    'libm.so.6',
    'libpthread.so.0',
    'librt.so.1'
)
foreach ($name in $glibcNames) {
    Add-FileEntry (Join-Path $runtimeRoot $name) `
        ('lib/x86_64-linux-gnu/{0}' -f $name) 'glibc-p7' $outputRoot $files
}
Add-FileEntry (Join-Path $runtimeRoot 'ld-linux-x86-64.so.2') `
    'lib64/ld-linux-x86-64.so.2' 'glibc-p7' $outputRoot $files

$jsonNames = @(
    '09_nvidia_wayland2.json', '10_nvidia.json', '10_nvidia_wayland.json',
    '15_nvidia_gbm.json', '20_nvidia_xcb.json', '20_nvidia_xlib.json'
)
foreach ($name in $jsonNames) {
    Add-FileEntry (Join-Path $packageRoot $name) `
        ('usr/share/glvnd/egl_vendor.d/{0}' -f $name) 'nvidia' $outputRoot $files
}

Add-Alias 'lib/x86_64-linux-gnu/libEGL.so.1' `
    'lib/x86_64-linux-gnu/libEGL.so.1.1.0' $outputRoot $aliases
Add-Alias 'lib/x86_64-linux-gnu/libEGL.so' `
    'lib/x86_64-linux-gnu/libEGL.so.1' $outputRoot $aliases
Add-Alias 'lib/x86_64-linux-gnu/libOpenGL.so' `
    'lib/x86_64-linux-gnu/libOpenGL.so.0' $outputRoot $aliases
Add-Alias 'lib/x86_64-linux-gnu/libEGL_nvidia.so.0' `
    'lib/x86_64-linux-gnu/libEGL_nvidia.so.595.84' $outputRoot $aliases
Add-Alias 'lib/x86_64-linux-gnu/libnvidia-allocator.so.1' `
    'lib/x86_64-linux-gnu/libnvidia-allocator.so.595.84' $outputRoot $aliases
Add-Alias 'lib/x86_64-linux-gnu/libnvidia-allocator.so' `
    'lib/x86_64-linux-gnu/libnvidia-allocator.so.1' $outputRoot $aliases

$manifestPath = Resolve-RepoPath $Manifest
$manifest = [ordered]@{
    release = '595.84'
    package_run = $packageRunPath.Replace('\', '/')
    package_sha256 = $actualPackageHash
    glibc_baseline = 'build-phase7/host-deps/glibc/libc6-extract'
    files = @($files)
    aliases = @($aliases)
}
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $manifestPath) |
    Out-Null
$manifest | ConvertTo-Json -Depth 8 |
    Set-Content -LiteralPath $manifestPath -Encoding UTF8

Write-Output ('P9_IMPORT_COPIED={0}' -f $script:copiedFiles)
Write-Output ('P9_IMPORT_SKIPPED={0}' -f $script:copiedSkipped)
Write-Output ('P9_USERSPACE_ROOT={0}' -f $outputRoot)
Write-Output ('P9_USERSPACE_MANIFEST={0}' -f $manifestPath)
Write-Output 'P9_IMPORT=PASS'
