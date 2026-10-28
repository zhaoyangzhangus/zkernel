param(
    [int]$DiskNumber = 1,
    [int]$PartitionNumber = 2,
    [ValidatePattern('^[A-Za-z]:?$')]
    [string]$DriveLetter = 'F',
    [string]$Build = 'build-realtest',
    [switch]$NvidiaPhase4,
    [switch]$ReuseNvidiaPhase4Build,
    [switch]$NvidiaPhase5,
    [switch]$ReuseNvidiaPhase5Build,
    [switch]$NvidiaPhase6,
    [switch]$ReuseNvidiaPhase6Build,
    [switch]$NvidiaPhase8,
    [switch]$ReuseNvidiaPhase8Build,
    [switch]$NvidiaPhase9,
    [switch]$ReuseNvidiaPhase9Build,
    [switch]$NvidiaPhase10,
    [switch]$ReuseNvidiaPhase10Build,
    [switch]$NvidiaPhase11,
    [switch]$ReuseNvidiaPhase11Build,
    [switch]$NvidiaPhase12,
    [switch]$ReuseNvidiaPhase12Build,
    [switch]$NvidiaPhaseW3,
    [switch]$ReuseNvidiaPhaseW3Build,
    [switch]$NvidiaCubeDemo,
    [switch]$ReuseNvidiaCubeDemoBuild,
    [switch]$DiagnosticBot,
    [switch]$Rtl8126FirmwareDownload,
    [switch]$ForceReboot,
    [switch]$NoReboot
)

$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$letter = $DriveLetter.TrimEnd(':').ToUpperInvariant()
$runId = Get-Date -Format 'yyyyMMdd-HHmmss'
$runDirectory = Join-Path (Join-Path $projectRoot $Build) "real-hardware\$runId"
$backupDirectory = Join-Path $runDirectory 'backup'
$runOncePath = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\RunOnce'
$runOnceName = "SystemRealTest_$($runId.Replace('-', '_'))"
$bootSequenceSet = $false
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
    public static extern bool SetFirmwareEnvironmentVariableEx(
        string name, string guid, byte[] value, UInt32 size, UInt32 attributes);
}
'@

if ($letter.Length -ne 1) { throw "DriveLetter must be one letter: $DriveLetter" }
if ($DiskNumber -lt 0 -or $PartitionNumber -lt 1) {
    throw 'DiskNumber and PartitionNumber are invalid.'
}
if (@($NvidiaPhase4, $NvidiaPhase5, $NvidiaPhase6, $NvidiaPhase8, $NvidiaPhase9, $NvidiaPhase10, $NvidiaPhase11, $NvidiaPhase12, $NvidiaPhaseW3, $NvidiaCubeDemo).Where({ $_ }).Count -gt 1) {
    throw 'NVIDIA phase switches are mutually exclusive.'
}
if ($ReuseNvidiaPhase4Build -and -not $NvidiaPhase4) {
    throw 'ReuseNvidiaPhase4Build requires NvidiaPhase4.'
}
if ($ReuseNvidiaPhase5Build -and -not $NvidiaPhase5) {
    throw 'ReuseNvidiaPhase5Build requires NvidiaPhase5.'
}
if ($ReuseNvidiaPhase6Build -and -not $NvidiaPhase6) {
    throw 'ReuseNvidiaPhase6Build requires NvidiaPhase6.'
}
if ($ReuseNvidiaPhase8Build -and -not $NvidiaPhase8) {
    throw 'ReuseNvidiaPhase8Build requires NvidiaPhase8.'
}
if ($ReuseNvidiaPhase9Build -and -not $NvidiaPhase9) {
    throw 'ReuseNvidiaPhase9Build requires NvidiaPhase9.'
}
if ($ReuseNvidiaPhase10Build -and -not $NvidiaPhase10) {
    throw 'ReuseNvidiaPhase10Build requires NvidiaPhase10.'
}
if ($ReuseNvidiaPhase11Build -and -not $NvidiaPhase11) {
    throw 'ReuseNvidiaPhase11Build requires NvidiaPhase11.'
}
if ($ReuseNvidiaPhase12Build -and -not $NvidiaPhase12) {
    throw 'ReuseNvidiaPhase12Build requires NvidiaPhase12.'
}
if ($ReuseNvidiaPhaseW3Build -and -not $NvidiaPhaseW3) {
    throw 'ReuseNvidiaPhaseW3Build requires NvidiaPhaseW3.'
}
if ($ReuseNvidiaCubeDemoBuild -and -not $NvidiaCubeDemo) {
    throw 'ReuseNvidiaCubeDemoBuild requires NvidiaCubeDemo.'
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'This operation must run from an elevated PowerShell window.'
}

New-Item -ItemType Directory -Force -Path $runDirectory,$backupDirectory | Out-Null

function Invoke-NativeChecked([string]$FilePath, [string[]]$Arguments) {
    $output = @(& $FilePath @Arguments 2>&1 | ForEach-Object { [string]$_ })
    $status = $LASTEXITCODE
    if ($status -ne 0) {
        throw "$FilePath failed ($status): $($output -join ' ')"
    }
    return ($output -join "`r`n")
}

function Save-Text([string]$Path, [string]$Text) {
    Set-Content -LiteralPath $Path -Value $Text -Encoding UTF8
}

function Set-FirmwareState([string]$state) {
    $bytes = [Text.Encoding]::ASCII.GetBytes($state)
    if (-not [SystemFirmwareEnvironment]::EnableFirmwarePrivilege()) { return $false }
    return [SystemFirmwareEnvironment]::SetFirmwareEnvironmentVariableEx(
        'LiteOSRealTestState', $firmwareGuid, $bytes, [uint32]$bytes.Length, 7)
}

function Backup-Target([string]$relativePath, [string]$volumeRoot) {
    $source = Join-Path $volumeRoot $relativePath
    if (-not (Test-Path -LiteralPath $source)) { return }
    $destination = Join-Path $backupDirectory $relativePath
    $parent = Split-Path -Parent $destination
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
    Copy-Item -LiteralPath $source -Destination $destination -Recurse -Force
}

function Copy-FileIfChanged([string]$sourceFile, [string]$targetFile) {
    $sourceHash = (Get-FileHash -LiteralPath $sourceFile -Algorithm SHA256).Hash
    if (Test-Path -LiteralPath $targetFile -PathType Leaf) {
        $targetHash = (Get-FileHash -LiteralPath $targetFile -Algorithm SHA256).Hash
        if ($sourceHash -eq $targetHash) {
            $script:deploymentSkippedFiles++
            $script:deploymentSkippedPaths.Add($targetFile)
            return
        }
    }

    $parent = Split-Path -Parent $targetFile
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
    Copy-Item -LiteralPath $sourceFile -Destination $targetFile -Force
    $script:deploymentCopiedFiles++
    $script:deploymentCopiedPaths.Add($targetFile)
}

function Copy-DirectoryContents([string]$sourceDirectory, [string]$targetDirectory) {
    New-Item -ItemType Directory -Force -Path $targetDirectory | Out-Null
    foreach ($child in (Get-ChildItem -LiteralPath $sourceDirectory -Force)) {
        $destination = Join-Path $targetDirectory $child.Name
        if ($child.PSIsContainer) {
            Copy-DirectoryContents $child.FullName $destination
        }
        else {
            Copy-FileIfChanged $child.FullName $destination
        }
    }
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
$preflight = [ordered]@{
    run_id = $runId
    disk_number = $disk.Number
    partition_number = $partition.PartitionNumber
    drive_letter = $letter
    disk_model = $disk.FriendlyName
    disk_serial = $disk.SerialNumber
    disk_unique_id = $disk.UniqueId
    bus_type = $disk.BusType.ToString()
    volume_label = $volume.FileSystemLabel
    volume_unique_id = $volume.UniqueId
    volume_size = $volume.Size
    build = $Build
}
$preflight | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $runDirectory 'preflight.json')
Save-Text (Join-Path $runDirectory 'disk.txt') (
    (Get-Disk -Number $DiskNumber | Format-List * | Out-String) +
    (Get-Partition -DiskNumber $DiskNumber -PartitionNumber $PartitionNumber |
        Format-List * | Out-String) +
    (Get-Volume -DriveLetter $letter | Format-List * | Out-String)
)

$firmwareBefore = Invoke-NativeChecked 'bcdedit.exe' @('/enum', 'firmware', '/v')
Save-Text (Join-Path $runDirectory 'bcd-firmware-before.txt') $firmwareBefore

$firmwareId = $null
$matchingFirmware = @()
$firmwareBlocks = $firmwareBefore -split '(?:\r?\n){2,}'
foreach ($block in $firmwareBlocks) {
    if ($block -notmatch "(?im)^\s*path\s+.*\\EFI\\BOOT\\BOOTX64\.EFI\s*$" -or
        $block -notmatch "(?im)^\s*device\s+.*$letter`:\s*$") {
        continue
    }
    $match = [regex]::Match($block, '(?im)^\s*identifier\s+(\{[^}]+\})\s*$')
    if ($match.Success) {
        $description = [regex]::Match(
            $block, '(?im)^\s*description\s+(.+?)\s*$')
        $matchingFirmware += [pscustomobject]@{
            Identifier = $match.Groups[1].Value
            Description = if ($description.Success) {
                $description.Groups[1].Value.Trim()
            } else { '' }
        }
    }
}
$preferredFirmware = @($matchingFirmware |
    Where-Object { $_.Description -in @("System USB $letter", "LiteOS USB $letter") })
if ($preferredFirmware.Count -eq 1) {
    $firmwareId = $preferredFirmware[0].Identifier
} elseif ($matchingFirmware.Count -eq 1) {
    $firmwareId = $matchingFirmware[0].Identifier
} elseif ($preferredFirmware.Count -gt 1) {
    throw "Multiple System firmware entries point to $letter`: ."
}
if (-not $firmwareId) {
    throw "No UEFI firmware entry for $letter`: \\EFI\\BOOT\\BOOTX64.EFI was found."
}

$espPath = $null
if ($NvidiaPhase4) {
    # 此选项刻意要求显式启用。普通真机测试路径继续构建正式 System 映像；NVIDIA 路径必须使用
    # phase4_build.ps1 生成的完整 Phase 4 供应商 RM 链接。
    $espPath = Join-Path $projectRoot 'build\nvidia\phase4\esp'
    if ($ReuseNvidiaPhase4Build) {
        foreach ($required in @(
            (Join-Path $espPath 'EFI\BOOT\BOOTX64.EFI'),
            (Join-Path $espPath 'EFI\OS\kernel.elf')
        )) {
            if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
                throw "Cannot reuse incomplete NVIDIA Phase 4 image: $required"
            }
        }
        Write-Host "Reusing verified NVIDIA Phase 4 image at $espPath"
    }
    else {
        $buildScript = Join-Path $projectRoot 'tools\nvidia\phase4_build.ps1'
        $buildArguments = @('-Build', $Build, '-RealTest')
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $buildScript @buildArguments
        if ($LASTEXITCODE -ne 0) { throw "Physical NVIDIA Phase 4 build failed ($LASTEXITCODE)." }
    }
}
elseif ($NvidiaPhase5) {
    # Phase 5 复用已完成的 Phase 4 供应商对象图，仅重建发生变化的 GSP/固件边界。
    # 构建脚本还会将匹配的 595.84 固件暂存到此 ESP。
    $espPath = Join-Path $projectRoot 'build\nvidia\phase5\esp'
    if ($ReuseNvidiaPhase5Build) {
        foreach ($required in @(
            (Join-Path $espPath 'EFI\BOOT\BOOTX64.EFI'),
            (Join-Path $espPath 'EFI\OS\kernel.elf'),
            (Join-Path $espPath 'EFI\OS\firmware\gsp_ga10x.bin')
        )) {
            if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
                throw "Cannot reuse incomplete NVIDIA Phase 5 image: $required"
            }
        }
        Write-Host "Reusing verified NVIDIA Phase 5 image at $espPath"
    }
    else {
        $buildScript = Join-Path $projectRoot 'tools\nvidia\phase5_build.ps1'
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $buildScript
        if ($LASTEXITCODE -ne 0) { throw "Physical NVIDIA Phase 5 build failed ($LASTEXITCODE)." }
    }
}
elseif ($NvidiaPhase6) {
    # Phase 6 复用已验证的 Phase 5 ESP，仅替换桥接依赖闭包和 RM/CE 探针。
    # 必须显式指定 -FullGate，避免普通 Phase 6 映像静默宣称通过 20 轮硬件门禁。
    $espPath = Join-Path $projectRoot 'build\nvidia\phase6\esp'
    if ($ReuseNvidiaPhase6Build) {
        foreach ($required in @(
            (Join-Path $espPath 'EFI\BOOT\BOOTX64.EFI'),
            (Join-Path $espPath 'EFI\OS\kernel.elf'),
            (Join-Path $espPath 'EFI\OS\firmware\gsp_ga10x.bin')
        )) {
            if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
                throw "Cannot reuse incomplete NVIDIA Phase 6 image: $required"
            }
        }
        Write-Host "Reusing verified NVIDIA Phase 6 image at $espPath"
    }
    else {
        $buildScript = Join-Path $projectRoot 'tools\nvidia\phase6_build.ps1'
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $buildScript -FullGate
        if ($LASTEXITCODE -ne 0) { throw "Physical NVIDIA Phase 6 build failed ($LASTEXITCODE)." }
    }
}
elseif ($NvidiaPhase8) {
    # Phase 8 是首个包含 Linux 兼容模式 NVIDIA 前端的映像。基于已验证的 Phase 6 ESP 构建，
    # 并暂存真实 glibc 探针；QEMU 映像无法提供所需 NVIDIA 节点。
    $espPath = Join-Path $projectRoot 'build\nvidia\phase8\esp'
    if ($ReuseNvidiaPhase8Build) {
        foreach ($required in @(
            (Join-Path $espPath 'EFI\BOOT\BOOTX64.EFI'),
            (Join-Path $espPath 'EFI\OS\kernel.elf'),
            (Join-Path $espPath 'EFI\OS\firmware\gsp_ga10x.bin'),
            (Join-Path $espPath 'tests\p8-frontend'),
            (Join-Path $espPath 'lib64\ld-linux-x86-64.so.2'),
            (Join-Path $espPath 'lib\x86_64-linux-gnu\libc.so.6')
        )) {
            if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
                throw "Cannot reuse incomplete NVIDIA Phase 8 image: $required"
            }
        }
        Write-Host "Reusing verified NVIDIA Phase 8 image at $espPath"
    }
    else {
        $buildScript = Join-Path $projectRoot 'tools\nvidia\phase8_build.ps1'
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $buildScript
        if ($LASTEXITCODE -ne 0) { throw "Physical NVIDIA Phase 8 build failed ($LASTEXITCODE)." }
        $stageScript = Join-Path $projectRoot 'tools\nvidia\phase8_stage.ps1'
        $stageArguments = @(
            '-Build', 'build/nvidia/phase8',
            '-GlibcRoot', 'build-phase7/host-deps/glibc',
            '-TestBinary', 'build-phase8/phase8-frontend/p8-frontend'
        )
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $stageScript @stageArguments
        if ($LASTEXITCODE -ne 0) { throw "Phase 8 probe staging failed ($LASTEXITCODE)." }
    }
}
elseif ($NvidiaPhase9) {
    # Phase 9 keeps the verified P8 kernel/RM/GSP image and adds only the
    # matching 595.84 Linux userspace closure plus its loader probe.
    $espPath = Join-Path $projectRoot 'build\nvidia\phase9\esp'
    if ($ReuseNvidiaPhase9Build) {
        foreach ($required in @(
            (Join-Path $espPath 'EFI\BOOT\BOOTX64.EFI'),
            (Join-Path $espPath 'EFI\OS\kernel.elf'),
            (Join-Path $espPath 'EFI\OS\firmware\gsp_ga10x.bin'),
            (Join-Path $espPath 'tests\p9-userspace-load'),
            (Join-Path $espPath 'lib64\ld-linux-x86-64.so.2'),
            (Join-Path $espPath 'lib\x86_64-linux-gnu\libc.so.6'),
            (Join-Path $espPath 'compat\linux\lib\x86_64-linux-gnu\libEGL.so.1'),
            (Join-Path $espPath 'compat\linux\lib\x86_64-linux-gnu\libEGL_nvidia.so.0')
        )) {
            if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
                throw "Cannot reuse incomplete NVIDIA Phase 9 image: $required"
            }
        }
        Write-Host "Reusing verified NVIDIA Phase 9 image at $espPath"
    }
    else {
        $buildScript = Join-Path $projectRoot 'tools\nvidia\phase9_build.ps1'
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $buildScript
        if ($LASTEXITCODE -ne 0) { throw "Physical NVIDIA Phase 9 build failed ($LASTEXITCODE)." }
    }
}
elseif ($NvidiaPhase10) {
    # Phase 10 keeps the verified P9 driver/userspace closure and adds only
    # the real EGL device/context acceptance probe.
    $espPath = Join-Path $projectRoot 'build\nvidia\phase10\esp'
    if ($ReuseNvidiaPhase10Build) {
        foreach ($required in @(
            (Join-Path $espPath 'EFI\BOOT\BOOTX64.EFI'),
            (Join-Path $espPath 'EFI\OS\kernel.elf'),
            (Join-Path $espPath 'EFI\OS\firmware\gsp_ga10x.bin'),
            (Join-Path $espPath 'tests\p10-egl-context'),
            (Join-Path $espPath 'lib64\ld-linux-x86-64.so.2'),
            (Join-Path $espPath 'lib\x86_64-linux-gnu\libc.so.6'),
            (Join-Path $espPath 'compat\linux\lib\x86_64-linux-gnu\libEGL.so.1'),
            (Join-Path $espPath 'compat\linux\lib\x86_64-linux-gnu\libEGL_nvidia.so.0')
        )) {
            if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
                throw "Cannot reuse incomplete NVIDIA Phase 10 image: $required"
            }
        }
        Write-Host "Reusing verified NVIDIA Phase 10 image at $espPath"
    }
    else {
        $buildScript = Join-Path $projectRoot 'tools\nvidia\phase10_build.ps1'
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $buildScript
        if ($LASTEXITCODE -ne 0) { throw "Physical NVIDIA Phase 10 build failed ($LASTEXITCODE)." }
    }
}
elseif ($NvidiaPhase11) {
    # Phase 11 keeps the verified P10 EGL/userspace closure and adds only the
    # real triangle/readback probe and its matching kernel runner.
    $espPath = Join-Path $projectRoot 'build\nvidia\phase11\esp'
    if ($ReuseNvidiaPhase11Build) {
        foreach ($required in @(
            (Join-Path $espPath 'EFI\BOOT\BOOTX64.EFI'),
            (Join-Path $espPath 'EFI\OS\kernel.elf'),
            (Join-Path $espPath 'EFI\OS\firmware\gsp_ga10x.bin'),
            (Join-Path $espPath 'tests\p11-triangle'),
            (Join-Path $espPath 'lib64\ld-linux-x86-64.so.2'),
            (Join-Path $espPath 'lib\x86_64-linux-gnu\libc.so.6'),
            (Join-Path $espPath 'compat\linux\lib\x86_64-linux-gnu\libEGL.so.1'),
            (Join-Path $espPath 'compat\linux\lib\x86_64-linux-gnu\libEGL_nvidia.so.0')
        )) {
            if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
                throw "Cannot reuse incomplete NVIDIA Phase 11 image: $required"
            }
        }
        Write-Host "Reusing verified NVIDIA Phase 11 image at $espPath"
    }
    else {
        $buildScript = Join-Path $projectRoot 'tools\nvidia\phase11_build.ps1'
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $buildScript
        if ($LASTEXITCODE -ne 0) { throw "Physical NVIDIA Phase 11 build failed ($LASTEXITCODE)." }
    }
}
elseif ($NvidiaCubeDemo) {
    # The cube demo reuses the verified Phase 12 kernel and GLVND closure,
    # replacing only the userspace test image in its own incremental ESP.
    $espPath = Join-Path $projectRoot 'build\nvidia\cube-demo\esp'
    if ($ReuseNvidiaCubeDemoBuild) {
        foreach ($required in @(
            (Join-Path $espPath 'EFI\BOOT\BOOTX64.EFI'),
            (Join-Path $espPath 'EFI\OS\kernel.elf'),
            (Join-Path $espPath 'EFI\OS\firmware\gsp_ga10x.bin'),
            (Join-Path $espPath 'tests\p12-window'),
            (Join-Path $espPath 'lib64\ld-linux-x86-64.so.2'),
            (Join-Path $espPath 'lib\x86_64-linux-gnu\libc.so.6'),
            (Join-Path $espPath 'compat\linux\lib\x86_64-linux-gnu\libEGL.so.1'),
            (Join-Path $espPath 'compat\linux\lib\x86_64-linux-gnu\libEGL_nvidia.so.0')
        )) {
            if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
                throw "Cannot reuse incomplete NVIDIA cube demo image: $required"
            }
        }
        Write-Host "Reusing NVIDIA cube demo image at $espPath"
    }
    else {
        $buildScript = Join-Path $projectRoot 'tools\nvidia\cube_demo_build.ps1'
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $buildScript
        if ($LASTEXITCODE -ne 0) { throw "NVIDIA cube demo build failed ($LASTEXITCODE)." }
    }
}
elseif ($NvidiaPhase12) {
    # Phase 12 keeps the verified P11 EGL/GL closure and adds the CPU
    # glReadPixels -> shared System window surface -> compositor bridge.
    $espPath = Join-Path $projectRoot 'build\nvidia\phase12\esp'
    if ($ReuseNvidiaPhase12Build) {
        foreach ($required in @(
            (Join-Path $espPath 'EFI\BOOT\BOOTX64.EFI'),
            (Join-Path $espPath 'EFI\OS\kernel.elf'),
            (Join-Path $espPath 'EFI\OS\firmware\gsp_ga10x.bin'),
            (Join-Path $espPath 'tests\p12-window'),
            (Join-Path $espPath 'lib64\ld-linux-x86-64.so.2'),
            (Join-Path $espPath 'lib\x86_64-linux-gnu\libc.so.6'),
            (Join-Path $espPath 'compat\linux\lib\x86_64-linux-gnu\libEGL.so.1'),
            (Join-Path $espPath 'compat\linux\lib\x86_64-linux-gnu\libEGL_nvidia.so.0')
        )) {
            if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
                throw "Cannot reuse incomplete NVIDIA Phase 12 image: $required"
            }
        }
        Write-Host "Reusing verified NVIDIA Phase 12 image at $espPath"
    }
    else {
        $buildScript = Join-Path $projectRoot 'tools\nvidia\phase12_build.ps1'
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $buildScript
        if ($LASTEXITCODE -ne 0) { throw "Physical NVIDIA Phase 12 build failed ($LASTEXITCODE)." }
    }
}
elseif ($NvidiaPhaseW3) {
    # W3 uses the verified Phase 9 ESP for the loader, matching GSP firmware,
    # and 595.84 user assets, then replaces only the W3 REALTEST kernel.
    $espPath = Join-Path $projectRoot 'build\nvidia\phase-w3\esp'
    if ($ReuseNvidiaPhaseW3Build) {
        foreach ($required in @(
            (Join-Path $espPath 'EFI\BOOT\BOOTX64.EFI'),
            (Join-Path $espPath 'EFI\OS\kernel.elf'),
            (Join-Path $espPath 'EFI\OS\loader.conf'),
            (Join-Path $espPath 'EFI\OS\firmware\gsp_ga10x.bin')
        )) {
            if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
                throw "Cannot reuse incomplete NVIDIA W3 image: $required"
            }
        }
        $loaderText = [IO.File]::ReadAllText((Join-Path $espPath 'EFI\OS\loader.conf'))
        if ($loaderText -notmatch '(?im)^cmdline=.*\brealtest=1\b') {
            throw 'Cannot reuse W3 image without cmdline=realtest=1.'
        }
        Write-Host "Reusing verified NVIDIA W3 image at $espPath"
    }
    else {
        $buildScript = Join-Path $projectRoot 'tools\nvidia\phase-w3_build.ps1'
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $buildScript -RealTest
        if ($LASTEXITCODE -ne 0) { throw "Physical NVIDIA W3 build failed ($LASTEXITCODE)." }
        $stageScript = Join-Path $projectRoot 'tools\nvidia\phase-w3_stage.ps1'
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $stageScript `
            -Build build/nvidia/phase-w3 -BaseEsp build/nvidia/phase9/esp
        if ($LASTEXITCODE -ne 0) { throw "NVIDIA W3 staging failed ($LASTEXITCODE)." }
    }
}
else {
    $buildScript = Join-Path $projectRoot 'tools\build-windows.ps1'
    $buildArguments = @('-Build', $Build, '-Debug', '2', '-RealTest')
    if ($DiagnosticBot) { $buildArguments += '-DiagnosticBot' }
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $buildScript @buildArguments
    if ($LASTEXITCODE -ne 0) { throw "Physical real-test build failed ($LASTEXITCODE)." }
    $espPath = Join-Path (Join-Path $projectRoot $Build) 'esp'
}

if ($Rtl8126FirmwareDownload) {
    $firmwareScript = Join-Path $projectRoot 'tools\stage-rtl8126-firmware.ps1'
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $firmwareScript `
        -Build $Build -Download
    if ($LASTEXITCODE -ne 0) {
        throw "RTL8126 firmware staging failed ($LASTEXITCODE)."
    }
}

foreach ($required in @(
    'EFI\BOOT\BOOTX64.EFI',
    'EFI\OS\kernel.elf',
    'EFI\OS\loader.conf'
)) {
    if (-not (Test-Path -LiteralPath (Join-Path $espPath $required))) {
        throw "Build is missing ESP file: $required"
    }
}
if (($NvidiaPhase5 -or $NvidiaPhase6 -or $NvidiaPhase8 -or $NvidiaPhase9 -or $NvidiaPhase10 -or $NvidiaPhase11 -or $NvidiaPhase12 -or $NvidiaPhaseW3 -or $NvidiaCubeDemo) -and
    -not (Test-Path -LiteralPath (Join-Path $espPath 'EFI\OS\firmware\gsp_ga10x.bin') -PathType Leaf)) {
    throw 'NVIDIA ESP is missing EFI\OS\firmware\gsp_ga10x.bin'
}

foreach ($relative in @(
    'EFI\BOOT\BOOTX64.EFI',
    'EFI\OS',
    'log',
    'lib',
    'sbin'
)) {
    Backup-Target $relative $volumeRoot
}

Remove-Item -LiteralPath (Join-Path $volumeRoot 'log\realtest.log'),
    (Join-Path $volumeRoot 'realtest.log') -Force -ErrorAction SilentlyContinue
$script:deploymentCopiedFiles = 0
$script:deploymentSkippedFiles = 0
$script:deploymentCopiedPaths = New-Object 'System.Collections.Generic.List[string]'
$script:deploymentSkippedPaths = New-Object 'System.Collections.Generic.List[string]'
foreach ($entry in (Get-ChildItem -LiteralPath $espPath -Force)) {
    $destination = Join-Path $volumeRoot $entry.Name
    if ($entry.PSIsContainer) {
        Copy-DirectoryContents $entry.FullName $destination
    }
    else {
        Copy-FileIfChanged $entry.FullName $destination
    }
}

$deploymentSummary = @(
    "copied_files=$($script:deploymentCopiedFiles)"
    "skipped_unchanged_files=$($script:deploymentSkippedFiles)"
    "copied_paths=$($script:deploymentCopiedPaths.Count)"
    "skipped_paths=$($script:deploymentSkippedPaths.Count)"
)
Save-Text (Join-Path $runDirectory 'deployment-summary.txt') ($deploymentSummary -join "`r`n")
Write-Host ("Incremental deployment: copied {0} file(s), skipped {1} unchanged file(s)." -f `
    $script:deploymentCopiedFiles, $script:deploymentSkippedFiles)

$sourceBoot = Join-Path $espPath 'EFI\BOOT\BOOTX64.EFI'
$targetBoot = Join-Path $volumeRoot 'EFI\BOOT\BOOTX64.EFI'
$sourceKernel = Join-Path $espPath 'EFI\OS\kernel.elf'
$targetKernel = Join-Path $volumeRoot 'EFI\OS\kernel.elf'
foreach ($pair in @(
    @($sourceBoot, $targetBoot),
    @($sourceKernel, $targetKernel)
)) {
    $sourceHash = (Get-FileHash -LiteralPath $pair[0] -Algorithm SHA256).Hash
    $targetHash = (Get-FileHash -LiteralPath $pair[1] -Algorithm SHA256).Hash
    if ($sourceHash -ne $targetHash) {
        throw "F: verification failed for $($pair[1])."
    }
}
Save-Text (Join-Path $runDirectory 'copied-files.txt') (
    (Get-ChildItem -LiteralPath $espPath -Recurse -File | ForEach-Object {
        "{0} {1}" -f $_.FullName, (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
    }) -join "`r`n"
)

$bcdBackup = Join-Path $runDirectory 'windows-bcd-backup'
Invoke-NativeChecked 'bcdedit.exe' @('/export', $bcdBackup) | Out-Null

if (-not (Set-FirmwareState "HOST_PREPARED run=$runId")) {
    Save-Text (Join-Path $runDirectory 'firmware-state-set.txt') (
        "failed win32_error=$([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
    )
}
else {
    Save-Text (Join-Path $runDirectory 'firmware-state-set.txt') "HOST_PREPARED run=$runId"
}

$collector = (Resolve-Path (Join-Path $projectRoot 'tools\collect-realtest-log.ps1')).Path
$collectorLiteral = $collector.Replace("'", "''")
$volumeIdLiteral = ([string]$volume.UniqueId).Replace("'", "''")
$buildLiteral = $Build.Replace("'", "''")
$userSidLiteral = $identity.User.Value.Replace("'", "''")
$collectorScript = "& '$collectorLiteral' -RunId '$runId' -DriveLetter '$letter' " +
    "-VolumeUniqueId '$volumeIdLiteral' -Build '$buildLiteral' " +
    "-UserSid '$userSidLiteral'"
$collectorEncoded = [Convert]::ToBase64String(
    [Text.Encoding]::Unicode.GetBytes($collectorScript))
$collectorArguments = '-NoProfile -ExecutionPolicy Bypass -EncodedCommand ' +
    $collectorEncoded
$runOnceCommand = 'powershell.exe ' + $collectorArguments
New-ItemProperty -Path $runOncePath -Name $runOnceName -PropertyType String `
    -Value $runOnceCommand -Force | Out-Null
Save-Text (Join-Path $runDirectory 'runonce-command.txt') $runOnceCommand
Save-Text (Join-Path $runDirectory 'collector-script.txt') $collectorScript

try {
    $taskAction = New-ScheduledTaskAction -Execute 'powershell.exe' `
        -Argument $collectorArguments -WorkingDirectory $projectRoot
    $taskTriggers = @(
        (New-ScheduledTaskTrigger -AtStartup),
        (New-ScheduledTaskTrigger -AtLogOn -User $identity.Name)
    )
    $taskPrincipal = New-ScheduledTaskPrincipal -UserId 'SYSTEM' `
        -LogonType ServiceAccount -RunLevel Highest
    Register-ScheduledTask -TaskName $runOnceName -Action $taskAction `
        -Trigger $taskTriggers -Principal $taskPrincipal -Force | Out-Null
    Save-Text (Join-Path $runDirectory 'scheduled-task.txt') (
        "$runOnceName`r`nprincipal=SYSTEM`r`ntriggers=AtStartup,AtLogOn"
    )
}
catch {
    Save-Text (Join-Path $runDirectory 'scheduled-task.txt') `
        "registration-failed: $($_.Exception.Message)"
}

try {
    Invoke-NativeChecked 'bcdedit.exe' @('/set', '{fwbootmgr}', 'bootsequence', $firmwareId) |
        Out-Null
    $bootSequenceSet = $true
    $firmwareAfter = Invoke-NativeChecked 'bcdedit.exe' @('/enum', '{fwbootmgr}', '/v')
    Save-Text (Join-Path $runDirectory 'bcd-fwbootmgr-after.txt') $firmwareAfter
    if ($firmwareAfter -notmatch [regex]::Escape($firmwareId)) {
        throw "UEFI one-time boot sequence was not set to $firmwareId."
    }
    Save-Text (Join-Path $runDirectory 'reboot-requested.txt') (
        "BootNext firmware entry: $firmwareId`r`nTarget: $letter`:\EFI\BOOT\BOOTX64.EFI`r`n"
    )
    if ($NoReboot) {
        Write-Host "Prepared one-time System boot for $letter`:; reboot was not requested."
        exit 0
    }
    Write-Host "Starting one-time System boot from $letter`:. Windows will resume after System resets."
    $shutdownArguments = @('/r')
    if ($ForceReboot) { $shutdownArguments += '/f' }
    $shutdownArguments += @('/t', '0', '/d', 'p:4:1', '/c', 'System real hardware test')
    & shutdown.exe @shutdownArguments
    if ($LASTEXITCODE -ne 0) { throw "shutdown.exe failed ($LASTEXITCODE)." }
}
catch {
    Remove-ItemProperty -Path $runOncePath -Name $runOnceName -ErrorAction SilentlyContinue
    Unregister-ScheduledTask -TaskName $runOnceName -Confirm:$false `
        -ErrorAction SilentlyContinue
    if ($bootSequenceSet) {
        & bcdedit.exe /deletevalue '{fwbootmgr}' bootsequence 2>$null | Out-Null
    }
    throw
}
