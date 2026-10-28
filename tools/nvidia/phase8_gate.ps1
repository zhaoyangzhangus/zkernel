[CmdletBinding()]
param(
    [string]$Build = 'build/nvidia/phase8',
    [string]$RealTestLog = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'app-layout.ps1')
. (Join-Path $PSScriptRoot 'log-records.ps1')

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

function Resolve-RepoPath([string]$Path) {
    if ([IO.Path]::IsPathRooted($Path)) {
        return [IO.Path]::GetFullPath($Path)
    }
    return [IO.Path]::GetFullPath((Join-Path $repoRoot $Path))
}

function Fail-Gate([string]$Message) {
    throw ('P8 Gate failed: {0}' -f $Message)
}

function Require-File([string]$Path, [string]$Description) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        Fail-Gate ('missing {0}: {1}' -f $Description, $Path)
    }
}

function Require-Text([string]$Text, [string]$Pattern,
                      [string]$Description) {
    if ($Text -notmatch [regex]::Escape($Pattern)) {
        Fail-Gate ('required evidence is absent: {0}' -f $Description)
    }
}

$buildRoot = Resolve-RepoPath $Build
Assert-AppGuestLayout (Join-Path $buildRoot 'esp')
$kernel = Join-Path $buildRoot 'esp\EFI\OS\kernel.elf'
$testBinary = Join-Path $buildRoot 'esp\tests\p8-frontend'
$loader = Join-Path $buildRoot 'esp\usr\apps\p8-frontend\lib\ld-linux-x86-64.so.2'
$libc = Join-Path $buildRoot 'esp\usr\apps\p8-frontend\lib\libc.so.6'
$manifest = Join-Path $repoRoot 'build\nvidia\phase5\firmware.manifest'
$uapiSource = Join-Path $repoRoot 'include\uapi\nvidia_frontend.h'
$linuxSource = Join-Path $repoRoot 'kernel\syscall\linux.c'
$frontendSource = Join-Path $repoRoot 'kernel\drivers\gpu\nvidia\rm\nv_rm_frontend.c'
$bridgeSource = Join-Path $repoRoot 'kernel\drivers\gpu\nvidia\rm\nv_rm_vendor_bridge.c'
$driverSource = Join-Path $repoRoot 'kernel\drivers\gpu\nvidia\rm\nv_rm_driver.c'
$runnerSource = Join-Path $repoRoot 'kernel\process\linux_phase8_test.c'
$runtimeSource = Join-Path $repoRoot 'kernel\init\runtime.c'
$testSource = Join-Path $repoRoot 'tools\nvidia\phase8_frontend_test.c'
$buildSource = Join-Path $repoRoot 'tools\nvidia\phase8_build.ps1'
$stageSource = Join-Path $repoRoot 'tools\nvidia\phase8_stage.ps1'
$p7GateSource = Join-Path $repoRoot 'tools\nvidia\phase7_gate.ps1'
$logPath = if ([string]::IsNullOrWhiteSpace($RealTestLog)) {
    Join-Path $buildRoot 'realtest-success.log'
} else {
    Resolve-RepoPath $RealTestLog
}

foreach ($required in @(
        @($kernel, 'Phase 8 kernel image'),
        @($testBinary, 'staged NVIDIA frontend probe'),
        @($loader, 'staged Linux dynamic loader'),
        @($libc, 'staged glibc library'),
        @($manifest, '595.84 firmware manifest'),
        @($uapiSource, '595.84 frontend UAPI mirror'),
        @($linuxSource, 'Linux frontend syscall dispatcher'),
        @($frontendSource, 'NVIDIA frontend lifetime adapter'),
        @($bridgeSource, 'vendor RM frontend bridge'),
        @($driverSource, 'RM driver lifecycle source'),
        @($runnerSource, 'Phase 8 process acceptance runner'),
        @($runtimeSource, 'Phase 8 runtime integration'),
        @($testSource, 'real Linux frontend probe source'),
        @($buildSource, 'incremental Phase 8 builder'),
        @($stageSource, 'Phase 8 staging script'),
        @($p7GateSource, 'Phase 7 Gate script'),
        @($logPath, 'real Phase 8 test log'))) {
    Require-File $required[0] $required[1]
}

$p7Output = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $p7GateSource -Build build-phase7 -RealTestLog build-phase7/realtest-success.log 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0 -or
    @($p7Output | Where-Object { $_ -match 'GATE_P7=PASS' }).Count -ne 1) {
    Fail-Gate 'the previous Phase 7 Gate is not passing'
}

$vendorRelative = 'third_party/nvidia/595.84/open-gpu-kernel-modules'
$vendorDiff = @(& git -C $repoRoot diff --name-only -- $vendorRelative 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0) { Fail-Gate 'git vendor diff check failed' }
if (@($vendorDiff | Where-Object { $_.Trim() }).Count -ne 0) {
    Fail-Gate ('vendor source tree is modified: {0}' -f $vendorDiff[0])
}

$uapiText = Get-Content -LiteralPath $uapiSource -Raw
$linuxText = Get-Content -LiteralPath $linuxSource -Raw
$frontendText = Get-Content -LiteralPath $frontendSource -Raw
$bridgeText = Get-Content -LiteralPath $bridgeSource -Raw
$driverText = Get-Content -LiteralPath $driverSource -Raw
$runnerText = Get-Content -LiteralPath $runnerSource -Raw
$runtimeText = Get-Content -LiteralPath $runtimeSource -Raw
$testText = Get-Content -LiteralPath $testSource -Raw

foreach ($requiredText in @(
        'NV_FRONTEND_IOCTL_CARD_INFO',
        'NV_FRONTEND_IOCTL_CHECK_VERSION',
        'NV_FRONTEND_IOCTL_QUERY_DEVICE_INTR',
        'NV_FRONTEND_IOC_SIZEBITS',
        'NV_FRONTEND_IOCTL_NR',
        'sizeof(nv_frontend_card_info_t) == 72U',
        'sizeof(nv_frontend_rm_api_version_t) == 72U',
        'sizeof(nv_frontend_query_device_intr_t) == 8U')) {
    Require-Text $uapiText $requiredText 'fixed 595.84 frontend UAPI layout'
}

foreach ($requiredText in @(
        'LINUX_FD_NVIDIA',
        'linux_openat',
        'linux_ioctl',
        'NV_FRONTEND_IOCTL_SIZE',
        'vm_object_create_device',
        'nv_frontend_mapping_release',
        'linux_fd_table_destroy',
        'LINUX_SYS_IOCTL',
        'MAP_SHARED')) {
    Require-Text $linuxText $requiredText 'real frontend open/ioctl/mmap/cleanup path'
}

foreach ($requiredText in @(
        'nv_frontend_open',
        'nv_frontend_file_put',
        'nv_frontend_live_count',
        'nv_frontend_mapping_release')) {
    Require-Text $frontendText $requiredText 'frontend object lifetime and fail-closed dispatch'
}

foreach ($requiredText in @(
        'rm_perform_version_check',
        'NV_ESC_CARD_INFO',
        'NV_ESC_CHECK_VERSION_STR',
        'NV_ESC_QUERY_DEVICE_INTR',
        'NV_GPU_GSP_READY',
        'VM_PROT_EXEC',
        'X86_CACHE_UC',
        'nv_rm_vendor_frontend_range_contains',
        'K_ENOSYS')) {
    Require-Text $bridgeText $requiredText 'real RM-backed query and restricted BAR mapping'
}

foreach ($requiredText in @(
        'nv_rm_vendor_frontend_open',
        'nv_rm_vendor_frontend_ioctl',
        'nv_rm_vendor_frontend_mmap',
        'nv_rm_vendor_frontend_close')) {
    Require-Text $driverText $requiredText 'frontend hook declarations and fail-closed defaults'
}

foreach ($requiredText in @(
        'process_load_elf_image',
        'thread_create_user_suspended',
        'thread_start',
        'nv_frontend_live_count',
        'OS_P8_PROCESS_CLEANUP_PASS')) {
    Require-Text $runnerText $requiredText 'real process-exit cleanup acceptance'
}

foreach ($requiredText in @(
        'OS_P8_NVIDIA_GSP_READY',
        'OS_P8_FRONTEND_BEGIN',
        'OS_P8_FRONTEND_PROCESS_PASS',
        'linux_phase8_realtest')) {
    Require-Text $runtimeText $requiredText 'Phase 8 runtime Gate integration'
}

foreach ($requiredText in @(
        '/dev/nvidiactl',
        '/dev/nvidia0',
        'NV_FRONTEND_IOCTL_CARD_INFO',
        'NV_FRONTEND_IOCTL_CHECK_VERSION',
        'NV_FRONTEND_IOCTL_QUERY_DEVICE_INTR',
        'MAP_SHARED',
        'OS_P8_FRONTEND_MMAP_PASS',
        'OS_P8_CLOSE_PASS')) {
    Require-Text $testText $requiredText 'dedicated Linux personality frontend probe'
}
if ($testText -match '(?i)\bEGL\b') {
    Fail-Gate 'P8 probe enters EGL before the P9 phase'
}
$readelf = Get-Command x86_64-elf-readelf -ErrorAction SilentlyContinue
if ($null -eq $readelf) { $readelf = Get-Command readelf -ErrorAction Stop }
$nm = Get-Command x86_64-elf-nm -ErrorAction SilentlyContinue
if ($null -eq $nm) { $nm = Get-Command nm -ErrorAction Stop }

$kernelHeader = @(& $readelf.Source '-h' '--' $kernel 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0 -or
    @($kernelHeader | Where-Object { $_ -match 'Class:\s+ELF64' }).Count -ne 1 -or
    @($kernelHeader | Where-Object { $_ -match 'Type:\s+EXEC' }).Count -ne 1 -or
    @($kernelHeader | Where-Object { $_ -match 'Machine:\s+.*X86-64' }).Count -ne 1) {
    Fail-Gate 'Phase 8 kernel image is not an x86-64 ELF executable'
}

$undefined = @(& $nm.Source '-u' '-P' '--' $kernel 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0) { Fail-Gate 'Phase 8 undefined-symbol scan failed' }
$allowedLinkerSymbols = @(
    '___tls_start__', '___tls_end__', '__end__',
    'nv_rm_vendor_start', 'nv_rm_vendor_stop')
$unexpectedUndefined = @(
    foreach ($row in $undefined) {
        $trimmed = $row.Trim()
        if ([string]::IsNullOrWhiteSpace($trimmed)) { continue }
        $symbol = ($trimmed -split '\s+')[0]
        if ($allowedLinkerSymbols -notcontains $symbol) { $row }
    })
if ($unexpectedUndefined.Count -ne 0) {
    Fail-Gate ('Phase 8 kernel image has unresolved symbols: {0}' -f $unexpectedUndefined[0])
}

$testHeader = @(& $readelf.Source '-h' '--' $testBinary 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0 -or
    @($testHeader | Where-Object { $_ -match 'Class:\s+ELF64' }).Count -ne 1 -or
    @($testHeader | Where-Object { $_ -match 'Type:\s+EXEC' }).Count -ne 1 -or
    @($testHeader | Where-Object { $_ -match 'Machine:\s+.*X86-64' }).Count -ne 1) {
    Fail-Gate 'staged frontend probe is not an x86-64 ELF executable'
}
$testProgram = @(& $readelf.Source '-lW' '--' $testBinary 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0 -or
    @($testProgram | Where-Object {
        $_ -match 'Requesting program interpreter:\s+/usr/apps/p8-frontend/lib/ld-linux-x86-64\.so\.2'
    }).Count -ne 1) {
    Fail-Gate 'staged frontend probe does not carry the required PT_INTERP'
}
$testDynamic = @(& $readelf.Source '-d' '--' $testBinary 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0 -or
    @($testDynamic | Where-Object {
        $_ -match 'Shared library:\s+\[libc\.so\.6\]'
    }).Count -ne 1 -or
    @($testDynamic | Where-Object {
        $_ -match 'Shared library:\s+\[ld-linux-x86-64\.so\.2\]'
    }).Count -ne 0) {
    Fail-Gate 'staged frontend probe has an invalid dynamic dependency set'
}

$manifestText = Get-Content -LiteralPath $manifest -Raw
foreach ($firmwareName in @('gsp_ga10x.bin', 'gsp_tu10x.bin')) {
    $firmware = Join-Path $buildRoot ('esp\EFI\OS\firmware\{0}' -f $firmwareName)
    Require-File $firmware ('staged 595.84 firmware ' + $firmwareName)
    $hash = (Get-FileHash -LiteralPath $firmware -Algorithm SHA256).Hash
    $manifestPattern = 'file={0} .*sha256={1} baseline=595\.84' -f
        [regex]::Escape($firmwareName), $hash
    if ($manifestText -notmatch $manifestPattern) {
        Fail-Gate ('staged firmware is not the recorded 595.84 artifact: {0}' -f
                   $firmwareName)
    }
}

$realText = Get-Content -LiteralPath $logPath -Raw
foreach ($requiredMarker in @(
        'OS_P8_NVIDIA_GSP_READY',
        'OS_P8_FRONTEND_BEGIN',
        'OS_P8_FRONTEND_PROBE_BEGIN',
        'OS_P8_OPEN_NVIDIACTL_PASS',
        'OS_P8_OPEN_NVIDIA0_PASS',
        'OS_P8_DEVICE_QUERY_PASS',
        'OS_P8_RM_VERSION_QUERY_PASS',
        'OS_P8_DEVICE_INTR_QUERY_PASS',
        'OS_P8_FRONTEND_MMAP_PASS',
        'OS_P8_CLOSE_PASS',
        'OS_P8_PASS',
        'OS_P8_PROCESS_CLEANUP_PASS',
        'OS_P8_FRONTEND_PROCESS_PASS',
        'OS_REALTEST_PASS')) {
    if (-not (Test-LogRecord $realText $requiredMarker)) {
        Fail-Gate "required evidence is absent: $requiredMarker"
    }
}
if ($realText -match 'OS_P8_[A-Z0-9_]*FAIL' -or
    $realText -match 'OS_REALTEST_FAIL') {
    Fail-Gate 'real NVIDIA frontend log contains a failure marker'
}

Write-Output 'P8_STATIC=PASS'
Write-Output 'REAL_BINARY_GATE=PASS'
Write-Output 'GATE_P8=PASS'
