param(
    [string]$Build = 'build-refactor',
    [string]$Cpu = '1,2,4,8',
    [int]$Seconds = 45,
    [ValidateSet(1, 2)]
    [int]$Debug = 2,
    [switch]$NvmeRoot,
    [switch]$NoBuild
)

$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

if ([IO.Path]::IsPathRooted($Build) -or
    $Build -match '(^|[\\/])\.\.(?:[\\/]|$)') {
    throw "Build directory must be relative to the project: $Build"
}
if ($Seconds -lt 1) {
    throw "Seconds must be positive: $Seconds"
}
$cpuCounts = @(
    $Cpu -split '[,; ]+' |
        Where-Object { $_ } |
        ForEach-Object { [int]$_ }
)
if ($cpuCounts.Count -eq 0 -or ($cpuCounts | Where-Object { $_ -lt 1 }).Count -ne 0) {
    throw "Cpu must contain only positive values."
}

$buildDirectory = Join-Path $projectRoot $Build
$serialLog = Join-Path $buildDirectory 'qemu-native-serial.log'
$matrixDirectory = Join-Path $buildDirectory 'native-matrix'
$runScript = Join-Path $projectRoot 'run-qemu-windows.ps1'
$verifyScript = 'tools/verify-qemu-stages.sh'
$verifyLocationsScript = 'tools/verify-debug-locations.sh'
$kernelRelative = "$Build/esp/EFI/OS/kernel.elf"

New-Item -ItemType Directory -Force -Path $buildDirectory,$matrixDirectory | Out-Null
Set-Location $projectRoot

function Get-MatchingQemu([string]$buildName) {
    $expectedImage = [IO.Path]::GetFullPath(
        (Join-Path (Join-Path $projectRoot $buildName) 'qemu-native-fat.img'))
    $expectedImagePattern = [regex]::Escape($expectedImage)

    return @(
        Get-CimInstance Win32_Process -Filter "Name = 'qemu-system-x86_64.exe'" -ErrorAction SilentlyContinue |
            Where-Object {
                $_.CommandLine -and
                $_.CommandLine -like '*qemu-native-fat.img*' -and
                $_.CommandLine -match $expectedImagePattern
            }
    )
}

function Stop-NativeRun(
    [System.Diagnostics.Process]$runner,
    [string]$buildName) {
    foreach ($record in @(Get-MatchingQemu $buildName)) {
        $process = Get-Process -Id ([int]$record.ProcessId) -ErrorAction SilentlyContinue
        if (-not $process) { continue }
        try { $process.CloseMainWindow() | Out-Null } catch { }
        try {
            if (-not $process.WaitForExit(1000)) {
                $process.Kill()
                $process.WaitForExit(1000)
            }
        }
        catch {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        }
    }

    if ($runner) {
        try { $null = $runner.WaitForExit(5000) } catch { }
        if (-not $runner.HasExited) {
            Stop-Process -Id $runner.Id -Force -ErrorAction SilentlyContinue
        }
    }

    # 包装器可能在 Start-Process 启动后、进入等待循环前被取消。
    # 包装器退出后重新扫描，确保本轮没有遗留 QEMU 子进程。
    foreach ($record in @(Get-MatchingQemu $buildName)) {
        Stop-Process -Id ([int]$record.ProcessId) -Force -ErrorAction SilentlyContinue
    }
}

if (-not $NoBuild) {
    $buildScript = Join-Path $projectRoot 'tools\build-windows.ps1'
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $buildScript `
        -Build $Build -Debug $Debug -Serial
    if ($LASTEXITCODE -ne 0) {
        throw "Native System build failed ($LASTEXITCODE)."
    }
}

if (-not (Test-Path -LiteralPath (Join-Path $buildDirectory 'esp\EFI\OS\kernel.elf'))) {
    throw "Kernel ELF not found in $Build."
}

$failures = 0
foreach ($cpuCount in $cpuCounts) {
    if ((Get-MatchingQemu $Build).Count -ne 0) {
        throw "A native QEMU run for $Build is already active."
    }

    $runner = $null
    $success = $false
    $savedLog = Join-Path $matrixDirectory ("qemu-serial-$cpuCount.log")
    try {
        # 子包装器也会截断此文件，但它是异步启动的。先删除上次运行的文件，避免轮询循环
        # 将过期成功标记误认为本次 CPU 已启动。
        Remove-Item -LiteralPath $serialLog -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $savedLog -Force -ErrorAction SilentlyContinue
        $runArguments = @(
            '-NoProfile',
            '-ExecutionPolicy', 'Bypass',
            '-File', $runScript,
            '-Build', $Build,
            '-Cpu', [string]$cpuCount
        )
        if ($NvmeRoot) {
            $runArguments += '-NvmeRoot'
        }
        $runner = Start-Process -FilePath powershell.exe -WindowStyle Hidden -PassThru `
            -ArgumentList $runArguments

        $deadline = (Get-Date).AddSeconds($Seconds)
        while ((Get-Date) -lt $deadline) {
            if (Test-Path -LiteralPath $serialLog) {
                $serial = Get-Content -LiteralPath $serialLog -Raw -ErrorAction SilentlyContinue
                if ($serial -match 'OS_LIBC_TEST_OK' -and
                    $serial -match 'OS_USER_RUNTIME_OK' -and
                    $serial -match 'OS_WINDOW_OK' -and
                    $serial -match 'OS_NET_DHCP_OK') {
                    $success = $true
                    break
                }
            }

            if ($runner.HasExited) { break }
            Start-Sleep -Milliseconds 250
        }

        if (Test-Path -LiteralPath $serialLog) {
            Copy-Item -LiteralPath $serialLog -Destination $savedLog -Force
        }
    }
    finally {
        Stop-NativeRun $runner $Build
    }

    if (-not $success) {
        Write-Host "native QEMU matrix: cpu=$cpuCount did not reach libc/runtime/window/network markers"
        $failures++
        continue
    }

    $savedLog = Join-Path $matrixDirectory ("qemu-serial-$cpuCount.log")
    $savedRelative = "$Build/native-matrix/qemu-serial-$cpuCount.log"
    & bash $verifyScript $savedRelative
    if ($LASTEXITCODE -ne 0) {
        Write-Host "native QEMU matrix: cpu=$cpuCount stage verification failed"
        $failures++
        continue
    }

    & bash $verifyLocationsScript $kernelRelative $savedRelative
    if ($LASTEXITCODE -ne 0) {
        Write-Host "native QEMU matrix: cpu=$cpuCount debug location verification failed"
        $failures++
        continue
    }

    $stageCount = @(Select-String -LiteralPath $savedLog -Pattern '^OS_STAGE phase=').Count
    Write-Host "native QEMU matrix: cpu=$cpuCount PASS stages=$stageCount"
}

if ($failures -ne 0) {
    throw "Native QEMU matrix failed for $failures CPU configuration(s)."
}

Write-Host "Native QEMU matrix passed: $($cpuCounts.Count) configuration(s) ($($cpuCounts -join ','))"
