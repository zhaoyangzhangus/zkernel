param(
    [string]$Build = ''
)

$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
# 始终先执行原生清理。它也会扫描调试端口，以处理包装器取消后 PID 文件已被移除的情况。
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
    (Join-Path $PSScriptRoot 'stop-qemu-native-debug.ps1') -Build $Build
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

function Get-WslDistro {
    $wsl = Get-Command wsl.exe -ErrorAction SilentlyContinue
    if (-not $wsl) { return $null }

    $preferred = @($env:OS_WSL_DISTRO, 'Debian', 'Ubuntu')
    try {
        $rawAvailable = @(& $wsl.Source -l -q 2>$null)
    }
    catch {
        return $null
    }
    if ($LASTEXITCODE -ne 0) { return $null }

    $available = @(
        $rawAvailable |
            ForEach-Object { ("$_" -replace "`0", '').Trim() } |
            Where-Object { $_ }
    )
    foreach ($candidate in $preferred) {
        if ($candidate -and $available -contains $candidate) { return $candidate }
    }
    if ($available.Count -gt 0) { return $available[0] }
    return $null
}

$wslCommand = Get-Command wsl.exe -ErrorAction SilentlyContinue
if (-not $wslCommand) { exit 0 }

$distro = Get-WslDistro
if (-not $distro) { exit 0 }

$drive = $projectRoot.Substring(0, 1).ToLowerInvariant()
$rest = $projectRoot.Substring(2).Replace('\', '/')
$wslRoot = "/mnt/$drive$rest"
$escapedRoot = $wslRoot.Replace("'", "'\''")
$buildName = if ($Build) { $Build } elseif ($env:OS_BUILD) { $env:OS_BUILD } elseif ($env:BUILD) { $env:BUILD } else { 'build' }
$escapedBuildName = $buildName.Replace("'", "'\''")
& wsl.exe -d $distro -- bash -lc "cd '$escapedRoot' && BUILD='$escapedBuildName' ./tools/stop-qemu-debug.sh"
exit $LASTEXITCODE
