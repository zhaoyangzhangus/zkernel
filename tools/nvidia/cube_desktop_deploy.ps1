[CmdletBinding()]
param(
    [int]$DiskNumber = 1,
    [int]$PartitionNumber = 2,
    [ValidatePattern('^[A-Za-z]:?$')]
    [string]$DriveLetter = 'F',
    [string]$Build = 'build/nvidia/cube-desktop',
    [switch]$BootOnce,
    [switch]$Reboot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'app-layout.ps1')
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$letter = $DriveLetter.TrimEnd(':').ToUpperInvariant()

if ($letter.Length -ne 1) { throw "DriveLetter must be one letter: $DriveLetter" }
if ($DiskNumber -lt 0 -or $PartitionNumber -lt 1) {
    throw 'DiskNumber and PartitionNumber are invalid.'
}
if ($Reboot -and -not $BootOnce) {
    throw 'Reboot requires BootOnce so the normal desktop image is selected explicitly.'
}
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'This operation must run from an elevated PowerShell window.'
}

function Require-File([string]$Path, [string]$Description) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Missing $Description`: $Path"
    }
}

function Invoke-NativeChecked([string]$FilePath, [string[]]$Arguments) {
    $output = @(& $FilePath @Arguments 2>&1 |
        ForEach-Object { [string]$_ })
    $status = $LASTEXITCODE
    if ($status -ne 0) {
        throw "$FilePath failed ($status): $($output -join ' ')"
    }
    return ($output -join "`r`n")
}

function Copy-FileIfChanged([string]$SourceFile, [string]$TargetFile) {
    $sourceHash = (Get-FileHash -LiteralPath $SourceFile -Algorithm SHA256).Hash
    if (Test-Path -LiteralPath $TargetFile -PathType Leaf) {
        $targetHash = (Get-FileHash -LiteralPath $TargetFile -Algorithm SHA256).Hash
        if ($sourceHash -eq $targetHash) {
            $script:deploymentSkipped++
            return
        }
    }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $TargetFile) |
        Out-Null
    Copy-Item -LiteralPath $SourceFile -Destination $TargetFile -Force
    $script:deploymentCopied++
}

function Copy-DirectoryContents([string]$SourceDirectory,
                                [string]$TargetDirectory) {
    Assert-AppGuestLayout $TargetDirectory
    if (-not (Test-Path -LiteralPath $TargetDirectory -PathType Container)) {
        New-Item -ItemType Directory -Force -Path $TargetDirectory | Out-Null
    }
    foreach ($source in @(Get-ChildItem -LiteralPath $SourceDirectory -Recurse -File)) {
        $relative = $source.FullName.Substring($SourceDirectory.Length).TrimStart('\', '/')
        if (Test-LegacyGuestPath $relative) { continue }
        Copy-FileIfChanged $source.FullName (Join-Path $TargetDirectory $relative)
    }
}

$buildRoot = if ([IO.Path]::IsPathRooted($Build)) {
    [IO.Path]::GetFullPath($Build)
} else {
    [IO.Path]::GetFullPath((Join-Path $projectRoot $Build))
}
$espRoot = Join-Path $buildRoot 'esp'
Assert-AppGuestLayout $espRoot
Assert-AppInterpreter (Join-Path $espRoot 'usr\apps\cube\cube') 'cube'
& (Join-Path $PSScriptRoot 'check_userspace_59584.ps1') -AppRoot (Join-Path $espRoot 'usr\apps\cube')
if (-not $?) { throw 'cube private userspace check failed' }
foreach ($required in @(
        (Join-Path $espRoot 'EFI\BOOT\BOOTX64.EFI'),
        (Join-Path $espRoot 'EFI\OS\kernel.elf'),
        (Join-Path $espRoot 'EFI\OS\loader.conf'),
        (Join-Path $espRoot 'EFI\OS\firmware\gsp_ga10x.bin'),
        (Join-Path $espRoot 'usr\apps\gshell\gshell'),
        (Join-Path $espRoot 'usr\apps\cube\cube'))) {
    Require-File $required 'normal desktop source asset'
}
$loaderText = [IO.File]::ReadAllText((Join-Path $espRoot 'EFI\OS\loader.conf'))
if ($loaderText -match '(?im)^cmdline=.*\brealtest\s*=') {
    throw 'Refusing to deploy a realtest loader configuration as normal desktop.'
}

$disk = Get-Disk -Number $DiskNumber
$partition = Get-Partition -DiskNumber $DiskNumber -PartitionNumber $PartitionNumber
$volume = Get-Volume -DriveLetter $letter
if ($disk.BusType.ToString() -ne 'USB') {
    throw "Refusing non-USB target disk $DiskNumber (BusType=$($disk.BusType))."
}
if ($disk.IsBoot -or $disk.IsSystem) {
    throw "Refusing boot/system disk $DiskNumber."
}
if ($partition.DriveLetter.ToString().ToUpperInvariant() -ne $letter) {
    throw "Partition $DiskNumber/$PartitionNumber is not mounted as $letter`:."
}
if ($volume.FileSystem -ne 'FAT32') {
    throw "Refusing non-FAT32 target volume $letter`: (FileSystem=$($volume.FileSystem))."
}

$volumeRoot = "$letter`:\"
$script:deploymentCopied = 0
$script:deploymentSkipped = 0
Copy-DirectoryContents $espRoot $volumeRoot

$sourceKernel = Join-Path $espRoot 'EFI\OS\kernel.elf'
$targetKernel = Join-Path $volumeRoot 'EFI\OS\kernel.elf'
$sourceLoader = Join-Path $espRoot 'EFI\OS\loader.conf'
$targetLoader = Join-Path $volumeRoot 'EFI\OS\loader.conf'
if ((Get-FileHash -LiteralPath $sourceKernel -Algorithm SHA256).Hash -ne
    (Get-FileHash -LiteralPath $targetKernel -Algorithm SHA256).Hash) {
    throw 'Target kernel hash does not match the normal desktop build.'
}
if ([IO.File]::ReadAllText($targetLoader) -ne $loaderText) {
    throw 'Target loader.conf does not match the normal desktop build.'
}

Write-Output ('DESKTOP_DEPLOY_COPIED={0}' -f $script:deploymentCopied)
Write-Output ('DESKTOP_DEPLOY_SKIPPED={0}' -f $script:deploymentSkipped)
Write-Output ('DESKTOP_DEPLOY_TARGET={0}' -f $volumeRoot)
Write-Output 'DESKTOP_DEPLOY_HASH=PASS'

if ($BootOnce) {
    $firmwareBefore = Invoke-NativeChecked 'bcdedit.exe' @('/enum', 'firmware', '/v')
    $firmwareId = $null
    foreach ($block in ($firmwareBefore -split '(?:\r?\n){2,}')) {
        if ($block -notmatch '(?im)^\s*path\s+.*\\EFI\\BOOT\\BOOTX64\.EFI\s*$' -or
            $block -notmatch "(?im)^\s*device\s+.*$letter`:\s*$") {
            continue
        }
        $match = [regex]::Match($block,
            '(?im)^\s*identifier\s+(\{[^}]+\})\s*$')
        if ($match.Success) {
            if ($firmwareId) { throw "Multiple firmware entries point to $letter`: ." }
            $firmwareId = $match.Groups[1].Value
        }
    }
    if (-not $firmwareId) {
        throw "No UEFI firmware entry for $letter`: \\EFI\\BOOT\\BOOTX64.EFI was found."
    }
    [void](Invoke-NativeChecked 'bcdedit.exe' @(
        '/set', '{fwbootmgr}', 'bootsequence', $firmwareId))
    $firmwareAfter = Invoke-NativeChecked 'bcdedit.exe' @('/enum', '{fwbootmgr}', '/v')
    if ($firmwareAfter -notmatch [regex]::Escape($firmwareId)) {
        throw 'UEFI one-time boot selection could not be verified.'
    }
    Write-Output ('DESKTOP_BOOT_ONCE={0}' -f $firmwareId)
}

if ($Reboot) {
    Write-Output 'DESKTOP_REBOOT=STARTING'
    Write-Output 'System will remain in the normal desktop until you manually restart back to Windows.'
    [void](Invoke-NativeChecked 'shutdown.exe' @(
        '/r', '/f', '/t', '0', '/d', 'p:4:1',
        '/c', 'System normal desktop manual cube test'))
}
else {
    Write-Output 'DESKTOP_REBOOT=NOT_REQUESTED'
}
