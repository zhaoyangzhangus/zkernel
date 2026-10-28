[CmdletBinding()]
param(
    [string]$Build = 'build-phase7',
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
    throw "P7 Gate failed: $Message"
}

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
$testBinary = Join-Path $buildRoot 'esp\tests\p7-glibc'
$loader = Join-Path $buildRoot 'esp\usr\apps\p7-glibc\lib\ld-linux-x86-64.so.2'
$libc = Join-Path $buildRoot 'esp\usr\apps\p7-glibc\lib\libc.so.6'
$dataFile = Join-Path $buildRoot 'esp\usr\apps\p7-glibc\p7-data.txt'
$linuxSource = Join-Path $repoRoot 'kernel\syscall\linux.c'
$execSource = Join-Path $repoRoot 'kernel\process\exec.c'
$processSource = Join-Path $repoRoot 'kernel\process\process.c'
$runnerSource = Join-Path $repoRoot 'kernel\process\linux_phase7_test.c'
$runtimeSource = Join-Path $repoRoot 'kernel\init\runtime.c'
$testSource = Join-Path $repoRoot 'tools\nvidia\phase7_glibc_test.c'
$stageScript = Join-Path $repoRoot 'tools\nvidia\phase7_stage.ps1'
$logPath = if ([string]::IsNullOrWhiteSpace($RealTestLog)) {
    Join-Path $buildRoot 'realtest-success.log'
} else {
    Resolve-RepoPath $RealTestLog
}

foreach ($required in @(
        @($kernel, 'Phase 7 kernel image'),
        @($testBinary, 'staged Linux/glibc test binary'),
        @($loader, 'staged Linux dynamic loader'),
        @($libc, 'staged glibc library'),
        @($dataFile, 'staged file-I/O test data'),
        @($linuxSource, 'Linux ABI dispatcher'),
        @($execSource, 'PT_INTERP/auxv loader integration'),
        @($processSource, 'Linux process personality state'),
        @($runnerSource, 'Phase 7 kernel acceptance runner'),
        @($runtimeSource, 'Phase 7 runtime startup integration'),
        @($testSource, 'real glibc acceptance binary source'),
        @($stageScript, 'Phase 7 staging script'),
        @($logPath, 'real Linux/glibc test log'))) {
    Require-File $required[0] $required[1]
}

$vendorRelative = 'third_party/nvidia/595.84/open-gpu-kernel-modules'
$vendorDiff = @(& git -C $repoRoot diff --name-only -- $vendorRelative 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0) { Fail-Gate 'git vendor diff check failed' }
if (@($vendorDiff | Where-Object { $_.Trim() }).Count -ne 0) {
    Fail-Gate "vendor source tree is modified: $($vendorDiff[0])"
}

$linuxText = Get-Content -LiteralPath $linuxSource -Raw
$execText = Get-Content -LiteralPath $execSource -Raw
$runnerText = Get-Content -LiteralPath $runnerSource -Raw
$runtimeText = Get-Content -LiteralPath $runtimeSource -Raw
foreach ($requiredText in @(
        'linux_syscall_dispatch', 'linux_fd_table', 'LINUX_SYS_MMAP',
        'LINUX_SYS_CLONE', 'LINUX_SYS_FUTEX', 'LINUX_SYS_ARCH_PRCTL',
        'LINUX_SYS_STATX', 'LINUX_AT_EMPTY_PATH', 'LINUX_SYS_PREAD64',
        'frame->r10', 'return K_ENOSYS')) {
    Require-Text $linuxText $requiredText 'Linux ABI syscall/FD/VM implementation'
}
foreach ($requiredText in @(
        'PT_INTERP', 'linux_elf_interpreter_path', 'LINUX_AUX_PUSH',
        'linux_abi', 'linux_fd_table_close_on_exec')) {
    Require-Text $execText $requiredText 'Linux PT_INTERP and auxv integration'
}
foreach ($requiredText in @(
        'process_load_elf_image', 'linux_phase7_realtest',
        'thread_create_user_suspended', 'thread_start')) {
    Require-Text $runnerText $requiredText 'real binary process acceptance runner'
}
Require-Text $runtimeText 'OS_P7_GLIBC_PROCESS_PASS' 'Phase 7 runtime Gate completion'
if ($linuxText -match 'OS_P7_') {
    Fail-Gate 'temporary Phase 7 syscall diagnostics remain in the adapter'
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
    Fail-Gate 'Phase 7 kernel image is not an x86-64 ELF executable'
}

$undefined = @(& $nm.Source '-u' '-P' '--' $kernel 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0) { Fail-Gate 'Phase 7 undefined-symbol scan failed' }
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
    Fail-Gate "Phase 7 kernel image has unresolved symbols: $($unexpectedUndefined[0])"
}

$testHeader = @(& $readelf.Source '-h' '--' $testBinary 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0 -or
    @($testHeader | Where-Object { $_ -match 'Class:\s+ELF64' }).Count -ne 1 -or
    @($testHeader | Where-Object { $_ -match 'Type:\s+EXEC' }).Count -ne 1 -or
    @($testHeader | Where-Object { $_ -match 'Machine:\s+.*X86-64' }).Count -ne 1) {
    Fail-Gate 'staged test is not an x86-64 ELF executable'
}
$testProgram = @(& $readelf.Source '-lW' '--' $testBinary 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0 -or
    @($testProgram | Where-Object {
        $_ -match 'Requesting program interpreter:\s+/usr/apps/p7-glibc/lib/ld-linux-x86-64\.so\.2'
    }).Count -ne 1) {
    Fail-Gate 'staged test does not carry the required PT_INTERP'
}
$testDynamic = @(& $readelf.Source '-d' '--' $testBinary 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0 -or
    @($testDynamic | Where-Object { $_ -match 'Shared library:\s+\[libc\.so\.6\]' }).Count -ne 1) {
    Fail-Gate 'staged test is not dynamically linked against libc.so.6'
}

$realText = Get-Content -LiteralPath $logPath -Raw
foreach ($requiredMarker in @(
        'OS_P7_GLIBC_HELLO',
        'OS_P7_MALLOC_FREE_PASS',
        'OS_P7_FILE_IO_PASS',
        'OS_P7_PTHREAD_CREATE_JOIN_PASS',
        'OS_P7_TLS_PASS',
        'OS_P7_THREAD_10000_PASS',
        'OS_P7_PASS',
        'OS_P7_GLIBC_PROCESS_PASS',
        'OS_REALTEST_PASS')) {
    if (-not (Test-LogRecord $realText $requiredMarker)) {
        Fail-Gate "required evidence is absent: $requiredMarker"
    }
}
if ($realText -match 'OS_P7_(?:GLIBC|MALLOC_FREE|FILE_IO|PTHREAD|TLS|THREAD_10000)_FAIL' -or
    $realText -match 'OS_REALTEST_FAIL') {
    Fail-Gate 'real Linux/glibc test log contains a failure marker'
}

Write-Output 'P7_STATIC=PASS'
Write-Output 'REAL_BINARY_GATE=PASS'
Write-Output 'GATE_P7=PASS'
