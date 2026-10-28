[CmdletBinding()]
param(
    [string]$Build = 'build/nvidia/phase11',
    [string]$RealTestLog = '',
    [string]$PreviousPhase10Log = ''
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

function Fail-Gate([string]$Message) { throw "P11 Gate failed: $Message" }

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
$testBinary = Join-Path $buildRoot 'esp\tests\p11-triangle'
$loader = Join-Path $buildRoot 'esp\usr\apps\p11-triangle\lib\ld-linux-x86-64.so.2'
$libc = Join-Path $buildRoot 'esp\usr\apps\p11-triangle\lib\libc.so.6'
$appRoot = Join-Path $buildRoot 'esp\usr\apps\p11-triangle'
$phase10Build = Join-Path $repoRoot 'tools\nvidia\phase10_gate.ps1'
$phase10BuildRoot = Join-Path $repoRoot 'build\nvidia\phase10'
$manifest = Join-Path $repoRoot 'build\nvidia\userspace-595.84.json'
$manifestSource = Join-Path $repoRoot 'tools\nvidia\check_userspace_59584.ps1'
$pythonChecker = Join-Path $repoRoot 'tools\nvidia\check_userspace_59584.py'
$importSource = Join-Path $repoRoot 'tools\nvidia\import_userspace_59584.ps1'
$buildSource = Join-Path $repoRoot 'tools\nvidia\phase11_build.ps1'
$stageSource = Join-Path $repoRoot 'tools\nvidia\phase11_stage.ps1'
$testSource = Join-Path $repoRoot 'tools\nvidia\phase11_triangle_test.c'
$runnerSource = Join-Path $repoRoot 'kernel\process\linux_phase11_test.c'
$runtimeSource = Join-Path $repoRoot 'kernel\init\runtime.c'
$linuxAbiSource = Join-Path $repoRoot 'include\kernel\linux_abi.h'
$bridgeSource = Join-Path $repoRoot 'kernel\drivers\gpu\nvidia\rm\nv_rm_vendor_bridge.c'
$firmwareManifest = Join-Path $repoRoot 'build\nvidia\phase5\firmware.manifest'
$logPath = if ([string]::IsNullOrWhiteSpace($RealTestLog)) {
    Join-Path $buildRoot 'realtest-success.log'
} else { Resolve-RepoPath $RealTestLog }

foreach ($required in @(
        @($kernel, 'Phase 11 kernel image'),
        @($testBinary, 'Phase 11 triangle probe'),
        @($loader, 'staged Linux dynamic loader'),
        @($libc, 'staged glibc library'),
        @($phase10Build, 'Phase 10 Gate script'),
        @($phase10BuildRoot, 'Phase 10 build root'),
        @($manifest, '595.84 userspace manifest'),
        @($manifestSource, 'PowerShell userspace checker'),
        @($pythonChecker, 'required Python userspace checker'),
        @($importSource, '595.84 userspace import script'),
        @($buildSource, 'incremental Phase 11 builder'),
        @($stageSource, 'Phase 11 staging script'),
        @($testSource, 'real Phase 11 triangle probe source'),
        @($runnerSource, 'Phase 11 process acceptance runner'),
        @($runtimeSource, 'Phase 11 runtime integration'),
        @($linuxAbiSource, 'Linux ABI phase declarations'),
        @($bridgeSource, 'Phase 11 RM vendor bridge'),
        @($firmwareManifest, '595.84 firmware manifest'),
        @($logPath, 'real Phase 11 test log'))) {
    if ($required[0] -eq $phase10BuildRoot) {
        Require-Directory $required[0] $required[1]
    } else {
        Require-File $required[0] $required[1]
    }
}

$phase10LogPath = $null
if (-not [string]::IsNullOrWhiteSpace($PreviousPhase10Log)) {
    $phase10LogPath = Resolve-RepoPath $PreviousPhase10Log
}
else {
    $hardwareRoot = Join-Path $repoRoot 'build-realtest\real-hardware'
    $candidates = @(Get-ChildItem -LiteralPath $hardwareRoot -Recurse -File `
        -Filter 'realtest.log' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending)
    foreach ($candidate in $candidates) {
        $candidateText = Get-Content -LiteralPath $candidate.FullName -Raw
        $resultPath = Join-Path (Split-Path -Parent $candidate.FullName) 'realtest.result'
        if ((Test-LogRecord $candidateText 'OS_P10_PASS') -and
            (Test-Path -LiteralPath $resultPath -PathType Leaf) -and
            (Get-Content -LiteralPath $resultPath -Raw) -match 'status=pass') {
            $phase10LogPath = $candidate.FullName
            break
        }
    }
}
if ($null -eq $phase10LogPath) { Fail-Gate 'no completed Phase 10 evidence was found' }
Require-File $phase10LogPath 'previous Phase 10 real-test log'
$p10Output = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
    $phase10Build -Build build/nvidia/phase10 -RealTestLog $phase10LogPath 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0 -or
    @($p10Output | Where-Object { $_ -match 'GATE_P10=PASS' }).Count -ne 1) {
    Fail-Gate ('the previous Phase 10 Gate is not passing: {0}' -f
        ($p10Output -join ' '))
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
if ($LASTEXITCODE -ne 0 -or
    @($checkOutput | Where-Object { $_ -match 'CHECK_USERSPACE_59584=PASS' }).Count -ne 1) {
    Fail-Gate ('595.84 userspace closure check failed: {0}' -f
        ($checkOutput -join ' '))
}

$runtimeText = Get-Content -LiteralPath $runtimeSource -Raw
$runnerText = Get-Content -LiteralPath $runnerSource -Raw
$testText = Get-Content -LiteralPath $testSource -Raw
$buildText = Get-Content -LiteralPath $buildSource -Raw
foreach ($requiredText in @(
        'OS_PHASE11', 'linux_phase11_realtest',
        'OS_P11_NVIDIA_GSP_READY', 'OS_P11_TRIANGLE_BEGIN')) {
    Require-Text $runtimeText $requiredText 'Phase 11 runtime path'
}
foreach ($requiredText in @(
        'process_load_elf_image', 'thread_create_user_suspended',
        'thread_release_execution_ref', 'OS_P11_PROCESS_CLEANUP_PASS')) {
    Require-Text $runnerText $requiredText 'Phase 11 process cleanup path'
}
foreach ($requiredText in @(
        'eglQueryDevicesEXT', 'eglGetPlatformDisplayEXT', 'eglInitialize',
        'eglBindAPI', 'eglCreateContext', 'eglMakeCurrent',
        'glCreateShader', 'glBufferData', 'glDrawArrays', 'glFinish',
        'glReadPixels', 'OS_P11_TRIANGLE_PIXEL_PASS',
        'P11_FRAME_COUNT', 'OS_P11_STRESS_PASS frames=')) {
    Require-Text $testText $requiredText 'real OpenGL triangle probe'
}
foreach ($requiredText in @(
        'phase10_gate.ps1', '-DOS_PHASE11=1',
        '-DOS_NVIDIA_PHASE6=0')) {
    Require-Text $buildText $requiredText 'incremental Phase 11 builder'
}
foreach ($requiredText in @(
        'NV_ESC_RM_ALLOC_OBJECT', 'Nv01AllocObjectWithSecInfo',
        'NV_ESC_RM_ALLOC_CONTEXT_DMA2',
        'Nv04AllocContextDmaWithSecInfo',
        'NV_ESC_RM_BIND_CONTEXT_DMA',
        'Nv04BindContextDmaWithSecInfo',
        'NV_ESC_RM_MAP_MEMORY_DMA',
        'Nv04MapMemoryDmaWithSecInfo',
        'NV_ESC_RM_UNMAP_MEMORY_DMA',
        'Nv04UnmapMemoryDmaWithSecInfo',
        'NV_ESC_RM_UPDATE_DEVICE_MAPPING_INFO',
        'rm_update_device_mapping_info')) {
    Require-Text (Get-Content -LiteralPath $bridgeSource -Raw) $requiredText
        '595.84 RM mapping dispatch'
}

$readelf = Get-Command x86_64-elf-readelf -ErrorAction SilentlyContinue
if ($null -eq $readelf) { $readelf = Get-Command readelf -ErrorAction Stop }
$testHeader = @(& $readelf.Source '-hW' '--' $testBinary 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0 -or
    @($testHeader | Where-Object { $_ -match 'Class:\s+ELF64' }).Count -ne 1 -or
    @($testHeader | Where-Object { $_ -match 'Type:\s+EXEC' }).Count -ne 1 -or
    @($testHeader | Where-Object { $_ -match 'Machine:\s+.*X86-64' }).Count -ne 1) {
    Fail-Gate 'Phase 11 probe is not an x86-64 ELF executable'
}
$testProgram = @(& $readelf.Source '-lW' '--' $testBinary 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0 -or
    @($testProgram | Where-Object {
        $_ -match 'Requesting program interpreter:\s+/usr/apps/p11-triangle/lib/ld-linux-x86-64\.so\.2'
    }).Count -ne 1) {
    Fail-Gate 'Phase 11 probe does not carry the required PT_INTERP'
}
$testDynamic = @(& $readelf.Source '-dW' '--' $testBinary 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0 -or
    @($testDynamic | Where-Object { $_ -match 'Shared library:\s+\[libc\.so\.6\]' }).Count -ne 1 -or
    @($testDynamic | Where-Object { $_ -match '/usr/apps/p11-triangle/lib' }).Count -ne 1) {
    Fail-Gate 'Phase 11 probe has no app-private RPATH or libc dependency'
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
        'OS_P11_NVIDIA_GSP_READY',
        'OS_P11_TRIANGLE_BEGIN',
        'OS_P11_EGL_PROBE_BEGIN',
        'OS_P11_NVIDIA_DEVICE_PASS',
        'OS_P11_EGL_INITIALIZED',
        'OS_P11_CONTEXT_CURRENT',
        'OS_P11_GL_VENDOR=NVIDIA',
        'OS_P11_GL_RENDERER=',
        'OS_P11_TRIANGLE_PIXEL_PASS',
        'OS_P11_STRESS_PASS frames=20',
        'OS_P11_PROCESS_CLEANUP_PASS',
        'OS_P11_PASS',
        'OS_REALTEST_PASS',
        'OS_REALTEST_REBOOTING')) {
    if (-not (Test-LogRecord $realText $requiredMarker)) {
        Fail-Gate "required triangle record is absent: $requiredMarker"
    }
}
if ($realText -notmatch 'OS_NVIDIA_PHASE4_HAL_READY BLACKWELL GB205') {
    Fail-Gate 'real triangle evidence is not tied to the GB205 kernel path'
}
if ($realText -match 'OS_P11_[A-Z0-9_]*FAIL' -or
    $realText -match 'OS_REALTEST_FAIL' -or
    $realText -match '(?i)(GPU_FAULT|GSP_TIMEOUT|OS_P11_.*LEAK)' -or
    $realText -match '(?i)OS_P11.*ENOSYS') {
    Fail-Gate 'real Phase 11 log contains a failure, fault, leak, or ENOSYS marker'
}

$resultPath = Join-Path (Split-Path -Parent $logPath) 'realtest.result'
Require-File $resultPath 'Phase 11 real-test result'
if ((Get-Content -LiteralPath $resultPath -Raw) -notmatch 'status=pass') {
    Fail-Gate 'Phase 11 real-test result is not passing'
}

Write-Output 'P11_STATIC=PASS'
Write-Output 'P10_PREREQUISITE_GATE=PASS'
Write-Output 'USERSPACE_59584_GATE=PASS'
Write-Output 'REAL_BINARY_GATE=PASS'
Write-Output 'GATE_P11=PASS'
