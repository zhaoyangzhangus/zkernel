[CmdletBinding()]
param(
    [int]$TargetBoots = 4,
    [int]$WarmupSeconds = 20,
    [int]$ResultWaitSeconds = 180,
    [switch]$Cleanup
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ($TargetBoots -lt 1 -or $WarmupSeconds -lt 0 -or $ResultWaitSeconds -lt 1) {
    throw 'TargetBoots, WarmupSeconds, and ResultWaitSeconds are invalid.'
}

$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$hardwareRoot = Join-Path $projectRoot 'build-realtest\real-hardware'
$statePath = Join-Path $projectRoot 'build-realtest\phase5-hardware-gate-loop.json'
$loopLogPath = Join-Path $projectRoot 'build-realtest\phase5-hardware-gate-loop.log'
$taskName = 'System-NVIDIA-Phase5-Hardware-Gate'
$scriptPath = (Resolve-Path $MyInvocation.MyCommand.Path).Path
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$interactiveUser = $identity.Name

New-Item -ItemType Directory -Force -Path (Split-Path -Parent $statePath) | Out-Null

function Write-LoopLog([string]$Message) {
    $line = '{0:o} {1}' -f (Get-Date), $Message
    Add-Content -LiteralPath $loopLogPath -Value $line -Encoding UTF8
    Write-Output $line
}

function Remove-LoopTask {
    Unregister-ScheduledTask -TaskName $taskName -Confirm:$false -ErrorAction SilentlyContinue
}

function Save-LoopState([string]$Status, [int]$ValidCount, [string]$Detail,
                        [string]$BaselineRunId = '') {
    [ordered]@{
        status = $Status
        target_boots = $TargetBoots
        valid_boots = $ValidCount
        baseline_run_id = $BaselineRunId
        detail = $Detail
        updated_at = (Get-Date).ToUniversalTime().ToString('o')
    } | ConvertTo-Json | Set-Content -LiteralPath $statePath -Encoding UTF8
}

function Get-LoopState {
    if (-not (Test-Path -LiteralPath $statePath -PathType Leaf)) { return $null }
    return Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
}

function Get-ValidHardwareLogs {
    $seenRuns = @{}
    $files = @(Get-ChildItem -LiteralPath $hardwareRoot -Recurse -File -Filter 'realtest.log' -ErrorAction SilentlyContinue)
    foreach ($file in $files) {
        $text = Get-Content -LiteralPath $file.FullName -Raw
        $required =
            $text -match 'OS_NVIDIA_GSP_BOOT_START' -and
            $text -match 'OS_NVIDIA_FIRMWARE_RESOLVED path=\S+ size=\d+ sha256=\S+ expectation=595\.84 hash_ok=1' -and
            $text -match 'OS_NVIDIA_GSP_BOOT_FINISH status=ready' -and
            $text -match 'OS_NVIDIA_GSP_READY' -and
            $text -match 'OS_NVIDIA_PHASE5_GATE_READY' -and
            $text -match 'OS_REALTEST_PASS' -and
            $text -match 'OS_REALTEST_REBOOTING'
        $forbidden =
            $text -match '(?i)qemu|std\s+vga|bochs|virtual\s+machine' -or
            $text -match '(?i)fake|override|hard.?code|not_linked|probe_fail|GSP_BOOT_FAIL'
        $resultPath = Join-Path (Split-Path -Parent $file.FullName) 'realtest.result'
        $resultText = if (Test-Path -LiteralPath $resultPath -PathType Leaf) {
            Get-Content -LiteralPath $resultPath -Raw
        } else { '' }
        $runMatch = [regex]::Match($text, '(?m)^run_id=([^\r\n]+)')
        if (-not $runMatch.Success) {
            $runMatch = [regex]::Match($resultText, '(?m)^run_id=([^\r\n]+)')
        }
        $statusMatch = [regex]::Match($resultText, '(?m)^status=([^\r\n]+)')
        $resultIsPass = $statusMatch.Success -and
            $statusMatch.Groups[1].Value.Trim() -eq 'pass'
        if ($required -and -not $forbidden -and $runMatch.Success -and
            $resultIsPass -and
            -not $seenRuns.ContainsKey($runMatch.Groups[1].Value)) {
            $seenRuns[$runMatch.Groups[1].Value] = [pscustomobject]@{
                RunId = $runMatch.Groups[1].Value
                Path = $file.FullName
            }
        }
    }
    return @($seenRuns.Values | Sort-Object RunId)
}

function Get-LatestRunDirectory {
    $directories = @(Get-ChildItem -LiteralPath $hardwareRoot -Directory -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending)
    return $directories | Select-Object -First 1
}

function Wait-LatestCompletedRun([string]$AfterRunId = '') {
    $deadline = (Get-Date).AddSeconds($ResultWaitSeconds)
    $baselineResult = $null
    if (-not [string]::IsNullOrWhiteSpace($AfterRunId)) {
        $baselineDirectory = Join-Path $hardwareRoot $AfterRunId
        $baselineResultPath = Join-Path $baselineDirectory 'realtest.result'
        if (Test-Path -LiteralPath $baselineResultPath -PathType Leaf) {
            $baselineText = Get-Content -LiteralPath $baselineResultPath -Raw
            $baselineStatus = [regex]::Match($baselineText, '(?m)^status=([^\r\n]+)')
            $baselineResult = [pscustomobject]@{
                RunId = $AfterRunId
                Status = if ($baselineStatus.Success) { $baselineStatus.Groups[1].Value.Trim() } else { 'missing-status' }
                ResultPath = $baselineResultPath
            }
        }
    }
    do {
        $directories = @(Get-ChildItem -LiteralPath $hardwareRoot -Directory -ErrorAction SilentlyContinue |
            Sort-Object Name -Descending)
        $pendingBoot = $false
        foreach ($directory in $directories) {
            if (-not [string]::IsNullOrWhiteSpace($AfterRunId) -and
                [string]::CompareOrdinal($directory.Name, $AfterRunId) -le 0) {
                continue
            }
            $resultPath = Join-Path $directory.FullName 'realtest.result'
            if (-not (Test-Path -LiteralPath $resultPath -PathType Leaf)) {
                if (Test-Path -LiteralPath (Join-Path $directory.FullName 'reboot-requested.txt') -PathType Leaf) {
                    $pendingBoot = $true
                }
                continue
            }
            $resultText = Get-Content -LiteralPath $resultPath -Raw
            $statusMatch = [regex]::Match($resultText, '(?m)^status=([^\r\n]+)')
            return [pscustomobject]@{
                RunId = $directory.Name
                Status = if ($statusMatch.Success) { $statusMatch.Groups[1].Value.Trim() } else { 'missing-status' }
                ResultPath = $resultPath
            }
        }
        # 如果部署在设置 BootNext 前失败，只会留下预检目录。
        # 从上一次完成的结果开始重试，避免将该废弃目录误认为待启动项。
        if (-not $pendingBoot -and $null -ne $baselineResult) {
            return $baselineResult
        }
        Start-Sleep -Seconds 5
    } while ((Get-Date) -lt $deadline)
    return $null
}

if ($Cleanup) {
    $pausedCount = @(Get-ValidHardwareLogs).Count
    Save-LoopState 'stopped' $pausedCount 'Hardware Gate loop paused; no automatic reboot is scheduled.'
    Remove-LoopTask
    Write-LoopLog 'CLEANUP scheduled task removed.'
    return
}

try {
    $existingTask = Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
    if ($null -eq $existingTask) {
        $actionArguments = '-NoProfile -ExecutionPolicy Bypass -File "{0}" -TargetBoots {1} -WarmupSeconds {2} -ResultWaitSeconds {3}' -f `
            $scriptPath, $TargetBoots, $WarmupSeconds, $ResultWaitSeconds
        $action = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument $actionArguments
        $trigger = New-ScheduledTaskTrigger -AtLogOn -User $interactiveUser
        $principal = New-ScheduledTaskPrincipal -UserId $interactiveUser `
            -LogonType Interactive -RunLevel Highest
        Register-ScheduledTask -TaskName $taskName -Action $action `
            -Trigger $trigger -Principal $principal -Force | Out-Null
        Write-LoopLog ("INSTALLED target={0} warmup={1}s" -f $TargetBoots, $WarmupSeconds)
    }

    if ($WarmupSeconds -gt 0) { Start-Sleep -Seconds $WarmupSeconds }
    if (-not (Test-Path -LiteralPath $hardwareRoot -PathType Container)) {
        throw "Hardware log root is missing: $hardwareRoot"
    }

    $loopState = Get-LoopState
    $baselineRunId = ''
    if ($null -ne $loopState -and $loopState.status -eq 'rebooting' -and
        $loopState.PSObject.Properties.Name -contains 'baseline_run_id') {
        $baselineRunId = [string]$loopState.baseline_run_id
    }
    $latestRun = Wait-LatestCompletedRun $baselineRunId
    if ($null -eq $latestRun) {
        Save-LoopState 'stopped' 0 'No new completed realtest.result appeared before timeout.' $baselineRunId
        Remove-LoopTask
        throw 'No new completed real hardware result appeared before timeout.'
    }
    if ($latestRun.Status -ne 'pass') {
        $validCount = @(Get-ValidHardwareLogs).Count
        Save-LoopState 'stopped' $validCount ("Latest run {0} status={1}." -f $latestRun.RunId, $latestRun.Status) $baselineRunId
        Remove-LoopTask
        throw ("Latest real hardware run failed: {0} status={1}" -f $latestRun.RunId, $latestRun.Status)
    }

    $validLogs = @(Get-ValidHardwareLogs)
    Write-LoopLog ("PASS run={0} valid_boots={1}/{2}" -f $latestRun.RunId, $validLogs.Count, $TargetBoots)
    if ($validLogs.Count -ge $TargetBoots) {
        $gateScript = Join-Path $projectRoot 'tools\nvidia\phase5_gate.ps1'
        $gateArguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
            $gateScript, '-HardwareLog',
            (($validLogs | ForEach-Object { $_.Path }) -join ','))
        $gateOutput = @(& powershell.exe @gateArguments 2>&1 | ForEach-Object { [string]$_ })
        $gateExit = $LASTEXITCODE
        foreach ($line in $gateOutput) { Add-Content -LiteralPath $loopLogPath -Value $line -Encoding UTF8 }
        if ($gateExit -ne 0) {
            Save-LoopState 'stopped' $validLogs.Count ("Phase 5 Gate exited {0}." -f $gateExit)
            Remove-LoopTask
            throw ("Phase 5 Gate failed with exit code {0}." -f $gateExit)
        }
        Save-LoopState 'complete' $validLogs.Count 'GATE_P5=PASS'
        Remove-LoopTask
        Write-LoopLog 'COMPLETE GATE_P5=PASS scheduled task removed.'
        return
    }

    Save-LoopState 'rebooting' $validLogs.Count 'Launching the next incremental real-hardware boot.' $latestRun.RunId
    $deployScript = Join-Path $projectRoot 'tools\run-real-hardware-test.ps1'
    $deployArguments = @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $deployScript,
        '-NvidiaPhase5', '-ReuseNvidiaPhase5Build', '-ForceReboot'
    )
    Write-LoopLog 'DEPLOY incremental Phase 5 image and reboot.'
    $deployOutput = @(& powershell.exe @deployArguments 2>&1 | ForEach-Object { [string]$_ })
    foreach ($line in $deployOutput) { Add-Content -LiteralPath $loopLogPath -Value $line -Encoding UTF8 }
    $deployExit = $LASTEXITCODE
    $deployText = $deployOutput -join "`n"
    if ($deployExit -ne 0 -or
        $deployText -match '(?i)No UEFI firmware entry|failed \(\d+\)') {
        Save-LoopState 'stopped' $validLogs.Count ("Deployment exited {0}." -f $deployExit) $latestRun.RunId
        Remove-LoopTask
        throw ("Incremental deployment failed with exit code {0}." -f $deployExit)
    }
}
catch {
    Write-LoopLog ("STOP {0}" -f $_.Exception.Message)
    exit 1
}
