param(
    [string]$Build = '',
    [int]$Debug = 2,
    [switch]$Serial,
    [switch]$RealTest,
    [switch]$FailureTest,
    [switch]$DiagnosticBot,
    [switch]$Force,
    [string]$ToolPrefix = '',
    [string]$ElfToolPrefix = '',
    [string]$MakePath = ''
)

$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

function Assert-RelativeBuild([string]$name) {
    if (-not $name -or [IO.Path]::IsPathRooted($name) -or
        $name -match '(^|[\\/])\.\.(?:[\\/]|$)') {
        throw "Build directory must be relative to the project: $name"
    }
}

$buildName = if ($Build) {
    $Build
} elseif ($env:OS_BUILD) {
    $env:OS_BUILD
} elseif ($env:BUILD) {
    $env:BUILD
} else {
    'build'
}
Assert-RelativeBuild $buildName

if ($Debug -lt 0 -or $Debug -gt 2) {
    throw "Debug must be 0, 1, or 2: $Debug"
}

function Resolve-Executable([string]$explicitPath, [string[]]$names) {
    if ($explicitPath) {
        if (-not (Test-Path -LiteralPath $explicitPath)) {
            throw "Executable not found: $explicitPath"
        }
        return (Resolve-Path -LiteralPath $explicitPath).Path
    }

    foreach ($name in $names) {
        $command = Get-Command $name -ErrorAction SilentlyContinue
        if ($command) { return $command.Source }
    }
    return $null
}

# 将原生 GNU 工具放在前面。Windows 在 system32 中提供了不同的 find.exe；GNU make 展开
# 源清单时会调用 find。
$nativeBinCandidates = @(
    'C:\Program Files\w64devkit\bin',
    'C:\w64devkit\bin',
    (Join-Path $projectRoot 'w64devkit\bin')
)
$nativeBin = $nativeBinCandidates |
    Where-Object { Test-Path -LiteralPath $_ } |
    Select-Object -First 1
if ($nativeBin) {
    $env:Path = "$nativeBin;$env:Path"
}

$elfBinCandidates = @(
    'C:\Program Files\x86_64-elf-tools-windows\bin',
    'C:\x86_64-elf-tools-windows\bin',
    (Join-Path $projectRoot 'x86_64-elf-tools\bin')
)
$elfBin = $elfBinCandidates |
    Where-Object { Test-Path -LiteralPath $_ } |
    Select-Object -First 1
if ($elfBin) {
    $env:Path = "$elfBin;$env:Path"
}

$make = Resolve-Executable $MakePath @('make.exe', 'mingw32-make.exe')
if (-not $make) {
    throw 'GNU make was not found. Add w64devkit\bin to PATH or pass -MakePath.'
}

$prefix = if ($ToolPrefix) {
    $ToolPrefix
} elseif ($env:OS_TOOLPREFIX) {
    $env:OS_TOOLPREFIX
} elseif ($env:TOOLPREFIX) {
    $env:TOOLPREFIX
} else {
    'x86_64-w64-mingw32-'
}

$elfPrefix = if ($ElfToolPrefix) {
    $ElfToolPrefix
} elseif ($env:OS_ELFTOOLPREFIX) {
    $env:OS_ELFTOOLPREFIX
} elseif ($env:ELFTOOLPREFIX) {
    $env:ELFTOOLPREFIX
} else {
    'x86_64-elf-'
}

$toolNames = @('gcc.exe', 'ar.exe', 'objcopy.exe', 'objdump.exe')
foreach ($toolName in $toolNames) {
    $tool = Resolve-Executable '' @($prefix + $toolName)
    if (-not $tool) {
        throw "Missing native target tool: $prefix$toolName. The System UEFI build requires the MinGW PE/COFF toolchain."
    }
}

foreach ($toolName in @('gcc.exe', 'ld.exe', 'objcopy.exe', 'objdump.exe')) {
    $tool = Resolve-Executable '' @($elfPrefix + $toolName)
    if (-not $tool) {
        throw "Missing ELF runtime tool: $elfPrefix$toolName. Install the x86_64-elf toolchain or pass -ElfToolPrefix."
    }
}

$hostCompiler = Resolve-Executable '' @('gcc.exe')
if (-not $hostCompiler) {
    throw 'Native host gcc was not found. Install w64devkit and add its bin directory to PATH.'
}

$buildDirectory = Join-Path $projectRoot $buildName
$buildLog = Join-Path $buildDirectory 'windows-build.log'
New-Item -ItemType Directory -Force -Path $buildDirectory | Out-Null
Set-Content -LiteralPath $buildLog -Value '' -NoNewline

function Test-WindowsExecutable([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return $false }
    try {
        $bytes = [IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $path).Path)
        return $bytes.Length -ge 2 -and $bytes[0] -eq 0x4D -and $bytes[1] -eq 0x5A
    }
    catch {
        return $false
    }
}

# WSL 生成的构建目录包含 Linux 宿主辅助工具。只移除这些生成的辅助工具，让 make 使用
# w64devkit 重新构建；目标镜像对象仍可重用，因为它们采用相同的 MinGW 对象格式。
$hostHelpers = @(
    (Join-Path $buildDirectory 'build-id.exe'),
    (Join-Path $buildDirectory 'make-init-image.exe'),
    (Join-Path $buildDirectory 'make-test-elf.exe')
)
foreach ($helper in $hostHelpers) {
    if ((Test-Path -LiteralPath $helper) -and -not (Test-WindowsExecutable $helper)) {
        Remove-Item -LiteralPath $helper -Force
    }
}

$serialValue = if ($Serial) { '1' } else { '0' }
$realTestValue = if ($RealTest) { '1' } else { '0' }
$failureTestValue = if ($FailureTest) { '1' } else { '0' }
$diagnosticBotValue = if ($DiagnosticBot) { '1' } else { '0' }
$makeArguments = @(
    '-f', 'GNUmakefile'
)
if ($Force) { $makeArguments += '-B' }
$makeArguments += @(
    "BUILD=$buildName",
    "TOOLPREFIX=$prefix",
    "ELFTOOLPREFIX=$elfPrefix",
    'HOSTCC=gcc',
    "DEBUG=$Debug",
    "OS_DEBUG_SERIAL=$serialValue",
    "OS_REALTEST=$realTestValue",
    "OS_REALTEST_FAILURE_TEST=$failureTestValue",
    "OS_XHCI_DIAGNOSTIC_BOT=$diagnosticBotValue"
)
$makeArguments += 'esp'

Push-Location $projectRoot
try {
    & $make @makeArguments 2>&1 |
        ForEach-Object {
            $line = [string]$_
            Add-Content -LiteralPath $buildLog -Value $line
            if ($line -match '(?i)(error:|undefined reference|fatal|failed|cannot|invalid|no such file)') {
                Write-Host $line
            }
        }
    $status = $LASTEXITCODE
}
finally {
    Pop-Location
}

if ($status -ne 0) {
    throw "Native System build failed ($status). See $buildLog."
}

$kernel = Join-Path $buildDirectory 'esp\EFI\OS\kernel.elf'
$bootloader = Join-Path $buildDirectory 'esp\EFI\BOOT\BOOTX64.EFI'
if (-not (Test-Path -LiteralPath $kernel) -or -not (Test-Path -LiteralPath $bootloader)) {
    throw "Native build completed without the ESP boot files. See $buildLog."
}

Write-Host "System native build ready: $buildName"
