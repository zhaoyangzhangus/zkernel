[CmdletBinding()]
param(
    [string]$Build = 'build/nvidia/phase12',
    [string]$RealTestLog = '',
    [string]$PreviousPhase11Log = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'app-layout.ps1')
. (Join-Path $PSScriptRoot 'log-records.ps1')
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

function Resolve-RepoPath([string]$Path) {
    if ([IO.Path]::IsPathRooted($Path)) { return [IO.Path]::GetFullPath($Path) }
    return [IO.Path]::GetFullPath((Join-Path $repoRoot $Path))
}

function Fail-Gate([string]$Message) { throw "P12 Gate failed: $Message" }

function Require-File([string]$Path, [string]$Description) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        Fail-Gate "missing $Description`: $Path"
    }
}

function Require-Directory([string]$Path, [string]$Description) {
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        Fail-Gate "missing $Description`: $Path"
    }
}

function Require-Text([string]$Text, [string]$Pattern, [string]$Description) {
    if ($Text -notmatch [regex]::Escape($Pattern)) {
        Fail-Gate "required evidence is absent: $Description"
    }
}

$buildRoot = Resolve-RepoPath $Build
Assert-AppGuestLayout (Join-Path $buildRoot 'esp')
$kernel = Join-Path $buildRoot 'esp\EFI\OS\kernel.elf'
$testBinary = Join-Path $buildRoot 'esp\tests\p12-window'
$loader = Join-Path $buildRoot 'esp\usr\apps\p12-window\lib\ld-linux-x86-64.so.2'
$libc = Join-Path $buildRoot 'esp\usr\apps\p12-window\lib\libc.so.6'
$appRoot = Join-Path $buildRoot 'esp\usr\apps\p12-window'
$p11Gate = Join-Path $repoRoot 'tools\nvidia\phase11_gate.ps1'
$p11BuildRoot = Join-Path $repoRoot 'build\nvidia\phase11'
$manifest = Join-Path $repoRoot 'build\nvidia\userspace-595.84.json'
$manifestSource = Join-Path $repoRoot 'tools\nvidia\check_userspace_59584.ps1'
$pythonChecker = Join-Path $repoRoot 'tools\nvidia\check_userspace_59584.py'
$importSource = Join-Path $repoRoot 'tools\nvidia\import_userspace_59584.ps1'
$buildSource = Join-Path $repoRoot 'tools\nvidia\phase12_build.ps1'
$stageSource = Join-Path $repoRoot 'tools\nvidia\phase12_stage.ps1'
$testSource = Join-Path $repoRoot 'tools\nvidia\phase12_window_test.c'
$p11TestSource = Join-Path $repoRoot 'tools\nvidia\phase11_triangle_test.c'
$runnerSource = Join-Path $repoRoot 'kernel\process\linux_phase12_test.c'
$runtimeSource = Join-Path $repoRoot 'kernel\init\runtime.c'
$linuxSource = Join-Path $repoRoot 'kernel\syscall\linux.c'
$linuxHeader = Join-Path $repoRoot 'include\uapi\linux_abi.h'
$windowHeader = Join-Path $repoRoot 'include\kernel\window_server.h'
$firmwareManifest = Join-Path $repoRoot 'build\nvidia\phase5\firmware.manifest'
$logPath = if ([string]::IsNullOrWhiteSpace($RealTestLog)) {
    Join-Path $buildRoot 'realtest-success.log'
} else { Resolve-RepoPath $RealTestLog }

foreach ($required in @(
        @($kernel, 'Phase 12 kernel image'),
        @($testBinary, 'Phase 12 window bridge probe'),
        @($loader, 'staged Linux dynamic loader'),
        @($libc, 'staged glibc library'),
        @($p11Gate, 'Phase 11 Gate script'),
        @($p11BuildRoot, 'Phase 11 build root'),
        @($manifest, '595.84 userspace manifest'),
        @($manifestSource, 'PowerShell userspace checker'),
        @($pythonChecker, 'required Python userspace checker'),
        @($importSource, '595.84 userspace import script'),
        @($buildSource, 'incremental Phase 12 builder'),
        @($stageSource, 'Phase 12 staging script'),
        @($testSource, 'Phase 12 window probe source'),
        @($p11TestSource, 'Phase 11 OpenGL probe dependency'),
        @($runnerSource, 'Phase 12 process acceptance runner'),
        @($runtimeSource, 'Phase 12 runtime integration'),
        @($linuxSource, 'Linux window ABI dispatch'),
        @($linuxHeader, 'Linux window UAPI'),
        @($windowHeader, 'window server bridge API'),
        @($firmwareManifest, '595.84 firmware manifest'),
        @($logPath, 'real Phase 12 test log'))) {
    if ($required[0] -eq $p11BuildRoot) {
        Require-Directory $required[0] $required[1]
    } else {
        Require-File $required[0] $required[1]
    }
}

$phase11LogPath = $null
if (-not [string]::IsNullOrWhiteSpace($PreviousPhase11Log)) {
    $phase11LogPath = Resolve-RepoPath $PreviousPhase11Log
} else {
    $hardwareRoot = Join-Path $repoRoot 'build-realtest\real-hardware'
    $candidates = @(Get-ChildItem -LiteralPath $hardwareRoot -Recurse -File `
        -Filter 'realtest.log' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending)
    foreach ($candidate in $candidates) {
        $candidateText = Get-Content -LiteralPath $candidate.FullName -Raw
        $resultPath = Join-Path (Split-Path -Parent $candidate.FullName) 'realtest.result'
        if ((Test-LogRecord $candidateText 'OS_P11_TRIANGLE_BEGIN') -and
            (Test-LogRecord $candidateText 'OS_P11_PASS') -and
            (-not (Test-LogRecord $candidateText 'OS_P12_PASS')) -and
            (Test-Path -LiteralPath $resultPath -PathType Leaf) -and
            (Get-Content -LiteralPath $resultPath -Raw) -match 'status=pass') {
            $phase11LogPath = $candidate.FullName
            break
        }
    }
}
if ($null -eq $phase11LogPath) { Fail-Gate 'no completed Phase 11 evidence was found' }
Require-File $phase11LogPath 'previous Phase 11 real-test log'
$p11Output = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
    $p11Gate -Build build/nvidia/phase11 -RealTestLog $phase11LogPath 2>&1 |
    ForEach-Object { $_.ToString() })
foreach ($line in $p11Output) { Write-Output $line }
if ($LASTEXITCODE -ne 0 -or
    @($p11Output | Where-Object { $_ -match 'GATE_P11=PASS' }).Count -ne 1) {
    Fail-Gate 'the previous Phase 11 Gate is not passing'
}

$vendorRelative = 'third_party/nvidia/595.84/open-gpu-kernel-modules'
$vendorDiff = @(& git -C $repoRoot diff --name-only -- $vendorRelative 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0) { Fail-Gate 'git vendor diff check failed' }
if (@($vendorDiff | Where-Object { $_.Trim() }).Count -ne 0) {
    Fail-Gate ('vendor source tree is modified: {0}' -f $vendorDiff[0])
}

$checkOutput = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
    $manifestSource -AppRoot $appRoot -Manifest $manifest `
    -PackageRoot build/nvidia/official-595.84/extracted-gitbash 2>&1 |
    ForEach-Object { $_.ToString() })
foreach ($line in $checkOutput) { Write-Output $line }
if ($LASTEXITCODE -ne 0 -or
    @($checkOutput | Where-Object { $_ -match 'CHECK_USERSPACE_59584=PASS' }).Count -ne 1) {
    Fail-Gate '595.84 userspace closure check failed'
}

$runtimeText = Get-Content -LiteralPath $runtimeSource -Raw
$runnerText = Get-Content -LiteralPath $runnerSource -Raw
$linuxText = Get-Content -LiteralPath $linuxSource -Raw
$uapiText = Get-Content -LiteralPath $linuxHeader -Raw
$windowText = Get-Content -LiteralPath $windowHeader -Raw
$testText = Get-Content -LiteralPath $testSource -Raw
$buildText = Get-Content -LiteralPath $buildSource -Raw
$probeText = $testText + [Environment]::NewLine + $p11TestSource +
    [Environment]::NewLine + (Get-Content -LiteralPath $p11TestSource -Raw)
foreach ($requiredText in @(
        'OS_PHASE12', 'linux_phase12_realtest',
        'OS_P12_NVIDIA_GSP_READY', 'OS_P12_WINDOW_BEGIN')) {
    Require-Text $runtimeText $requiredText 'Phase 12 runtime path'
}
foreach ($requiredText in @(
        'window_server_create', 'window_server_map',
        'window_server_set_owner_address', 'thread_create_user_suspended',
        'OS_P12_PROCESS_CLEANUP_PASS')) {
    Require-Text $runnerText $requiredText 'Phase 12 window/process setup'
}
foreach ($requiredText in @(
        'LINUX_FD_WINDOW', 'LINUX_WINDOW_DEVICE_PATH',
        'LINUX_WINDOW_IOCTL_COMMIT', 'linux_window_ioctl',
        'window_server_present', 'window_server_move',
        'window_server_resize', 'window_server_close_owned')) {
    Require-Text $linuxText $requiredText 'Linux window device dispatch'
}
foreach ($requiredText in @(
        'linux_window_command_t', 'LINUX_WINDOW_OPERATION_PRESENT',
        'LINUX_WINDOW_OPERATION_MOVE', 'LINUX_WINDOW_OPERATION_RESIZE',
        'LINUX_WINDOW_OPERATION_CLOSE', 'LINUX_WINDOW_IOCTL_COMMIT')) {
    Require-Text $uapiText $requiredText 'Linux window command ABI'
}
foreach ($requiredText in @(
        'window_server_lookup_owner', 'window_server_present',
        'window_server_move', 'window_server_resize',
        'window_server_close_owned')) {
    Require-Text $windowText $requiredText 'window server bridge API'
}
foreach ($requiredText in @(
        'P11_WINDOW_BRIDGE', 'phase11_triangle_test.c',
        'glReadPixels', 'LINUX_WINDOW_IOCTL_COMMIT',
        'OS_P12_READBACK_HBUFFER_PASS', 'OS_P12_VISIBLE_PASS',
        'OS_P12_MOVE_PASS', 'OS_P12_RESIZE_PASS',
        'OS_P12_CLOSE_PASS', 'OS_P12_PASS')) {
    Require-Text $probeText $requiredText 'CPU readback HBUFFER probe'
}
foreach ($requiredText in @(
        'phase11_gate.ps1', '-DOS_PHASE12=1',
        'phase12_stage.ps1', 'p12-window')) {
    Require-Text $buildText $requiredText 'incremental Phase 12 builder'
}

$readelf = Get-Command x86_64-elf-readelf -ErrorAction SilentlyContinue
if ($null -eq $readelf) { $readelf = Get-Command readelf -ErrorAction Stop }
$testHeader = @(& $readelf.Source '-hW' '--' $testBinary 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0 -or
    @($testHeader | Where-Object { $_ -match 'Class:\s+ELF64' }).Count -ne 1 -or
    @($testHeader | Where-Object { $_ -match 'Type:\s+EXEC' }).Count -ne 1 -or
    @($testHeader | Where-Object { $_ -match 'Machine:\s+.*X86-64' }).Count -ne 1) {
    Fail-Gate 'Phase 12 probe is not an x86-64 ELF executable'
}
$testProgram = @(& $readelf.Source '-lW' '--' $testBinary 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0 -or
    @($testProgram | Where-Object {
        $_ -match 'Requesting program interpreter:\s+/usr/apps/p12-window/lib/ld-linux-x86-64\.so\.2'
    }).Count -ne 1) {
    Fail-Gate 'Phase 12 probe does not carry the required PT_INTERP'
}
$testDynamic = @(& $readelf.Source '-dW' '--' $testBinary 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0 -or
    @($testDynamic | Where-Object { $_ -match 'Shared library:\s+\[libc\.so\.6\]' }).Count -ne 1 -or
    @($testDynamic | Where-Object { $_ -match '/usr/apps/p12-window/lib' }).Count -ne 1) {
    Fail-Gate 'Phase 12 probe has no app-private RPATH or libc dependency'
}

foreach ($relative in @(
        'lib/libEGL.so.1',
        'lib/libEGL_nvidia.so.0',
        'lib/libGLdispatch.so.0',
        'lib/libOpenGL.so.0',
        'lib/libnvidia-eglcore.so.595.84',
        'lib/libnvidia-glcore.so.595.84',
        'lib/libnvidia-glsi.so.595.84',
        'lib/libnvidia-gpucomp.so.595.84',
        'lib/libnvidia-tls.so.595.84',
        'egl/10_nvidia.json')) {
    Require-File (Join-Path $appRoot ($relative.Replace('/', '\'))) `
        "595.84 userspace asset $relative"
}

foreach ($firmwareName in @('gsp_ga10x.bin', 'gsp_tu10x.bin')) {
    $firmware = Join-Path $buildRoot ('esp\EFI\OS\firmware\{0}' -f $firmwareName)
    Require-File $firmware "staged 595.84 firmware $firmwareName"
    $hash = (Get-FileHash -LiteralPath $firmware -Algorithm SHA256).Hash
    $manifestPattern = 'file={0} .*sha256={1} baseline=595\.84' -f
        [regex]::Escape($firmwareName), $hash
    if ((Get-Content -LiteralPath $firmwareManifest -Raw) -notmatch $manifestPattern) {
        Fail-Gate "staged firmware is not the recorded 595.84 artifact: $firmwareName"
    }
}

$realText = Get-Content -LiteralPath $logPath -Raw
foreach ($requiredMarker in @(
        'OS_P12_NVIDIA_GSP_READY',
        'OS_P12_WINDOW_BEGIN',
        'OS_P12_EGL_PROBE_BEGIN',
        'OS_P12_NVIDIA_DEVICE_PASS',
        'OS_P12_EGL_INITIALIZED',
        'OS_P12_CONTEXT_CURRENT',
        'OS_P12_GL_VENDOR=NVIDIA',
        'OS_P12_GL_RENDERER=',
        'OS_P12_READBACK_HBUFFER_PASS',
        'OS_P12_VISIBLE_PASS',
        'OS_P12_MOVE_PASS',
        'OS_P12_RESIZE_PASS',
        'OS_P12_CLOSE_PASS',
        'OS_P12_RENDER_PASS frames=2',
        'OS_P12_PROCESS_CLEANUP_PASS',
        'OS_P12_PASS',
        'OS_REALTEST_PASS',
        'OS_REALTEST_REBOOTING')) {
    if (-not (Test-LogRecord $realText $requiredMarker)) {
        Fail-Gate "required window record is absent: $requiredMarker"
    }
}
if ($realText -notmatch 'OS_NVIDIA_PHASE4_HAL_READY BLACKWELL GB205') {
    Fail-Gate 'window evidence is not tied to the GB205 kernel path'
}
if ($realText -match 'OS_P12_[A-Z0-9_]*FAIL' -or
    $realText -match 'OS_REALTEST_FAIL' -or
    $realText -match '(?i)(GPU_FAULT|GSP_TIMEOUT|OS_P12_.*LEAK)' -or
    $realText -match '(?i)OS_(P11|P12).*ENOSYS') {
    Fail-Gate 'real Phase 12 log contains a failure, fault, leak, or ENOSYS marker'
}

$resultPath = Join-Path (Split-Path -Parent $logPath) 'realtest.result'
Require-File $resultPath 'Phase 12 real-test result'
if ((Get-Content -LiteralPath $resultPath -Raw) -notmatch 'status=pass') {
    Fail-Gate 'Phase 12 real-test result is not status=pass'
}

Write-Output 'P12_STATIC=PASS'
Write-Output 'P11_PREREQUISITE_GATE=PASS'
Write-Output 'USERSPACE_59584_GATE=PASS'
Write-Output 'REAL_BINARY_GATE=PASS'
Write-Output 'GATE_P12=PASS'
