[CmdletBinding()]
param(
    [string]$Build = 'build/nvidia/cube-demo',
    [string]$RealTestLog = ''
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

function Fail-Gate([string]$Message) { throw "Cube demo Gate failed: $Message" }

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
$espRoot = Join-Path $buildRoot 'esp'
Assert-AppGuestLayout $espRoot
$checker = Join-Path $PSScriptRoot 'check_userspace_59584.ps1'
& $checker -AppRoot (Join-Path $espRoot 'usr\apps\cube')
if (-not $?) { Fail-Gate 'cube private userspace check failed' }
$binary = Join-Path $espRoot 'tests\p12-window'
$source = Join-Path $repoRoot 'tools\nvidia\opengl_cube_demo.c'
$buildSource = Join-Path $repoRoot 'tools\nvidia\cube_demo_build.ps1'
$runSource = Join-Path $repoRoot 'tools\run-real-hardware-test.ps1'
$linuxSource = Join-Path $repoRoot 'kernel\syscall\linux.c'
$firmwareManifest = Join-Path $repoRoot 'build\nvidia\phase5\firmware.manifest'
$logPath = if ([string]::IsNullOrWhiteSpace($RealTestLog)) {
    $candidates = @(Get-ChildItem -LiteralPath (Join-Path $repoRoot 'build-realtest\real-hardware') `
        -Recurse -File -Filter 'realtest.log' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending)
    $selected = $null
    foreach ($candidate in $candidates) {
        $candidateText = Get-Content -LiteralPath $candidate.FullName -Raw
        $candidateResult = Join-Path (Split-Path -Parent $candidate.FullName) 'realtest.result'
        if ((Test-LogRecord $candidateText 'OS_CUBE_PASS') -and
            (Test-Path -LiteralPath $candidateResult -PathType Leaf) -and
            (Get-Content -LiteralPath $candidateResult -Raw) -match 'status=pass') {
            $selected = $candidate.FullName
            break
        }
    }
    $selected
} else { Resolve-RepoPath $RealTestLog }

foreach ($required in @(
        @($binary, 'cube demo ELF'),
        @($source, 'cube demo source'),
        @($buildSource, 'cube demo builder'),
        @($runSource, 'real hardware runner'),
        @($linuxSource, 'Linux window event adapter'),
        @($firmwareManifest, '595.84 firmware manifest'))) {
    Require-File $required[0] $required[1]
}
if ([string]::IsNullOrWhiteSpace($logPath)) {
    Fail-Gate 'no completed cube demo real-test log was found'
}
Require-File $logPath 'cube demo real-test log'

$vendorRelative = 'third_party/nvidia/595.84/open-gpu-kernel-modules'
$vendorDiff = @(& git -C $repoRoot diff --name-only -- $vendorRelative 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0) { Fail-Gate 'git vendor diff check failed' }
if (@($vendorDiff | Where-Object { $_.Trim() }).Count -ne 0) {
    Fail-Gate ('vendor source tree is modified: {0}' -f $vendorDiff[0])
}

$sourceText = Get-Content -LiteralPath $source -Raw
$buildText = Get-Content -LiteralPath $buildSource -Raw
$runText = Get-Content -LiteralPath $runSource -Raw
$linuxText = Get-Content -LiteralPath $linuxSource -Raw
foreach ($requiredText in @(
        'glDrawArrays', 'glReadPixels', 'interpolated_color',
        'CUBE_GL_DEPTH_BUFFER_BIT', 'CUBE_HBUFFER_ADDRESS',
        'CUBE_GL_BGRA', 'OS_CUBE_GPU_SHARED_BUFFER',
        'LINUX_SYS_WINDOW_CREATE', 'linux_window_create_t',
        'OS_CUBE_WINDOW_CREATED',
        'OS_INPUT_BUTTON_LEFT', 'OS_INPUT_REL_X', 'OS_INPUT_REL_Y',
        'OS_CUBE_MOUSE_DRAG_READY', 'OS_CUBE_DRAG_PASS',
        'OS_CUBE_FRAME_PASS', 'OS_CUBE_VISIBLE',
        'OS_CUBE_CLOSE_PASS', 'OS_CUBE_PASS')) {
    Require-Text $sourceText $requiredText 'cube demo implementation'
}
foreach ($requiredText in @(
        'phase12_build.ps1', 'CUBE_INCREMENTAL_SKIP',
        'CUBE_STAGE_SKIPPED', 'tests\p12-window')) {
    Require-Text $buildText $requiredText 'cube demo incremental builder'
}
foreach ($requiredText in @(
        'NvidiaCubeDemo', 'ReuseNvidiaCubeDemoBuild',
        'cube-demo\esp', 'gsp_ga10x.bin')) {
    Require-Text $runText $requiredText 'cube demo deployment path'
}
foreach ($requiredText in @(
        'LINUX_FD_WINDOW', 'LINUX_O_NONBLOCK',
        'window_server_event_read', 'os_window_event_t',
        'LINUX_SYS_WINDOW_CREATE', 'linux_window_create')) {
    Require-Text $linuxText $requiredText 'window event read adapter'
}

$readelf = Get-Command x86_64-elf-readelf -ErrorAction SilentlyContinue
if ($null -eq $readelf) { $readelf = Get-Command readelf -ErrorAction Stop }
$header = @(& $readelf.Source '-hW' '--' $binary 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0 -or
    @($header | Where-Object { $_ -match 'Class:\s+ELF64' }).Count -ne 1 -or
    @($header | Where-Object { $_ -match 'Type:\s+EXEC' }).Count -ne 1 -or
    @($header | Where-Object { $_ -match 'Machine:\s+.*X86-64' }).Count -ne 1) {
    Fail-Gate 'cube demo is not an x86-64 ELF executable'
}
$program = @(& $readelf.Source '-lW' '--' $binary 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0 -or @($program | Where-Object {
        $_ -match 'Requesting program interpreter:\s+/usr/apps/cube/lib/ld-linux-x86-64\.so\.2'
    }).Count -ne 1) {
    Fail-Gate 'cube demo has no pinned Linux interpreter'
}
$dynamic = @(& $readelf.Source '-dW' '--' $binary 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0 -or
    @($dynamic | Where-Object { $_ -match 'Shared library:\s+\[libc\.so\.6\]' }).Count -ne 1 -or
    @($dynamic | Where-Object { $_ -match '/usr/apps/cube/lib' }).Count -ne 1) {
    Fail-Gate 'cube demo has no pinned glibc dependency/RPATH'
}

foreach ($relative in @(
        'EFI\BOOT\BOOTX64.EFI',
        'EFI\OS\kernel.elf',
        'EFI\OS\firmware\gsp_ga10x.bin',
        'usr\apps\cube\lib\ld-linux-x86-64.so.2',
        'usr\apps\cube\lib\libc.so.6',
        'usr\apps\cube\lib\libEGL.so.1',
        'usr\apps\cube\lib\libEGL_nvidia.so.0')) {
    Require-File (Join-Path $espRoot $relative) "cube demo ESP asset $relative"
}
$firmware = Join-Path $espRoot 'EFI\OS\firmware\gsp_ga10x.bin'
$firmwareHash = (Get-FileHash -LiteralPath $firmware -Algorithm SHA256).Hash
if ((Get-Content -LiteralPath $firmwareManifest -Raw) -notmatch
    ('file=gsp_ga10x\.bin .*sha256={0} baseline=595\.84' -f $firmwareHash)) {
    Fail-Gate 'cube demo GSP firmware is not the recorded 595.84 artifact'
}

$realText = Get-Content -LiteralPath $logPath -Raw
foreach ($requiredMarker in @(
        'OS_P12_NVIDIA_GSP_READY',
        'OS_P12_WINDOW_BEGIN',
        'OS_CUBE_EGL_BEGIN',
        'OS_CUBE_WINDOW_CREATED',
        'OS_CUBE_NVIDIA_DEVICE_PASS',
        'OS_CUBE_EGL_INITIALIZED',
        'OS_CUBE_CONTEXT_CURRENT',
        'OS_WINDOW_PRESENT_GEOMETRY x=64 y=48 width=800 height=600',
        'OS_CUBE_WINDOW_POSITION x=64 y=48',
        'OS_CUBE_GL_VENDOR=NVIDIA',
        'OS_CUBE_FBO_COMPLETE',
        'OS_CUBE_SHADER_READY',
        'OS_CUBE_GPU_SHARED_BUFFER address=',
        'OS_CUBE_READBACK center=',
        'OS_CUBE_MOUSE_DRAG_READY',
        'OS_CUBE_VISIBLE',
        'OS_CUBE_FRAME_PASS frames=',
        'OS_CUBE_CLOSE_PASS',
        'OS_CUBE_PASS',
        'OS_REALTEST_PASS',
        'OS_REALTEST_REBOOTING')) {
    if (-not (Test-LogRecord $realText $requiredMarker)) {
        Fail-Gate "required cube record is absent: $requiredMarker"
    }
}
if ($realText -notmatch '(?m)(?:^|[ \t])OS_CUBE_FRAME_PASS frames=[1-9][0-9]*(?=[ \t\r\n]|$)') {
    Fail-Gate 'cube demo did not render a complete frame sequence'
}
if ($realText -notmatch 'OS_NVIDIA_PHASE4_HAL_READY BLACKWELL GB205') {
    Fail-Gate 'cube evidence is not tied to the GB205 kernel path'
}
if ($realText -match '(?m)(?:^|[ \t])OS_CUBE_[A-Z0-9_]*FAIL(?=[ \t\r\n]|$)' -or
    $realText -match 'OS_REALTEST_FAIL' -or
    $realText -match '(?i)(GPU_FAULT|GSP_TIMEOUT|OS_CUBE_.*LEAK|ENOSYS)') {
    Fail-Gate 'cube demo log contains a failure, fault, leak, or ENOSYS marker'
}
$resultPath = Join-Path (Split-Path -Parent $logPath) 'realtest.result'
Require-File $resultPath 'cube demo real-test result'
if ((Get-Content -LiteralPath $resultPath -Raw) -notmatch 'status=pass') {
    Fail-Gate 'cube demo real-test result is not status=pass'
}

Write-Output 'CUBE_STATIC=PASS'
Write-Output 'CUBE_BINARY=PASS'
if (Test-LogRecord $realText 'OS_CUBE_DRAG_PASS') {
    Write-Output 'CUBE_MOUSE_INPUT=PASS'
} else {
    Write-Output 'CUBE_MOUSE_INPUT=READY_NO_PHYSICAL_EVENT'
}
Write-Output 'CUBE_REALTEST=PASS'
Write-Output 'GATE_CUBE=PASS'
