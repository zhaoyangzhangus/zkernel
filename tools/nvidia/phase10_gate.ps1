[CmdletBinding()]
param(
    [string]$Build = 'build/nvidia/phase10',
    [string]$RealTestLog = '',
    [string]$PreviousPhase9Log = ''
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

function Fail-Gate([string]$Message) { throw "P10 Gate failed: $Message" }

function Require-File([string]$Path, [string]$Description) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
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
$testBinary = Join-Path $buildRoot 'esp\tests\p10-egl-context'
$loader = Join-Path $buildRoot 'esp\usr\apps\p10-egl-context\lib\ld-linux-x86-64.so.2'
$libc = Join-Path $buildRoot 'esp\usr\apps\p10-egl-context\lib\libc.so.6'
$appRoot = Join-Path $buildRoot 'esp\usr\apps\p10-egl-context'
$manifest = Join-Path $repoRoot 'build\nvidia\userspace-595.84.json'
$manifestSource = Join-Path $repoRoot 'tools\nvidia\check_userspace_59584.ps1'
$pythonChecker = Join-Path $repoRoot 'tools\nvidia\check_userspace_59584.py'
$importSource = Join-Path $repoRoot 'tools\nvidia\import_userspace_59584.ps1'
$buildSource = Join-Path $repoRoot 'tools\nvidia\phase10_build.ps1'
$stageSource = Join-Path $repoRoot 'tools\nvidia\phase10_stage.ps1'
$testSource = Join-Path $repoRoot 'tools\nvidia\phase10_egl_context_test.c'
$runnerSource = Join-Path $repoRoot 'kernel\process\linux_phase10_test.c'
$runtimeSource = Join-Path $repoRoot 'kernel\init\runtime.c'
$linuxAbiSource = Join-Path $repoRoot 'include\kernel\linux_abi.h'
$makeSource = Join-Path $repoRoot 'makefile'
$firmwareManifest = Join-Path $repoRoot 'build\nvidia\phase5\firmware.manifest'
$logPath = if ([string]::IsNullOrWhiteSpace($RealTestLog)) {
    Join-Path $buildRoot 'realtest-success.log'
} else { Resolve-RepoPath $RealTestLog }

foreach ($required in @(
        @($kernel, 'Phase 10 kernel image'),
        @($testBinary, 'Phase 10 EGL/context probe'),
        @($loader, 'staged Linux dynamic loader'),
        @($libc, 'staged glibc library'),
        @($manifest, '595.84 userspace manifest'),
        @($manifestSource, 'PowerShell userspace checker'),
        @($pythonChecker, 'required Python userspace checker'),
        @($importSource, '595.84 userspace import script'),
        @($buildSource, 'incremental Phase 10 builder'),
        @($stageSource, 'Phase 10 staging script'),
        @($testSource, 'real Phase 10 EGL/context probe source'),
        @($runnerSource, 'Phase 10 process acceptance runner'),
        @($runtimeSource, 'Phase 10 runtime integration'),
        @($linuxAbiSource, 'Linux ABI phase declarations'),
        @($makeSource, 'Phase 10 build flags'),
        @($firmwareManifest, '595.84 firmware manifest'),
        @($logPath, 'real Phase 10 test log'))) {
    Require-File $required[0] $required[1]
}

$phase9LogPath = $null
if (-not [string]::IsNullOrWhiteSpace($PreviousPhase9Log)) {
    $phase9LogPath = Resolve-RepoPath $PreviousPhase9Log
}
else {
    $hardwareRoot = Join-Path $repoRoot 'build-realtest\real-hardware'
    $candidates = @(Get-ChildItem -LiteralPath $hardwareRoot -Recurse -File `
        -Filter 'realtest.log' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending)
    foreach ($candidate in $candidates) {
        $candidateText = Get-Content -LiteralPath $candidate.FullName -Raw
        $resultPath = Join-Path (Split-Path -Parent $candidate.FullName) 'realtest.result'
        if ($candidateText -match 'OS_P9_USERSPACE_PROCESS_PASS' -and
            (Test-Path -LiteralPath $resultPath -PathType Leaf) -and
            (Get-Content -LiteralPath $resultPath -Raw) -match 'status=pass') {
            $phase9LogPath = $candidate.FullName
            break
        }
    }
}
if ($null -eq $phase9LogPath) { Fail-Gate 'no completed Phase 9 real-test evidence was found' }
Require-File $phase9LogPath 'previous Phase 9 real-test log'
$p9Gate = Join-Path $repoRoot 'tools\nvidia\phase9_gate.ps1'
Require-File $p9Gate 'Phase 9 Gate script'
$p9Output = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $p9Gate `
    -Build build/nvidia/phase9 -RealTestLog $phase9LogPath 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0 -or
    @($p9Output | Where-Object { $_ -match 'GATE_P9=PASS' }).Count -ne 1) {
    Fail-Gate 'the previous Phase 9 Gate is not passing'
}

$vendorRelative = 'third_party/nvidia/595.84/open-gpu-kernel-modules'
$vendorDiff = @(& git -C $repoRoot diff --name-only -- $vendorRelative 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0) { Fail-Gate 'git vendor diff check failed' }
if (@($vendorDiff | Where-Object { $_.Trim() }).Count -ne 0) {
    Fail-Gate ('vendor source tree is modified: {0}' -f $vendorDiff[0])
}

$checkOutput = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
    $manifestSource -AppRoot $appRoot `
    -Manifest $manifest `
    -PackageRoot build/nvidia/official-595.84/extracted-gitbash 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0 -or
    @($checkOutput | Where-Object { $_ -match 'CHECK_USERSPACE_59584=PASS' }).Count -ne 1) {
    Fail-Gate ('595.84 userspace closure check failed: {0}' -f ($checkOutput -join ' '))
}

$runtimeText = Get-Content -LiteralPath $runtimeSource -Raw
$runnerText = Get-Content -LiteralPath $runnerSource -Raw
$testText = Get-Content -LiteralPath $testSource -Raw
$makeText = Get-Content -LiteralPath $makeSource -Raw
foreach ($requiredText in @(
        'OS_PHASE10', 'linux_phase10_realtest',
        'OS_P10_NVIDIA_GSP_READY', 'OS_P10_EGL_BEGIN',
        'OS_P10_EGL_PROCESS_PASS')) {
    Require-Text $runtimeText $requiredText 'Phase 10 runtime path'
}
foreach ($requiredText in @(
        'process_load_elf_image', 'thread_create_user_suspended',
        'thread_release_execution_ref', 'OS_P10_PROCESS_CLEANUP_PASS')) {
    Require-Text $runnerText $requiredText 'Phase 10 process cleanup path'
}
foreach ($requiredText in @(
        'eglQueryDevicesEXT', 'eglGetPlatformDisplayEXT', 'eglInitialize',
        'eglChooseConfig', 'eglCreateContext', 'eglMakeCurrent',
        'glGetString', 'OS_P10_NVIDIA_DEVICE_PASS',
        'OS_P10_EGL_INITIALIZED', 'OS_P10_CONTEXT_CURRENT',
        'OS_P10_GL_VENDOR=', 'OS_P10_GL_RENDERER=',
        'OS_P10_PASS')) {
    Require-Text $testText $requiredText 'real EGL device/context probe'
}
foreach ($requiredText in @('OS_PHASE10', '-DOS_PHASE10=$(OS_PHASE10)')) {
    Require-Text $makeText $requiredText 'Phase 10 compile definition'
}
$buildText = Get-Content -LiteralPath $buildSource -Raw
Require-Text $buildText '-DOS_NVIDIA_PHASE6=0' `
    'Phase 10 does not rerun the previous Phase 6 stress probe'

$readelf = Get-Command x86_64-elf-readelf -ErrorAction SilentlyContinue
if ($null -eq $readelf) { $readelf = Get-Command readelf -ErrorAction Stop }
$testHeader = @(& $readelf.Source '-hW' '--' $testBinary 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0 -or
    @($testHeader | Where-Object { $_ -match 'Class:\s+ELF64' }).Count -ne 1 -or
    @($testHeader | Where-Object { $_ -match 'Type:\s+EXEC' }).Count -ne 1 -or
    @($testHeader | Where-Object { $_ -match 'Machine:\s+.*X86-64' }).Count -ne 1) {
    Fail-Gate 'Phase 10 probe is not an x86-64 ELF executable'
}
$testProgram = @(& $readelf.Source '-lW' '--' $testBinary 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0 -or
    @($testProgram | Where-Object {
        $_ -match 'Requesting program interpreter:\s+/usr/apps/p10-egl-context/lib/ld-linux-x86-64\.so\.2'
    }).Count -ne 1) {
    Fail-Gate 'Phase 10 probe does not carry the required PT_INTERP'
}
$testDynamic = @(& $readelf.Source '-dW' '--' $testBinary 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0 -or
    @($testDynamic | Where-Object { $_ -match 'Shared library:\s+\[libc\.so\.6\]' }).Count -ne 1 -or
    @($testDynamic | Where-Object { $_ -match '/usr/apps/p10-egl-context/lib' }).Count -ne 1) {
    Fail-Gate 'Phase 10 probe has no app-private RPATH or libc dependency'
}

$requiredLibraries = @(
    'lib/libEGL.so.1',
    'lib/libEGL_nvidia.so.0',
    'lib/libGLdispatch.so.0',
    'lib/libOpenGL.so.0',
    'lib/libnvidia-eglcore.so.595.84',
    'lib/libnvidia-glcore.so.595.84',
    'lib/libnvidia-glsi.so.595.84',
    'lib/libnvidia-gpucomp.so.595.84',
    'lib/libnvidia-tls.so.595.84',
    'egl/10_nvidia.json'
)
foreach ($relative in $requiredLibraries) {
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
        'OS_P10_NVIDIA_GSP_READY',
        'OS_P10_EGL_BEGIN',
        'OS_P10_EGL_PROBE_BEGIN',
        'OS_P10_NVIDIA_DEVICE_PASS',
        'OS_P10_EGL_INITIALIZED',
        'OS_P10_CONTEXT_CURRENT',
        'OS_P10_GL_VENDOR=',
        'OS_P10_GL_RENDERER=',
        'OS_P10_PASS',
        'OS_P10_PROCESS_CLEANUP_PASS',
        'OS_P10_EGL_PROCESS_PASS',
        'OS_REALTEST_PASS',
        'OS_REALTEST_REBOOTING')) {
    if (-not (Test-LogRecord $realText $requiredMarker)) {
        Fail-Gate "required evidence is absent: $requiredMarker"
    }
}
if ($realText -notmatch 'OS_P10_GL_VENDOR=NVIDIA') {
    Fail-Gate 'real EGL context did not report an NVIDIA GL vendor'
}
if ($realText -notmatch 'OS_P10_GL_RENDERER=.+') {
    Fail-Gate 'real EGL context did not report a GL renderer'
}
if ($realText -notmatch 'OS_NVIDIA_PHASE4_HAL_READY BLACKWELL GB205') {
    Fail-Gate 'real EGL evidence is not tied to the GB205 kernel path'
}
if ($realText -match 'OS_P10_[A-Z0-9_]*FAIL' -or
    $realText -match 'OS_REALTEST_FAIL' -or
    $realText -match '(?i)OS_P10.*ENOSYS') {
    Fail-Gate 'real Phase 10 log contains a failure or unexpected ENOSYS marker'
}

Write-Output 'P10_STATIC=PASS'
Write-Output 'USERSPACE_59584_GATE=PASS'
Write-Output 'REAL_BINARY_GATE=PASS'
Write-Output 'GATE_P10=PASS'
