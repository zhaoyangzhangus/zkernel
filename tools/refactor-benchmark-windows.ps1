param(
    [string]$Build = 'build-refactor',
    [int]$Runs = 5,
    [int]$Cpu = 4,
    [int]$Seconds = 45,
    [ValidateSet('usb', 'nvme')]
    [string]$Storage = 'usb',
    [string]$OutputPath = '',
    [string]$ComparePath = '',
    [double]$GatePercent = 1,
    [switch]$NoBuild,
    [switch]$SkipStageVerification
)

$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

if ([IO.Path]::IsPathRooted($Build) -or
    $Build -match '(^|[\\/])\.\.(?:[\\/]|$)') {
    throw "Build directory must be relative to the project: $Build"
}
if ($Runs -lt 1 -or $Cpu -lt 1 -or $Seconds -lt 1) {
    throw 'Runs, Cpu, and Seconds must be positive.'
}
if ($GatePercent -lt 0) {
    throw 'GatePercent cannot be negative.'
}

$buildDirectory = Join-Path $projectRoot $Build
$benchmarkDirectory = Join-Path $buildDirectory 'native-benchmark'
$serialLog = Join-Path $buildDirectory 'qemu-native-serial.log'
$runScript = Join-Path $projectRoot 'run-qemu-windows.ps1'
$verifyLocationsScript = 'tools/verify-debug-locations.sh'
$verifySchemaScript = 'tools/verify-benchmark-schema.sh'

New-Item -ItemType Directory -Force -Path $benchmarkDirectory | Out-Null
Set-Location $projectRoot

if (-not $NoBuild) {
    $buildScript = Join-Path $projectRoot 'tools\build-windows.ps1'
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $buildScript `
        -Build $Build -Debug 2 -Serial
    if ($LASTEXITCODE -ne 0) {
        throw "Native System build failed ($LASTEXITCODE)."
    }
}

$kernelPath = Join-Path $buildDirectory 'esp\EFI\OS\kernel.elf'
$kernelRelative = "$Build/esp/EFI/OS/kernel.elf"
if (-not (Test-Path -LiteralPath $kernelPath)) {
    throw "Kernel ELF not found in $Build."
}

function Get-MatchingQemu([string]$buildName) {
    return @(
        Get-CimInstance Win32_Process -Filter "Name = 'qemu-system-x86_64.exe'" `
            -ErrorAction SilentlyContinue |
            Where-Object {
                $_.CommandLine -and
                $_.CommandLine -like '*qemu-native-fat.img*' -and
                $_.CommandLine -like "*$buildName*"
            }
    )
}

function Stop-NativeRun([System.Diagnostics.Process]$runner,
                        [string]$buildName) {
    foreach ($record in @(Get-MatchingQemu $buildName)) {
        $process = Get-Process -Id ([int]$record.ProcessId) -ErrorAction SilentlyContinue
        if (-not $process) { continue }
        try {
            if (-not $process.HasExited) {
                $null = $process.Kill()
                $null = $process.WaitForExit(1000)
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

    # 包装器可能在 Start-Process 启动后、创建子进程前被终止。重新扫描进程，避免失败轮次
    # 遗留 QEMU 进程。
    foreach ($record in @(Get-MatchingQemu $buildName)) {
        Stop-Process -Id ([int]$record.ProcessId) -Force -ErrorAction SilentlyContinue
    }
}

function Read-BenchmarkMetrics([string]$path) {
    $values = @{}
    foreach ($line in Get-Content -LiteralPath $path) {
        if ($line -match '^OS_BENCH(?:_VALUE)?\s+name=(\S+)\s+(?:cycles|value)=(\d+)') {
            $values[$Matches[1]] = [UInt64]$Matches[2]
            continue
        }
        if ($line -match '^OS_PERF_KMALLOC_OK\b') {
            foreach ($field in [regex]::Matches($line, '\b(MIN|MEDIAN|P95|AVG|MAX)=(\d+)')) {
                $name = switch ($field.Groups[1].Value) {
                    'MIN' { 'kmalloc.min_tsc' }
                    'MEDIAN' { 'kmalloc.median_tsc' }
                    'P95' { 'kmalloc.p95_tsc' }
                    'AVG' { 'kmalloc.average_tsc' }
                    'MAX' { 'kmalloc.max_tsc' }
                }
                $values[$name] = [UInt64]$field.Groups[2].Value
            }
        }
    }
    return [pscustomobject]@{ Values = $values }
}

function Read-SharedText([string]$path) {
    $stream = [IO.File]::Open(
        $path,
        [IO.FileMode]::Open,
        [IO.FileAccess]::Read,
        [IO.FileShare]::ReadWrite)
    try {
        $reader = New-Object -TypeName System.IO.StreamReader -ArgumentList @(
            $stream,
            [Text.Encoding]::ASCII,
            $false,
            4096,
            $true)
        try {
            return $reader.ReadToEnd()
        }
        finally {
            $reader.Dispose()
        }
    }
    finally {
        $stream.Dispose()
    }
}

function Get-MetricStats([UInt64[]]$values) {
    $sorted = @($values | Sort-Object)
    $count = $sorted.Count
    if ($count -eq 0) { throw 'Cannot calculate statistics for an empty metric.' }

    $median = if (($count % 2) -eq 1) {
        $sorted[[int](($count - 1) / 2)]
    }
    else {
        [UInt64](($sorted[$count / 2 - 1] + $sorted[$count / 2]) / 2)
    }
    $p95Index = [int][Math]::Ceiling($count * 0.95) - 1
    if ($p95Index -lt 0) { $p95Index = 0 }
    [pscustomobject]@{
        Count = $count
        Min = $sorted[0]
        Median = $median
        P95 = $sorted[$p95Index]
        Max = $sorted[$count - 1]
    }
}

function Invoke-StageVerification([string]$relativeLogPath) {
    # 向 MSYS bash 传递项目相对路径。绝对 Windows 路径会在 shell 读取前被参数转换改写为
    # C:Users...。
    & bash $verifyLocationsScript $kernelRelative $relativeLogPath
    if ($LASTEXITCODE -ne 0) {
        throw "Stage/DWARF location verification failed for $relativeLogPath."
    }
    & bash $verifySchemaScript $Storage.ToLowerInvariant() $relativeLogPath
    if ($LASTEXITCODE -ne 0) {
        throw "Benchmark schema verification failed for $relativeLogPath."
    }
}

$rawRows = @()
$metricNames = $null

for ($run = 1; $run -le $Runs; ++$run) {
    $savedLog = Join-Path $benchmarkDirectory ("qemu-serial-$run.log")
    $savedRelative = "$Build/native-benchmark/qemu-serial-$run.log"
    $runner = $null
    $success = $false
    $runError = $null

    try {
        # 启动器也会截断该文件，但它是异步启动的。先删除两个文件，避免过期标记满足本轮
        # 的轮询条件。
        Remove-Item -LiteralPath $serialLog -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $savedLog -Force -ErrorAction SilentlyContinue

        $qemuArguments = @(
            '-NoProfile',
            '-ExecutionPolicy', 'Bypass',
            '-File', $runScript,
            '-Build', $Build,
            '-Cpu', [string]$Cpu,
            '-Headless',
            '-NoMouse'
        )
        if ($Storage -eq 'nvme') {
            $qemuArguments += '-NvmeRoot'
        }
        $runner = Start-Process -FilePath powershell.exe -WindowStyle Hidden -PassThru `
            -ArgumentList $qemuArguments

        # Seconds 是基准测试轮次的固定时长，而不只是启动标记超时时间。运行时/窗口/网络
        # 标记出现后仍保持 QEMU 运行，以便观察启动后的卡死情况，并确保每轮工作负载的
        # 墙上时钟时长相同。
        $deadline = (Get-Date).AddSeconds($Seconds)
        while ((Get-Date) -lt $deadline) {
            if (Test-Path -LiteralPath $serialLog) {
                $serial = Read-SharedText $serialLog
                if (-not $success -and
                    $serial.Contains('OS_USER_RUNTIME_OK') -and
                    $serial.Contains('OS_USER_RUNTIME_SUBTESTS_OK') -and
                    $serial.Contains('OS_USERMODE_OK') -and
                    $serial.Contains('OS_WINDOW_OK') -and
                    $serial.Contains('OS_NET_DHCP_OK')) {
                    $success = $true
                }
            }
            if ($runner.HasExited) {
                if ($success) {
                    $runError = "QEMU round $run exited before the fixed $Seconds-second benchmark window completed."
                }
                break
            }
            Start-Sleep -Milliseconds 250
        }

        if (Test-Path -LiteralPath $serialLog) {
            Copy-Item -LiteralPath $serialLog -Destination $savedLog -Force
        }
        if (-not $success -and -not $runError) {
            $runError = "QEMU round $run did not reach the runtime/window/network markers within $Seconds seconds."
        }
    }
    catch {
        $runError = $_.Exception.Message
    }
    finally {
        Stop-NativeRun $runner $Build
    }

    if ($runError) {
        if (Test-Path -LiteralPath $savedLog) {
            Select-String -LiteralPath $savedLog `
                -Pattern 'FAIL|ERROR|FATAL|PANIC|FAULT|TIMEOUT|ASSERT' |
                Select-Object -Last 24 | ForEach-Object { $_.Line }
        }
        throw $runError
    }

    $storageMarker = if ($Storage -eq 'nvme') {
        'OS_NVME_HW_OK'
    }
    else {
        'OS_USB_MSC_FOUND'
    }
    if (-not (Read-SharedText $savedLog).Contains($storageMarker)) {
        throw "QEMU round $run did not produce the requested $Storage storage marker: $storageMarker"
    }

    if ($SkipStageVerification) {
        Write-Host "Native benchmark: round=$run stage verification skipped for historical baseline"
    }
    else {
        Invoke-StageVerification $savedRelative
    }
    $parsed = Read-BenchmarkMetrics $savedLog
    $values = $parsed.Values
    if ($values.Count -eq 0) {
        throw "QEMU round $run produced no OS_BENCH or kmalloc metrics."
    }
    if ($null -eq $metricNames) {
        $metricNames = @($values.Keys | Sort-Object)
    }
    else {
        $missing = @($metricNames | Where-Object { -not $values.ContainsKey($_) })
        if ($missing.Count -ne 0) {
            throw "QEMU round $run is missing metrics: $($missing -join ', ')"
        }
    }
    foreach ($metric in $metricNames) {
        $rawRows += [pscustomobject]@{
            Metric = $metric
            Run = $run
            Value = [UInt64]$values[$metric]
        }
    }
    Write-Host "Native benchmark: round=$run PASS metrics=$($metricNames.Count)"
}

if (-not $OutputPath) {
    $OutputPath = Join-Path $benchmarkDirectory `
        ("baseline-windows-{0}.tsv" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
elseif (-not [IO.Path]::IsPathRooted($OutputPath)) {
    $OutputPath = Join-Path $projectRoot $OutputPath
}
$outputParent = Split-Path -Parent $OutputPath
New-Item -ItemType Directory -Force -Path $outputParent | Out-Null

$statsRows = @()
foreach ($metric in $metricNames) {
    $metricValues = @($rawRows |
        Where-Object { $_.Metric -eq $metric } |
        ForEach-Object { [UInt64]$_.Value })
    $stats = Get-MetricStats $metricValues
    $statsRows += [pscustomobject]@{
        Metric = $metric
        Count = $stats.Count
        Min = $stats.Min
        Median = $stats.Median
        P95 = $stats.P95
        Max = $stats.Max
    }
}

$temporaryOutput = "$OutputPath.tmp.$PID"
try {
    $lines = @(
        '# System native Windows refactor benchmark baseline v1'
        "# runs=$Runs cpu=$Cpu seconds=$Seconds"
        "# storage=$Storage profile=$(if ($Storage -eq 'nvme') { 'nvme-qemu' } else { 'usb-msc' }) accel=tcg"
        "metric`tcount`tmin`tmedian`tp95`tmax"
    )
    foreach ($row in $statsRows) {
        $lines += "$($row.Metric)`t$($row.Count)`t$($row.Min)`t$($row.Median)`t$($row.P95)`t$($row.Max)"
    }
    [IO.File]::WriteAllLines($temporaryOutput, $lines,
                             [Text.Encoding]::ASCII)
    Move-Item -LiteralPath $temporaryOutput -Destination $OutputPath -Force
}
finally {
    Remove-Item -LiteralPath $temporaryOutput -Force -ErrorAction SilentlyContinue
}

Write-Host "Native benchmark baseline written: $OutputPath"

if ($ComparePath) {
    if (-not [IO.Path]::IsPathRooted($ComparePath)) {
        $ComparePath = Join-Path $projectRoot $ComparePath
    }
    if (-not (Test-Path -LiteralPath $ComparePath)) {
        throw "Comparison baseline not found: $ComparePath"
    }

    function Get-BaselineMetadata([string]$path, [string]$key) {
        $line = Get-Content -LiteralPath $path |
            Where-Object { $_ -like '# *' -and $_ -match "\b$key=[^ ]+" } |
            Select-Object -First 1
        if ($line -match "\b$key=([^ ]+)") { return $Matches[1] }
        return ''
    }

    foreach ($key in @('runs', 'cpu', 'seconds', 'storage', 'profile', 'accel')) {
        $currentMetadata = Get-BaselineMetadata $OutputPath $key
        $compareMetadata = Get-BaselineMetadata $ComparePath $key
        if (-not $currentMetadata -or -not $compareMetadata -or
            $currentMetadata -ne $compareMetadata) {
            throw "Benchmark metadata mismatch for ${key}: current=$currentMetadata compare=$compareMetadata"
        }
    }

    $compareRows = @{}
    foreach ($line in Get-Content -LiteralPath $ComparePath) {
        if ($line -match '^(\S+)\t\d+\t\d+\t(\d+)\t(\d+)\t\d+$') {
            $compareRows[$Matches[1]] = [pscustomobject]@{
                Median = [double]$Matches[2]
                P95 = [double]$Matches[3]
            }
        }
    }

    $regressions = @()
    foreach ($row in $statsRows) {
        if (-not $compareRows.ContainsKey($row.Metric)) {
            $regressions += "$($row.Metric): missing from comparison baseline"
            continue
        }
        $baseline = $compareRows[$row.Metric]
        $limit = 1.0 + ($GatePercent / 100.0)
        if ([double]$row.Median -gt $baseline.Median * $limit) {
            $regressions += "$($row.Metric) median $($row.Median) > $($baseline.Median) (+$GatePercent%)"
        }
        if ([double]$row.P95 -gt $baseline.P95 * $limit) {
            $regressions += "$($row.Metric) p95 $($row.P95) > $($baseline.P95) (+$GatePercent%)"
        }
    }
    if ($regressions.Count -ne 0) {
        $regressions | ForEach-Object { Write-Error "Benchmark regression: $_" }
        throw "Native benchmark regression exceeded $GatePercent%."
    }
    Write-Host "Native benchmark comparison PASS: gate=$GatePercent%"
}
else {
    Write-Host 'Native benchmark comparison not requested; this file is diagnostic evidence until a matched pre-change baseline is supplied.'
}
