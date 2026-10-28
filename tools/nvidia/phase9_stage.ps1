[CmdletBinding()]
param(
    [string]$Build = 'build/nvidia/phase9',
    [string]$GlibcRoot = 'build-phase7/host-deps/glibc',
    [string]$TestBinary = 'build/nvidia/phase9/phase9-userspace/p9-userspace-load',
    [string]$UserspaceRoot = 'build/nvidia/userspace'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'app-layout.ps1')
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

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

function Copy-DirectoryContents([string]$SourceRoot, [string]$TargetRoot) {
    Assert-AppGuestLayout $TargetRoot
    Require-Directory $SourceRoot 'Phase 9 source tree'
    foreach ($source in @(Get-ChildItem -LiteralPath $SourceRoot -Recurse -File)) {
        $relative = $source.FullName.Substring($SourceRoot.Length).TrimStart('\', '/')
        if (Test-LegacyGuestPath $relative) { continue }
        Copy-FileIfChanged $source.FullName (Join-Path $TargetRoot $relative)
    }
}

$buildRoot = Resolve-RepoPath $Build
$phase6Esp = Join-Path $repoRoot 'build\nvidia\phase6\esp'
$phase9Kernel = Join-Path $buildRoot 'kernel.elf'
$testBinaryPath = Resolve-RepoPath $TestBinary
$userspacePath = Resolve-RepoPath $UserspaceRoot
$espRoot = Join-Path $buildRoot 'esp'
Assert-AppGuestLayout $espRoot
Assert-AppInterpreter $testBinaryPath 'p9-userspace-load'

Require-Directory $phase6Esp 'Phase 6 ESP'
Require-File $phase9Kernel 'Phase 9 kernel image'
Require-File $testBinaryPath 'Phase 9 loader probe'
Require-Directory $userspacePath 'imported NVIDIA userspace'
$script:stageCopied = 0
$script:stageSkipped = 0
Copy-DirectoryContents $phase6Esp $espRoot
Install-NvidiaApp $userspacePath $espRoot 'p9-userspace-load'
Copy-FileIfChanged $testBinaryPath (Join-Path $espRoot 'tests\p9-userspace-load')
Copy-FileIfChanged $testBinaryPath (Join-Path $espRoot 'usr\apps\p9-userspace-load\p9-userspace-load')
Copy-FileIfChanged $phase9Kernel (Join-Path $espRoot 'EFI\OS\kernel.elf')

Write-Output ('P9_STAGE_COPIED={0}' -f $script:stageCopied)
Write-Output ('P9_STAGE_SKIPPED={0}' -f $script:stageSkipped)
Write-Output ('P9_ESP={0}' -f $espRoot)
Write-Output 'P9_STAGE=PASS'
