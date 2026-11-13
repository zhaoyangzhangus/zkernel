param(
    [int]$GdbPort = 1234,
    [string]$Build = ''
)

$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$buildName = if ($Build) { $Build } elseif ($env:OS_BUILD) { $env:OS_BUILD } elseif ($env:BUILD) { $env:BUILD } else { 'build' }
if ([IO.Path]::IsPathRooted($buildName) -or $buildName -match '(^|[\\/])\.\.(?:[\\/]|$)') {
    throw "Build directory must be relative to the project: $buildName"
}
$pidFile = Join-Path (Join-Path $projectRoot $buildName) 'qemu-native-debug.pid'

function Test-IsQemuProcess([System.Diagnostics.Process]$process) {
    if (-not $process) { return $false }
    try {
        if ($process.ProcessName -ine 'qemu-system-x86_64') { return $false }
        $processPath = $process.Path
        if (-not $processPath) { return $true }
        return ([IO.Path]::GetFileName($processPath) -ieq 'qemu-system-x86_64.exe')
    }
    catch {
        return $false
    }
}

function Stop-QemuProcess([System.Diagnostics.Process]$process) {
    if (-not $process) { return }
    try { $process.Refresh() } catch { return }
    if ($process.HasExited) { return }
    try { $process.CloseMainWindow() | Out-Null } catch { }
    try {
        if (-not $process.WaitForExit(1000)) {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
            $null = $process.WaitForExit(1000)
        }
    }
    catch {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
    }
}

if (Test-Path -LiteralPath $pidFile) {
    $rawPid = (Get-Content -LiteralPath $pidFile -Raw).Trim()
    $qemuPid = 0
    if ([int]::TryParse($rawPid, [ref]$qemuPid) -and $qemuPid -gt 0) {
        $process = Get-Process -Id $qemuPid -ErrorAction SilentlyContinue
        if ($process -and (Test-IsQemuProcess $process)) {
            Stop-QemuProcess $process
        }
    }
    Remove-Item -LiteralPath $pidFile -Force -ErrorAction SilentlyContinue
}

# 同时清理由于提前终止而未能写入/删除 PID 文件的包装器。候选进程仅限监听指定调试端口的 QEMU。
$listeners = @(Get-NetTCPConnection -State Listen -LocalPort $GdbPort -ErrorAction SilentlyContinue)
foreach ($listener in $listeners) {
    $process = Get-Process -Id $listener.OwningProcess -ErrorAction SilentlyContinue
    if ($process -and (Test-IsQemuProcess $process)) {
        Stop-QemuProcess $process
    }
}
