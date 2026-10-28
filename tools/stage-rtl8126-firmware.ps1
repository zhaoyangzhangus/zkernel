param(
    [string]$Build = 'build-refactor',
    [string]$SourceDirectory = '',
    [switch]$Download
)

$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

if ([IO.Path]::IsPathRooted($Build) -or
    $Build -match '(^|[\\/])\.\.(?:[\\/]|$)') {
    throw "Build directory must be relative to the project: $Build"
}
if ($SourceDirectory -and $Download) {
    throw 'Choose either -SourceDirectory or -Download.'
}
if (-not $SourceDirectory -and -not $Download) {
    throw 'Pass -SourceDirectory <linux-firmware directory> or -Download.'
}

$targetDirectory = Join-Path (Join-Path $projectRoot $Build) 'esp\EFI\OS\firmware\rtl8126'
$firmware = @(
    @('rtl8126a-2.fw',
      'https://kernel.googlesource.com/pub/scm/linux/kernel/git/firmware/linux-firmware/+/refs/heads/main/rtl_nic/rtl8126a-2.fw?format=TEXT'),
    @('rtl8126a-3.fw',
      'https://kernel.googlesource.com/pub/scm/linux/kernel/git/firmware/linux-firmware/+/refs/heads/main/rtl_nic/rtl8126a-3.fw?format=TEXT')
)

New-Item -ItemType Directory -Force -Path $targetDirectory | Out-Null

function Test-Rtl8126Firmware([string]$Path) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 45) { throw "Firmware file is truncated: $Path" }
    if ([BitConverter]::ToUInt32($bytes, 0) -ne 0) {
        throw "Firmware header is invalid: $Path"
    }
    $offset = [BitConverter]::ToUInt32($bytes, 36)
    $count = [BitConverter]::ToUInt32($bytes, 40)
    if ($offset -gt $bytes.Length -or
        $count -gt [uint32](($bytes.Length - $offset) / 4)) {
        throw "Firmware action range is invalid: $Path"
    }
    $checksum = 0
    foreach ($byte in $bytes) { $checksum = ($checksum + $byte) % 256 }
    if ($checksum -ne 0) { throw "Firmware checksum is invalid: $Path" }
}

foreach ($entry in $firmware) {
    $name = $entry[0]
    $destination = Join-Path $targetDirectory $name
    if ($SourceDirectory) {
        $source = Join-Path $SourceDirectory $name
        if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
            throw "Firmware file not found: $source"
        }
        Copy-Item -LiteralPath $source -Destination $destination -Force
    }
    else {
        $response = Invoke-WebRequest -Uri $entry[1] -UseBasicParsing
        $encoded = ($response.Content -replace '\s', '')
        try {
            [IO.File]::WriteAllBytes($destination,
                [Convert]::FromBase64String($encoded))
        }
        catch {
            throw "Firmware download is not valid Base64: $name"
        }
    }
    $length = (Get-Item -LiteralPath $destination).Length
    if ($length -lt 4) {
        throw "Firmware file is empty or truncated: $destination"
    }
    Test-Rtl8126Firmware $destination
    Write-Host ("Staged {0} ({1} bytes)" -f $name, $length)
}

Write-Host "RTL8126 firmware staged in $targetDirectory"
