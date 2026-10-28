[CmdletBinding()]
param(
    [string]$Build = 'build/nvidia/phase9',
    [string]$RealTestLog = '',
    [string]$PreviousPhase8Log = ''
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

function Fail-Gate([string]$Message) { throw "P9 Gate failed: $Message" }

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
$testBinary = Join-Path $buildRoot 'esp\tests\p9-userspace-load'
$loader = Join-Path $buildRoot 'esp\usr\apps\p9-userspace-load\lib\ld-linux-x86-64.so.2'
$libc = Join-Path $buildRoot 'esp\usr\apps\p9-userspace-load\lib\libc.so.6'
$appRoot = Join-Path $buildRoot 'esp\usr\apps\p9-userspace-load'
$manifest = Join-Path $repoRoot 'build\nvidia\userspace-595.84.json'
$manifestSource = Join-Path $repoRoot 'tools\nvidia\check_userspace_59584.ps1'
$pythonChecker = Join-Path $repoRoot 'tools\nvidia\check_userspace_59584.py'
$importSource = Join-Path $repoRoot 'tools\nvidia\import_userspace_59584.ps1'
$buildSource = Join-Path $repoRoot 'tools\nvidia\phase9_build.ps1'
$stageSource = Join-Path $repoRoot 'tools\nvidia\phase9_stage.ps1'
$testSource = Join-Path $repoRoot 'tools\nvidia\phase9_userspace_load_test.c'
$runnerSource = Join-Path $repoRoot 'kernel\process\linux_phase9_test.c'
$runtimeSource = Join-Path $repoRoot 'kernel\init\runtime.c'
$linuxAbiSource = Join-Path $repoRoot 'include\kernel\linux_abi.h'
$makeSource = Join-Path $repoRoot 'makefile'
$firmwareManifest = Join-Path $repoRoot 'build\nvidia\phase5\firmware.manifest'
$logPath = if ([string]::IsNullOrWhiteSpace($RealTestLog)) {
    Join-Path $buildRoot 'realtest-success.log'
} else { Resolve-RepoPath $RealTestLog }

foreach ($required in @(
        @($kernel, 'Phase 9 kernel image'),
        @($testBinary, 'Phase 9 loader probe'),
        @($loader, 'staged Linux dynamic loader'),
        @($libc, 'staged glibc library'),
        @($manifest, '595.84 userspace manifest'),
        @($manifestSource, 'PowerShell userspace checker'),
        @($pythonChecker, 'required Python userspace checker'),
        @($importSource, '595.84 userspace import script'),
        @($buildSource, 'incremental Phase 9 builder'),
        @($stageSource, 'Phase 9 staging script'),
        @($testSource, 'real Phase 9 loader probe source'),
        @($runnerSource, 'Phase 9 process acceptance runner'),
        @($runtimeSource, 'Phase 9 runtime integration'),
        @($linuxAbiSource, 'Linux ABI phase declarations'),
        @($makeSource, 'Phase 9 build flags'),
        @($firmwareManifest, '595.84 firmware manifest'),
        @($logPath, 'real Phase 9 test log'))) {
    Require-File $required[0] $required[1]
}

$phase8LogPath = $null
if (-not [string]::IsNullOrWhiteSpace($PreviousPhase8Log)) {
    $phase8LogPath = Resolve-RepoPath $PreviousPhase8Log
}
else {
    $hardwareRoot = Join-Path $repoRoot 'build-realtest\real-hardware'
    $candidates = @(Get-ChildItem -LiteralPath $hardwareRoot -Recurse -File `
        -Filter 'realtest.log' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending)
    foreach ($candidate in $candidates) {
        $candidateText = Get-Content -LiteralPath $candidate.FullName -Raw
        $resultPath = Join-Path (Split-Path -Parent $candidate.FullName) 'realtest.result'
        if ($candidateText -match 'OS_P8_FRONTEND_PROCESS_PASS' -and
            (Test-Path -LiteralPath $resultPath -PathType Leaf) -and
            (Get-Content -LiteralPath $resultPath -Raw) -match 'status=pass') {
            $phase8LogPath = $candidate.FullName
            break
        }
    }
}
if ($null -eq $phase8LogPath) { Fail-Gate 'no completed Phase 8 real-test evidence was found' }
Require-File $phase8LogPath 'previous Phase 8 real-test log'
$p8Gate = Join-Path $repoRoot 'tools\nvidia\phase8_gate.ps1'
Require-File $p8Gate 'Phase 8 Gate script'
$p8Output = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $p8Gate `
    -Build build/nvidia/phase8 -RealTestLog $phase8LogPath 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0 -or
    @($p8Output | Where-Object { $_ -match 'GATE_P8=PASS' }).Count -ne 1) {
    Fail-Gate 'the previous Phase 8 Gate is not passing'
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
    -Manifest build/nvidia/userspace-595.84.json `
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
        'OS_PHASE9', 'linux_phase9_realtest',
        'OS_P9_NVIDIA_GSP_READY', 'OS_P9_USERSPACE_BEGIN',
        'OS_P9_USERSPACE_PROCESS_PASS')) {
    Require-Text $runtimeText $requiredText 'Phase 9 runtime path'
}
foreach ($requiredText in @(
        'process_load_elf_image', 'thread_create_user_suspended',
        'thread_release_execution_ref', 'OS_P9_PROCESS_CLEANUP_PASS')) {
    Require-Text $runnerText $requiredText 'Phase 9 process cleanup path'
}
foreach ($requiredText in @(
        'dlopen', 'libEGL.so.1', 'libEGL_nvidia.so.0',
        'OS_P9_LIBEGL_PASS', 'OS_P9_LIBEGL_NVIDIA_PASS',
        'OS_P9_LOADER_PASS')) {
    Require-Text $testText $requiredText 'real 595.84 library-load probe'
}
if ($testText -match '(?i)eglGetDisplay|eglInitialize|eglChooseConfig|eglCreateContext|eglMakeCurrent|glClear') {
    Fail-Gate 'P9 probe enters EGL device/context/rendering before Phase 10'
}
foreach ($requiredText in @('OS_PHASE9', '-DOS_PHASE9=$(OS_PHASE9)')) {
    Require-Text $makeText $requiredText 'Phase 9 compile definition'
}

$readelf = Get-Command x86_64-elf-readelf -ErrorAction SilentlyContinue
if ($null -eq $readelf) { $readelf = Get-Command readelf -ErrorAction Stop }
$testHeader = @(& $readelf.Source '-hW' '--' $testBinary 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0 -or
    @($testHeader | Where-Object { $_ -match 'Class:\s+ELF64' }).Count -ne 1 -or
    @($testHeader | Where-Object { $_ -match 'Type:\s+EXEC' }).Count -ne 1 -or
    @($testHeader | Where-Object { $_ -match 'Machine:\s+.*X86-64' }).Count -ne 1) {
    Fail-Gate 'Phase 9 probe is not an x86-64 ELF executable'
}
$testProgram = @(& $readelf.Source '-lW' '--' $testBinary 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0 -or
    @($testProgram | Where-Object {
        $_ -match 'Requesting program interpreter:\s+/usr/apps/p9-userspace-load/lib/ld-linux-x86-64\.so\.2'
    }).Count -ne 1) {
    Fail-Gate 'Phase 9 probe does not carry the required PT_INTERP'
}
$testDynamic = @(& $readelf.Source '-dW' '--' $testBinary 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0 -or
    @($testDynamic | Where-Object { $_ -match 'Shared library:\s+\[libc\.so\.6\]' }).Count -ne 1 -or
    @($testDynamic | Where-Object { $_ -match '/usr/apps/p9-userspace-load/lib' }).Count -ne 1) {
    Fail-Gate 'Phase 9 probe has no app-private RPATH or libc dependency'
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

$manifestText = Get-Content -LiteralPath $manifest -Raw
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
        'OS_P9_NVIDIA_GSP_READY',
        'OS_P9_USERSPACE_BEGIN',
        'OS_P9_LOADER_PROBE_BEGIN',
        'OS_P9_LIBEGL_PASS',
        'OS_P9_LIBEGL_NVIDIA_PASS',
        'OS_P9_LOADER_PASS',
        'OS_P9_PROCESS_CLEANUP_PASS',
        'OS_P9_USERSPACE_PROCESS_PASS',
        'OS_REALTEST_PASS',
        'OS_REALTEST_REBOOTING')) {
    if (-not (Test-LogRecord $realText $requiredMarker)) {
        Fail-Gate "required evidence is absent: $requiredMarker"
    }
}
if ($realText -match 'OS_P9_[A-Z0-9_]*FAIL' -or
    $realText -match 'OS_REALTEST_FAIL' -or
    $realText -match '(?i)OS_P9.*ENOSYS') {
    Fail-Gate 'real Phase 9 log contains a failure or unexpected ENOSYS marker'
}

Write-Output 'P9_STATIC=PASS'
Write-Output 'USERSPACE_59584_GATE=PASS'
Write-Output 'REAL_BINARY_GATE=PASS'
Write-Output 'GATE_P9=PASS'
