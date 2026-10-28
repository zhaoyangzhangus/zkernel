param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z0-9._-]+$')]
    [string]$RunId,
    [string]$DriveLetter = 'F',
    [string]$VolumeUniqueId = '',
    [string]$Build = 'build-real-hardware',
    [string]$UserSid = ''
)

$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if ([IO.Path]::IsPathRooted($Build) -or
    $Build -match '(^|[\\/])\.\.(?:[\\/]|$)') {
    throw "Build directory must be relative to the project: $Build"
}
$outputDirectory = Join-Path (Join-Path $projectRoot $Build) "real-hardware\$RunId"
$resultPath = Join-Path $outputDirectory 'realtest.result'
$outputLog = Join-Path $outputDirectory 'realtest.log'
$firmwareStatePath = Join-Path $outputDirectory 'firmware-state.txt'
$runOnceName = "SystemRealTest_$($RunId.Replace('-', '_'))"
$taskName = $runOnceName
$collectionComplete = $false

$firmwareGuid = '{4c495445-4f53-5254-4553-544355524e31}'
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class SystemFirmwareEnvironment {
    [StructLayout(LayoutKind.Sequential)]
    public struct Luid { public UInt32 LowPart; public Int32 HighPart; }
    [StructLayout(LayoutKind.Sequential)]
    public struct TokenPrivileges { public UInt32 Count; public Luid Luid; public UInt32 Attributes; }
    [DllImport("advapi32.dll", SetLastError = true)]
    static extern bool OpenProcessToken(IntPtr process, UInt32 access, out IntPtr token);
    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool LookupPrivilegeValue(string system, string name, out Luid luid);
    [DllImport("advapi32.dll", SetLastError = true)]
    static extern bool AdjustTokenPrivileges(IntPtr token, bool disable,
        ref TokenPrivileges privileges, UInt32 length, IntPtr previous, IntPtr returnLength);
    [DllImport("kernel32.dll")] static extern IntPtr GetCurrentProcess();
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
    public static bool EnableFirmwarePrivilege() {
        IntPtr token;
        Luid luid;
        if (!OpenProcessToken(GetCurrentProcess(), 0x28U, out token)) return false;
        try {
            TokenPrivileges privileges = new TokenPrivileges();
            if (!LookupPrivilegeValue(null, "SeSystemEnvironmentPrivilege", out luid)) return false;
            privileges.Count = 1U; privileges.Luid = luid; privileges.Attributes = 2U;
            return AdjustTokenPrivileges(token, false, ref privileges, 0U,
                IntPtr.Zero, IntPtr.Zero);
        }
        finally { CloseHandle(token); }
    }
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern UInt32 GetFirmwareEnvironmentVariable(
        string name, string guid, byte[] buffer, UInt32 size);
}
'@

New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null

function Write-Result([string]$status, [string]$detail) {
    Set-Content -LiteralPath $resultPath -Value @(
        "run_id=$RunId",
        "status=$status",
        "detail=$detail",
        "collected_at=$([DateTime]::UtcNow.ToString('o'))",
        "log=$outputLog"
    )
}

function Read-FirmwareState {
    $buffer = New-Object byte[] 8192
    if (-not [SystemFirmwareEnvironment]::EnableFirmwarePrivilege()) { return '' }
    $size = [SystemFirmwareEnvironment]::GetFirmwareEnvironmentVariable(
        'LiteOSRealTestState', $firmwareGuid, $buffer, [uint32]$buffer.Length)
    if ($size -eq 0) { return '' }
    return [Text.Encoding]::ASCII.GetString($buffer, 0, [int]$size).Trim([char]0,
        [char]13, [char]10)
}

function Write-FirmwareFallbackLog([string]$reason, [string]$state) {
    $lines = @(
        'OS_HOST_FIRMWARE_LOG',
        "reason=$reason"
    )
    if ($state) { $lines += $state }
    else { $lines += 'OS_REALTEST_NO_FIRMWARE_STATE' }
    Set-Content -LiteralPath $outputLog -Value $lines -Encoding UTF8
}

function Append-FirmwareStateIfNeeded([string]$path, [string]$log,
                                      [string]$state) {
    if (-not $state -or $log -match 'OS_REALTEST_PASS|OS_REALTEST_FAIL' -or
        $log -match 'OS_HOST_FIRMWARE_STATE') {
        return
    }
    Add-Content -LiteralPath $path -Value @(
        '',
        'OS_HOST_FIRMWARE_STATE',
        $state
    ) -Encoding UTF8
}

function Test-TerminalFirmwareFailure([string]$state) {
    if (-not $state) { return $false }
    return $state -match '(?m)^(?:OS_REALTEST_FAIL|FAILURE_ROOT_PROBE|ROOT_PROBE_FAIL|ROOT_USB_(?:ATTACH|VOLUME)_FAIL)\s*$'
}

function Get-UserRunOncePath {
    if ($UserSid -and $UserSid -match '^S-1-[0-9-]+$') {
        return "Registry::HKEY_USERS\$UserSid\Software\Microsoft\Windows\CurrentVersion\RunOnce"
    }
    return 'HKCU:\Software\Microsoft\Windows\CurrentVersion\RunOnce'
}

try {
    $firmwareState = Read-FirmwareState
    Set-Content -LiteralPath $firmwareStatePath -Value $firmwareState
    $volume = $null
    $volumeRoot = $null
    $sourceLog = $null
    if ($VolumeUniqueId) {
        $volume = Get-Volume -ErrorAction SilentlyContinue |
            Where-Object { $_.UniqueId -eq $VolumeUniqueId } |
            Select-Object -First 1
    }
    if ($null -eq $volume) {
        $volume = Get-Volume -DriveLetter $DriveLetter -ErrorAction SilentlyContinue
    }
    if ($null -ne $volume) {
        $roots = @()
        if ($volume.DriveLetter) { $roots += "$($volume.DriveLetter):\" }
        if ($volume.Path) { $roots += [string]$volume.Path }
        foreach ($accessPath in @($volume.AccessPaths)) {
            if ($accessPath) { $roots += [string]$accessPath }
        }
        foreach ($root in ($roots | Select-Object -Unique)) {
            $candidate = Join-Path $root 'log\realtest.log'
            if (Test-Path -LiteralPath $candidate) {
                $volumeRoot = $root
                $sourceLog = $candidate
                break
            }
        }
    }
    if ($null -eq $volume) {
        Write-FirmwareFallbackLog 'volume-unavailable' $firmwareState
        if ($firmwareState -match 'OS_REALTEST_PASS') {
            Write-Result 'pass-firmware-only' 'No volume was mounted; saved the terminal firmware state.'
            $collectionComplete = $true
            exit 0
        }
        if (Test-TerminalFirmwareFailure $firmwareState) {
            Write-Result 'firmware-failure' "No volume was mounted; saved firmware state: $firmwareState"
            $collectionComplete = $true
            exit 1
        }
        Write-Result 'missing-volume' "The System volume was not available after Windows logon. firmware=$firmwareState"
        exit 1
    }

    if ($null -eq $sourceLog) {
        Write-FirmwareFallbackLog 'log-unavailable' $firmwareState
        if ($firmwareState -match 'OS_REALTEST_PASS') {
            Write-Result 'pass-firmware-only' 'The volume had no file log; saved the terminal firmware state.'
            $collectionComplete = $true
            exit 0
        }
        if (Test-TerminalFirmwareFailure $firmwareState) {
            Write-Result 'firmware-failure' "The volume had no file log; saved firmware state: $firmwareState"
            $collectionComplete = $true
            exit 1
        }
        Write-Result 'missing-log' "No realtest.log was found on $volumeRoot. firmware=$firmwareState"
        exit 1
    }

    Copy-Item -LiteralPath $sourceLog -Destination $outputLog -Force
    $log = Get-Content -LiteralPath $outputLog -Raw
    Append-FirmwareStateIfNeeded $outputLog $log $firmwareState
    $log = Get-Content -LiteralPath $outputLog -Raw
    if ($log -match 'OS_REALTEST_FAIL') {
        Write-Result 'guest-failure' "Copied failure log from $sourceLog."
        $collectionComplete = $true
        exit 1
    }
    if ($log -match 'OS_REALTEST_PASS') {
        Write-Result 'pass' "Copied $sourceLog from volume $($volume.UniqueId)."
        $collectionComplete = $true
        exit 0
    }
    if (Test-TerminalFirmwareFailure $firmwareState) {
        Write-Result 'firmware-failure' "Firmware state: $firmwareState"
        $collectionComplete = $true
        exit 1
    }
    Write-Result 'incomplete-log' "Copied $sourceLog, but no PASS/FAIL marker was present. firmware=$firmwareState"
    exit 1
}
catch {
    Write-Result 'collector-error' $_.Exception.Message
    exit 1
}
finally {
    if ($collectionComplete) {
        Remove-ItemProperty -Path (Get-UserRunOncePath) -Name $runOnceName `
            -ErrorAction SilentlyContinue
        Unregister-ScheduledTask -TaskName $taskName -Confirm:$false `
            -ErrorAction SilentlyContinue
    }
}
