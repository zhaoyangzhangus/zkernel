param(
    [int]$Cpu = 4,
    [switch]$Debug,
    [switch]$NvmeRoot,
    [switch]$AllDisks,
    [int]$GdbPort = 1234,
    [string]$Build = '',
    [string]$ToolPrefix = ''
)

$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$buildName = if ($Build) { $Build } elseif ($env:OS_BUILD) { $env:OS_BUILD } elseif ($env:BUILD) { $env:BUILD } else { 'build' }
if ([IO.Path]::IsPathRooted($buildName) -or $buildName -match '(^|[\\/])\.\.(?:[\\/]|$)') {
    throw "Build directory must be relative to the project: $buildName"
}
$buildDirectory = Join-Path $projectRoot $buildName
$buildLog = Join-Path $buildDirectory 'qemu-build.log'
$failurePattern = '(^|[^A-Z])(FAIL|FAILED|FAILURE|ERROR|FATAL|PANIC|ABORT|INVALID|CANNOT|UNDEFINED REFERENCE)([^A-Z]|$)'

function Invoke-WslBuild([string]$distro, [string]$command) {
    New-Item -ItemType Directory -Force -Path $buildDirectory | Out-Null
    Set-Content -LiteralPath $buildLog -Value '' -NoNewline

    & wsl.exe -d $distro -- bash -lc $command 2>&1 |
        ForEach-Object {
            $line = [string]$_
            Add-Content -LiteralPath $buildLog -Value $line
            if ($line -match $failurePattern) { Write-Host $line }
        }
    $status = $LASTEXITCODE
    if ($status -ne 0) {
        throw "System build failed in WSL ($status). See $buildLog."
    }
}

function Get-WslDistro {
    $wsl = Get-Command wsl.exe -ErrorAction SilentlyContinue
    if (-not $wsl) { return $null }

    $preferred = @($env:OS_WSL_DISTRO, 'Debian', 'Ubuntu')
    try {
        # 即使未安装 WSL，Windows 可选功能占位程序也可能提供 wsl.exe。
        # 在这种情况下，`wsl -l -q` 会输出错误；Stop 模式下 PowerShell 会将其转换为
        # NativeCommandError。
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
        if ($candidate -and $available -contains $candidate) {
            return $candidate
        }
    }
    if ($available.Count -gt 0) {
        return $available[0]
    }
    return $null
}

function Find-OptionalPath([string[]]$candidates, [string]$commandName) {
    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path -LiteralPath $candidate)) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    $command = Get-Command $commandName -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }
    return $null
}

function Get-WslProjectRoot([string]$distro) {
    if ($projectRoot -notmatch '^[A-Za-z]:\\') {
        throw "Project path is not on a Windows drive: $projectRoot"
    }
    $drive = $projectRoot.Substring(0, 1).ToLowerInvariant()
    $rest = $projectRoot.Substring(2).Replace('\', '/')
    return "/mnt/$drive$rest"
}

function Test-NativeToolchain([string]$prefix) {
    $make = Find-OptionalPath @(
        'C:\Program Files\w64devkit\bin\make.exe',
        'C:\w64devkit\bin\make.exe'
    ) 'make.exe'
    $compiler = Find-OptionalPath @(
        (Join-Path 'C:\Program Files\w64devkit\bin' ($prefix + 'gcc.exe')),
        (Join-Path 'C:\w64devkit\bin' ($prefix + 'gcc.exe'))
    ) ($prefix + 'gcc.exe')
    return $make -and $compiler
}

$distro = Get-WslDistro
$qemu = Find-OptionalPath @(
    'C:\Program Files\qemu\qemu-system-x86_64.exe',
    'C:\Program Files\QEMU\qemu-system-x86_64.exe'
) 'qemu-system-x86_64.exe'

$qemuImg = $null
$firmware = $null
if ($qemu) {
    $qemuDirectory = Split-Path -Parent $qemu
    $qemuImg = Find-OptionalPath @(
        (Join-Path $qemuDirectory 'qemu-img.exe'),
        'C:\Program Files\qemu\qemu-img.exe'
    ) 'qemu-img.exe'
    $firmware = Find-OptionalPath @(
        (Join-Path $qemuDirectory 'share\edk2-x86_64-code.fd'),
        'C:\Program Files\qemu\share\edk2-x86_64-code.fd'
    ) 'edk2-x86_64-code.fd'
}

$useNativeQemu = $qemu -and $qemuImg -and $firmware
$nativePrefix = if ($ToolPrefix) {
    $ToolPrefix
} elseif ($env:OS_TOOLPREFIX) {
    $env:OS_TOOLPREFIX
} elseif ($env:TOOLPREFIX) {
    $env:TOOLPREFIX
} else {
    'x86_64-w64-mingw32-'
}
$useNativeToolchain = Test-NativeToolchain $nativePrefix
$kernel = Join-Path $buildDirectory 'esp\EFI\OS\kernel.elf'
$bootloader = Join-Path $buildDirectory 'esp\EFI\BOOT\BOOTX64.EFI'

if ($useNativeQemu) {
    if ($useNativeToolchain) {
        $buildScript = Join-Path $projectRoot 'tools\build-windows.ps1'
        $buildArguments = @(
            '-Build', $buildName,
            '-Debug', '2',
            '-Serial',
            '-ToolPrefix', $nativePrefix
        )
        Write-Verbose 'Environment: Windows native QEMU + native w64devkit build'
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $buildScript @buildArguments
        if ($LASTEXITCODE -ne 0) {
            throw "Native Windows build failed ($LASTEXITCODE)."
        }
    }
    elseif (-not $distro) {
        if (-not (Test-Path -LiteralPath $kernel) -or
            -not (Test-Path -LiteralPath $bootloader)) {
            throw 'Native QEMU is available, but no native System toolchain or WSL distribution was found.'
        }
        Write-Verbose 'Environment: Windows native QEMU (existing build)'
    } else {
        $wslRoot = Get-WslProjectRoot $distro
        $escapedRoot = $wslRoot.Replace("'", "'\''")
        $escapedBuildName = $buildName.Replace("'", "'\''")
        $buildCommand = "cd '$escapedRoot' && make -f GNUmakefile esp BUILD='$escapedBuildName' DEBUG=2 OS_DEBUG_SERIAL=1 -j4"
        Write-Verbose "Environment: Windows native QEMU + WSL fallback build ($distro)"
        Invoke-WslBuild $distro $buildCommand
    }

    $nativeArguments = @('-Cpu', $Cpu)
    $nativeArguments += @('-Build', $buildName)
    if ($NvmeRoot) {
        $nativeArguments += '-NvmeRoot'
    }
    if ($AllDisks) {
        $nativeArguments += '-AllDisks'
    }
    if ($Debug) {
        $nativeArguments += @('-Debug', '-GdbPort', $GdbPort)
    }
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
        (Join-Path $projectRoot 'run-qemu-windows.ps1') @nativeArguments
    exit $LASTEXITCODE
}

if (-not $distro) {
    throw 'No usable QEMU environment found. Install Windows QEMU or WSL with QEMU/OVMF.'
}

$wslRoot = Get-WslProjectRoot $distro
$escapedRoot = $wslRoot.Replace("'", "'\''")
$escapedBuildName = $buildName.Replace("'", "'\''")
$runArguments = if ($Debug) {
    "./run-qemu.sh --debug --gdb-port $GdbPort --cpu $Cpu"
} else {
    "./run-qemu.sh --keep-open --cpu $Cpu"
}
if ($NvmeRoot) {
    $runArguments += ' --nvme-root'
}
if ($AllDisks) {
    $runArguments += ' --all-disks'
}
$protocolPrefix = if ($Debug) { 'OS_DEBUG_PROTOCOL=1 ' } else { '' }
$runCommand = "cd '$escapedRoot' && BUILD='$escapedBuildName' $protocolPrefix$runArguments"
Write-Verbose "Environment: WSL QEMU ($distro)"
& wsl.exe -d $distro -- bash -lc $runCommand
exit $LASTEXITCODE
