[CmdletBinding()]
param(
    [string]$Build = 'build/nvidia/phase-w3',
    [string]$BaseEsp = 'build/nvidia/phase9/esp'
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
    if (Test-Path -LiteralPath $Target -PathType Leaf) {
        if ((Get-FileHash -LiteralPath $Source -Algorithm SHA256).Hash -eq
            (Get-FileHash -LiteralPath $Target -Algorithm SHA256).Hash) {
            $script:stageSkipped++
            return
        }
    }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Target) |
        Out-Null
    Copy-Item -LiteralPath $Source -Destination $Target -Force
    $script:stageCopied++
}

function Copy-DirectoryContents([string]$SourceRoot, [string]$TargetRoot) {
    Assert-AppGuestLayout $TargetRoot
    Require-Directory $SourceRoot 'base NVIDIA ESP'
    foreach ($source in @(Get-ChildItem -LiteralPath $SourceRoot -Recurse -File)) {
        $relative = $source.FullName.Substring($SourceRoot.Length).TrimStart('\', '/')
        if (Test-LegacyGuestPath $relative) { continue }
        Copy-FileIfChanged $source.FullName (Join-Path $TargetRoot $relative)
    }
}

$buildRoot = Resolve-RepoPath $Build
$baseRoot = Resolve-RepoPath $BaseEsp
$kernel = Join-Path $buildRoot 'kernel.elf'
$espRoot = Join-Path $buildRoot 'esp'

Require-File $kernel 'W3 kernel image'
foreach ($relative in @(
        'EFI\BOOT\BOOTX64.EFI',
        'EFI\OS\loader.conf',
        'EFI\OS\firmware\gsp_ga10x.bin')) {
    Require-File (Join-Path $baseRoot $relative) "base ESP asset $relative"
}

$script:stageCopied = 0
$script:stageSkipped = 0
Copy-DirectoryContents $baseRoot $espRoot
Copy-FileIfChanged $kernel (Join-Path $espRoot 'EFI\OS\kernel.elf')

foreach ($relative in @(
        'EFI\BOOT\BOOTX64.EFI',
        'EFI\OS\kernel.elf',
        'EFI\OS\loader.conf',
        'EFI\OS\firmware\gsp_ga10x.bin')) {
    Require-File (Join-Path $espRoot $relative) "staged W3 asset $relative"
}

$loaderText = [IO.File]::ReadAllText((Join-Path $espRoot 'EFI\OS\loader.conf'))
if ($loaderText -notmatch '(?im)^cmdline=.*\brealtest=1\b') {
    throw 'W3 real-test ESP must use cmdline=realtest=1.'
}

Write-Output ('W3_STAGE_COPIED={0}' -f $script:stageCopied)
Write-Output ('W3_STAGE_SKIPPED={0}' -f $script:stageSkipped)
Write-Output ('W3_ESP={0}' -f $espRoot)
Write-Output 'W3_STAGE=PASS'
