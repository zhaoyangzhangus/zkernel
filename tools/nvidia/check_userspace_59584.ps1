[CmdletBinding()]
param(
    [string]$Root = 'build/nvidia/userspace',
    [string]$Manifest = 'build/nvidia/userspace-595.84.json',
    [string]$PackageRoot = '',
    [string]$AppRoot = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

function Resolve-RepoPath([string]$Path) {
    if ([IO.Path]::IsPathRooted($Path)) { return [IO.Path]::GetFullPath($Path) }
    return [IO.Path]::GetFullPath((Join-Path $repoRoot $Path))
}

function Fail-Check([string]$Message) { throw "595.84 userspace check failed: $Message" }

function Hash-File([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToUpperInvariant()
}

function Invoke-Readelf([string]$Tool, [string[]]$Arguments) {
    $output = @(& $Tool @Arguments 2>&1 | ForEach-Object { $_.ToString() })
    if ($LASTEXITCODE -ne 0) { Fail-Check "readelf failed for $($Arguments[-1])" }
    return $output
}

# App deployment has a different topology from the rename-only host cache.
# Use the shared checker for manifest remapping, hashes, ELF and symbol closure.
if (-not [string]::IsNullOrWhiteSpace($AppRoot)) {
    $python = Get-Command python3 -ErrorAction SilentlyContinue
    if ($null -eq $python) { $python = Get-Command python -ErrorAction Stop }
    $checkArguments = @((Join-Path $PSScriptRoot 'check_userspace_59584.py'),
        '--app-root', (Resolve-RepoPath $AppRoot),
        '--manifest', (Resolve-RepoPath $Manifest))
    if (-not [string]::IsNullOrWhiteSpace($PackageRoot)) {
        $checkArguments += @('--package-root', (Resolve-RepoPath $PackageRoot))
    }
    & $python.Source @checkArguments
    if ($LASTEXITCODE -ne 0) { Fail-Check 'app-private userspace closure failed' }
    return
}

$root = Resolve-RepoPath $Root
$manifestPath = Resolve-RepoPath $Manifest
if (-not (Test-Path -LiteralPath $root -PathType Container)) { Fail-Check "missing root $root" }
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) { Fail-Check "missing manifest $manifestPath" }
$manifestObject = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ($manifestObject.release -ne '595.84') { Fail-Check 'manifest release is not 595.84' }
$packageRootPath = if ([string]::IsNullOrWhiteSpace($PackageRoot)) { $null } else { Resolve-RepoPath $PackageRoot }

foreach ($entry in @($manifestObject.files)) {
    $relative = ([string]$entry.target).Replace('/', '\')
    $path = Join-Path $root $relative
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { Fail-Check "missing $relative" }
    if ((Hash-File $path) -ne ([string]$entry.sha256).ToUpperInvariant()) {
        Fail-Check "hash mismatch $relative"
    }
    if ((Get-Item -LiteralPath $path).Length -ne [int64]$entry.size) {
        Fail-Check "size mismatch $relative"
    }
    if ($packageRootPath -and $entry.source_kind -eq 'nvidia') {
        $source = Join-Path $packageRootPath (([string]$entry.source).Replace('/', '\'))
        if (-not (Test-Path -LiteralPath $source -PathType Leaf) -or
            (Hash-File $source) -ne (Hash-File $path)) {
            Fail-Check "NVIDIA source mismatch $($entry.source)"
        }
    }
}
foreach ($entry in @($manifestObject.aliases)) {
    $alias = Join-Path $root (([string]$entry.alias).Replace('/', '\'))
    $target = Join-Path $root (([string]$entry.target).Replace('/', '\'))
    if (-not (Test-Path -LiteralPath $alias -PathType Leaf) -or
        -not (Test-Path -LiteralPath $target -PathType Leaf)) {
        Fail-Check "missing alias $($entry.alias)"
    }
    if ((Hash-File $alias) -ne (Hash-File $target)) {
        Fail-Check "alias content mismatch $($entry.alias)"
    }
}
$requiredAliases = @(
    'lib/x86_64-linux-gnu/libEGL.so.1',
    'lib/x86_64-linux-gnu/libEGL.so',
    'lib/x86_64-linux-gnu/libEGL_nvidia.so.0',
    'lib/x86_64-linux-gnu/libOpenGL.so',
    'lib/x86_64-linux-gnu/libnvidia-allocator.so.1',
    'lib/x86_64-linux-gnu/libnvidia-allocator.so'
)
$aliasNames = @($manifestObject.aliases | ForEach-Object { [string]$_.alias })
foreach ($required in $requiredAliases) {
    if ($aliasNames -notcontains $required) { Fail-Check "missing required alias $required" }
}

$readelfCommand = Get-Command x86_64-elf-readelf -ErrorAction SilentlyContinue
if ($null -eq $readelfCommand) { $readelfCommand = Get-Command readelf -ErrorAction Stop }
$elfFiles = New-Object 'System.Collections.Generic.List[object]'
$elfNames = New-Object 'System.Collections.Generic.HashSet[string]'
foreach ($file in @(Get-ChildItem -LiteralPath $root -Recurse -File)) {
    $magic = @(Get-Content -LiteralPath $file.FullName -Encoding Byte -TotalCount 4)
    if ($magic.Count -ne 4 -or $magic[0] -ne 0x7f -or $magic[1] -ne 0x45 -or
        $magic[2] -ne 0x4c -or $magic[3] -ne 0x46) { continue }
    $header = Invoke-Readelf $readelfCommand.Source @('-hW', '--', $file.FullName)
    if (@($header | Where-Object { $_ -match 'Class:\s+ELF64' }).Count -ne 1 -or
        @($header | Where-Object { $_ -match 'Machine:\s+.*X86-64' }).Count -ne 1 -or
        @($header | Where-Object { $_ -match 'Type:\s+(DYN|EXEC)' }).Count -ne 1) {
        Fail-Check "non-x86-64 ELF $($file.FullName)"
    }
    [void]$elfFiles.Add($file)
    [void]$elfNames.Add($file.Name)
}
if ($elfFiles.Count -eq 0) { Fail-Check 'no ELF files in userspace tree' }

$allowedRelocations = @(
    'R_X86_64_64', 'R_X86_64_DTPMOD64', 'R_X86_64_GLOB_DAT',
    'R_X86_64_IRELATIVE', 'R_X86_64_JUMP_SLOT', 'R_X86_64_RELATIVE',
    'R_X86_64_TPOFF64', 'R_X86_64_NONE', 'R_X86_64_PC32',
    'R_X86_64_PLT32', 'R_X86_64_GOTPCREL', 'R_X86_64_GOTPCRELX',
    'R_X86_64_REX_GOTPCRELX', 'R_X86_64_32', 'R_X86_64_32S',
    'R_X86_64_SIZE32', 'R_X86_64_SIZE64'
)
$unresolved = New-Object 'System.Collections.Generic.List[string]'
$unknownRelocations = New-Object 'System.Collections.Generic.List[string]'
foreach ($file in $elfFiles) {
    $dynamic = Invoke-Readelf $readelfCommand.Source @('-dW', '--', $file.FullName)
    foreach ($line in $dynamic) {
        $match = [regex]::Match($line, 'Shared library:\s+\[([^]]+)\]')
        if ($match.Success -and -not $elfNames.Contains($match.Groups[1].Value) -and
            $match.Groups[1].Value -ne 'linux-vdso.so.1') {
            [void]$unresolved.Add("$($file.Name) -> $($match.Groups[1].Value)")
        }
    }
    $relocations = Invoke-Readelf $readelfCommand.Source @('-rW', '--', $file.FullName)
    foreach ($line in $relocations) {
        foreach ($match in [regex]::Matches($line, '\bR_[A-Z0-9_]+\b')) {
            if ($allowedRelocations -notcontains $match.Value) {
                [void]$unknownRelocations.Add("$($file.Name) -> $($match.Value)")
            }
        }
    }
}
if ($unresolved.Count -ne 0) { Fail-Check ('unresolved DT_NEEDED: ' + $unresolved[0]) }
if ($unknownRelocations.Count -ne 0) {
    Fail-Check ('unknown relocation: ' + $unknownRelocations[0])
}

Write-Output 'RELEASE_CONSISTENCY=PASS'
Write-Output 'SYMLINK_TOPOLOGY=PASS'
Write-Output ("ELF_X86_64_COUNT={0}" -f $elfFiles.Count)
Write-Output 'DT_NEEDED_GATE=PASS'
Write-Output 'RELOCATION_GATE=PASS'
Write-Output 'CHECK_USERSPACE_59584=PASS'
