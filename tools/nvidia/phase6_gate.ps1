[CmdletBinding()]
param(
    [string[]]$HardwareLog = @()
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# powershell.exe -File 可能会将字符串数组作为逗号分隔的字符串传入。
if ($HardwareLog.Count -eq 1 -and $HardwareLog[0].Contains(',')) {
    $HardwareLog = @($HardwareLog[0] -split ',')
}

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$vendorRelative = 'third_party/nvidia/595.84/open-gpu-kernel-modules'
$phase5Root = Join-Path $repoRoot 'build\nvidia\phase5'
$phase6Root = Join-Path $repoRoot 'build\nvidia\phase6'
$phase5Kernel = Join-Path $phase5Root 'kernel.elf'
$phase6Kernel = Join-Path $phase6Root 'kernel.elf'
$phase6LinkList = Join-Path $phase6Root 'phase6_link_objects.list'
$phase6Source = Join-Path $repoRoot 'kernel\drivers\gpu\nvidia\rm\nv_rm_phase6.c'
$phase6Build = Join-Path $repoRoot 'tools\nvidia\phase6_build.ps1'

function Fail-Gate([string]$Message) {
    throw "P6 Gate failed: $Message"
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

Require-File $phase5Kernel 'Phase 5 prerequisite kernel image'
Require-File $phase6Kernel 'Phase 6 kernel image'
Require-File $phase6LinkList 'Phase 6 link list'
Require-File $phase6Source 'Phase 6 RM/CE source'
Require-File $phase6Build 'Phase 6 build script'

$vendorDiff = @(& git -C $repoRoot diff --name-only -- $vendorRelative 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0) { Fail-Gate 'git vendor diff check failed' }
if (@($vendorDiff | Where-Object { $_.Trim() }).Count -ne 0) {
    Fail-Gate "vendor source tree is modified: $($vendorDiff[0])"
}

$sourceText = Get-Content -LiteralPath $phase6Source -Raw
foreach ($requiredText in @(
        'NVOS32_TYPE_DMA', 'NV01_MEMORY_SYSTEM',
        'NV01_MEMORY_LOCAL_USER', 'NVOS32_ALLOC_FLAGS_PERSISTENT_VIDMEM',
        'rmapiutilAllocClientAndDeviceHandles', 'rmapiutilFreeClientAndDeviceHandles',
        'rm_api->MapToCpu', 'rm_api->UnmapFromCpu', 'ceutilsMemcopy(',
        'memdescFlushCpuCaches',
        'OS_NVIDIA_PHASE6_FULL_GATE',
        'NV_RM_PHASE6_CYCLE_COUNT', '20U',
        'OS_NVIDIA_PHASE6_SIZE_SWEEP_READY',
        'OS_NVIDIA_PHASE6_ALIGNMENT_SWEEP_READY')) {
    Require-Text $sourceText $requiredText 'RM-owned Phase 6 memory/CE path'
}
$forbiddenSourcePattern =
    '(?im)\b(?:os|system|liteos)_(?:vram_alloc_physical|gmmu_map|' +
    'gsp_rpc_alloc|gb205_channel_program)\s*\('
if ($sourceText -match $forbiddenSourcePattern) {
    Fail-Gate 'Phase 6 source contains a forbidden private GPU owner'
}

$buildText = Get-Content -LiteralPath $phase6Build -Raw
Require-Text $buildText '-DOS_NVIDIA_PHASE6=1' 'Phase 6 bridge build define'
Require-Text $buildText '-DOS_NVIDIA_PHASE6_FULL_GATE=1' 'explicit full Gate build switch'
$vendorWritePattern =
    '(?im)third_party[\\/]nvidia[\\/]595\.84.*(?:Set-Content|Out-File|Copy-Item)'
if ($buildText -match $vendorWritePattern) {
    Fail-Gate 'Phase 6 build script writes into the vendor tree'
}

$readelf = Get-Command x86_64-elf-readelf -ErrorAction SilentlyContinue
if ($null -eq $readelf) { $readelf = Get-Command readelf -ErrorAction Stop }
$nm = Get-Command x86_64-elf-nm -ErrorAction SilentlyContinue
if ($null -eq $nm) { $nm = Get-Command nm -ErrorAction Stop }

$header = @(& $readelf.Source '-h' '--' $phase6Kernel 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0 -or
    @($header | Where-Object { $_ -match 'Class:\s+ELF64' }).Count -ne 1 -or
    @($header | Where-Object { $_ -match 'Type:\s+EXEC' }).Count -ne 1 -or
    @($header | Where-Object { $_ -match 'Machine:\s+.*X86-64' }).Count -ne 1) {
    Fail-Gate 'Phase 6 image is not an x86-64 ELF executable'
}

$undefined = @(& $nm.Source '-u' '-P' '--' $phase6Kernel 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0) { Fail-Gate 'Phase 6 undefined-symbol scan failed' }
if (@($undefined | Where-Object { $_.Trim() }).Count -ne 0) {
    Fail-Gate "Phase 6 image has unresolved symbols: $($undefined[0])"
}

$linkRows = @(Get-Content -LiteralPath $phase6LinkList |
    Where-Object { $_ -and $_[0] -ne '#' })
if (@($linkRows | Where-Object { $_ -like '*phase5/objects/nv_rm_vendor_bridge.o' }).Count -ne 0 -or
    @($linkRows | Where-Object { $_ -like '*phase6/objects/nv_rm_vendor_bridge.o' }).Count -ne 1 -or
    @($linkRows | Where-Object { $_ -like '*phase6/objects/nv_rm_phase6.o' }).Count -ne 1) {
    Fail-Gate 'Phase 6 link list does not contain the Phase 6 bridge/probe closure'
}

Write-Output 'P6_STATIC=PASS'

$usableLogs = @()
foreach ($path in $HardwareLog) {
    if ([string]::IsNullOrWhiteSpace($path)) { continue }
    $fullPath = [IO.Path]::GetFullPath($path)
    Require-File $fullPath 'Phase 6 hardware serial log'
    $usableLogs += $fullPath
}
$usableLogs = @($usableLogs | Sort-Object -Unique)
if ($usableLogs.Count -eq 0) {
    Write-Output 'HARDWARE_GATE=UNAVAILABLE logs=0/1'
    Write-Output 'GATE_P6=UNAVAILABLE'
    exit 2
}

$seenRuns = New-Object 'System.Collections.Generic.HashSet[string]'
foreach ($path in $usableLogs) {
    $text = Get-Content -LiteralPath $path -Raw
    if ($text -match '(?i)qemu|std\s+vga|bochs|virtual\s+machine') {
        Fail-Gate "QEMU or virtual-machine log cannot satisfy the real-hardware Gate: $path"
    }
    foreach ($requiredHardwareText in @(
            'OS_NVIDIA_PHASE5_GATE_READY',
            'OS_NVIDIA_PHASE6_FULL_GATE_BEGIN',
            'OS_NVIDIA_PHASE6_SIZE_SWEEP_READY',
            'OS_NVIDIA_PHASE6_ALIGNMENT_SWEEP_READY',
            'OS_NVIDIA_PHASE6_CYCLE_PROGRESS completed=20',
            'OS_NVIDIA_PHASE6_FULL_GATE_READY',
            'OS_NVIDIA_PHASE6_GATE_READY',
            'OS_REALTEST_PASS')) {
        if ($text -notmatch [regex]::Escape($requiredHardwareText)) {
            Fail-Gate "hardware log evidence is absent: $requiredHardwareText ($path)"
        }
    }
    if ($text -notmatch '(?m)^OS_NVIDIA_PHASE6_CYCLE_PROGRESS completed=20\r?$') {
        Fail-Gate "hardware log does not contain the exact 20-cycle completion marker: $path"
    }
    if ($text -match '(?i)fake|override|hard.?code|not_linked|probe_fail|' +
            'GSP_BOOT_FAIL|GPU_FAULT|GSP_TIMEOUT|LEAK') {
        Fail-Gate "hardware log contains a fake/override/failure marker: $path"
    }

    $resultPath = Join-Path (Split-Path -Parent $path) 'realtest.result'
    Require-File $resultPath 'real hardware result metadata'
    $resultText = Get-Content -LiteralPath $resultPath -Raw
    $runMatch = [regex]::Match($text, '(?m)^run_id=([^\r\n]+)')
    if (-not $runMatch.Success) {
        $runMatch = [regex]::Match($resultText, '(?m)^run_id=([^\r\n]+)')
    }
    $statusMatch = [regex]::Match($resultText, '(?m)^status=([^\r\n]+)')
    if (-not $statusMatch.Success -or $statusMatch.Groups[1].Value.Trim() -ne 'pass') {
        Fail-Gate "real hardware result is not pass: $resultPath"
    }
    if (-not $runMatch.Success -or
        -not $seenRuns.Add($runMatch.Groups[1].Value.Trim())) {
        Fail-Gate "hardware log does not identify a unique full Phase 6 run: $path"
    }
}

Write-Output ('HARDWARE_GATE=PASS full_gate_runs={0}' -f $usableLogs.Count)
Write-Output 'GATE_P6=PASS'
