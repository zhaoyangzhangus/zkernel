[CmdletBinding()]
param(
    [string]$Make = 'make',
    [string]$HardwareLog = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$vendorRoot = Join-Path $repoRoot 'third_party\nvidia\595.84'
$vendorSourceRoot = Join-Path $vendorRoot 'open-gpu-kernel-modules'
$phase3Gate = Join-Path $PSScriptRoot 'phase3_gate.ps1'
$bridgeSource = Join-Path $repoRoot 'kernel\drivers\gpu\nvidia\rm\nv_rm_vendor_bridge.c'
$driverSource = Join-Path $repoRoot 'kernel\drivers\gpu\nvidia\rm\nv_rm_driver.c'
$workQueueSource = Join-Path $repoRoot 'kernel\drivers\gpu\nvidia\rm\nv_rm_workqueue.c'
$kernelElf = Join-Path $repoRoot 'build\nvidia\phase4\kernel.elf'
$linkList = Join-Path $repoRoot 'build\nvidia\phase4\phase4_link_objects.list'
$workQueueObject = Join-Path $repoRoot 'build\nvidia\phase4\objects\nv_rm_workqueue.o'
$abiWorkQueueObject = Join-Path $repoRoot 'build\nvidia\phase4\objects\vendor_abi\0010.o'

function Fail-Gate([string]$Message) {
    throw "P4 Gate failed: $Message"
}

function Require-File([string]$Path, [string]$Description) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        Fail-Gate "missing $Description`: $Path"
    }
}

Require-File $phase3Gate 'P3 Gate script'

# 阶段顺序既在此处强制执行，也在阶段 4 构建脚本中执行。过期的阶段 3 报告
# 或阶段 4 映像均不能通过此 Gate。
$p3Args = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $phase3Gate,
            '-Make', $Make)
$p3Output = @(& powershell.exe @p3Args 2>&1 |
    ForEach-Object { $_.ToString() })
$p3ExitCode = $LASTEXITCODE
foreach ($line in $p3Output) { Write-Output $line }
if ($p3ExitCode -ne 0 -or
    @($p3Output | Where-Object { $_ -eq 'GATE_P3=PASS' }).Count -ne 1) {
    Fail-Gate 'P3 Gate did not pass'
}

Require-File $kernelElf 'Phase 4 kernel image'
Require-File $linkList 'Phase 4 link list'
Require-File $bridgeSource 'vendor bridge source'
Require-File $driverSource 'System RM driver source'
Require-File $workQueueSource 'System RM workqueue adapter source'

$readelfCommand = Get-Command x86_64-elf-readelf -ErrorAction SilentlyContinue
if ($null -eq $readelfCommand) { $readelfCommand = Get-Command readelf -ErrorAction Stop }
$nmCommand = Get-Command x86_64-elf-nm -ErrorAction SilentlyContinue
if ($null -eq $nmCommand) { $nmCommand = Get-Command nm -ErrorAction Stop }

$header = @(& $readelfCommand.Source '-h' '--' $kernelElf 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0) { Fail-Gate 'readelf -h failed' }
if (@($header | Where-Object { $_ -match 'Class:\s+ELF64' }).Count -ne 1 -or
    @($header | Where-Object { $_ -match 'Type:\s+EXEC' }).Count -ne 1 -or
    @($header | Where-Object { $_ -match 'Machine:\s+.*X86-64' }).Count -ne 1) {
    Fail-Gate 'Phase 4 image is not an x86-64 ELF executable'
}

$programHeaders = @(& $readelfCommand.Source '-l' '--' $kernelElf 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0) { Fail-Gate 'readelf -l failed' }
if (@($programHeaders | Where-Object { $_ -match '^\s*LOAD\s+' }).Count -lt 2 -or
    @($programHeaders | Where-Object { $_ -match '\sR E\s+' }).Count -eq 0 -or
    @($programHeaders | Where-Object { $_ -match '\sRW\s+' }).Count -eq 0) {
    Fail-Gate 'Phase 4 image does not have executable and writable load segments'
}

$symbols = @(& $nmCommand.Source '-g' '--defined-only' '--' $kernelElf 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0) { Fail-Gate 'nm defined-symbol scan failed' }
foreach ($requiredSymbol in @(
        'kernel_entry', 'nv_rm_driver_probe', 'nv_rm_vendor_start',
        'nv_rm_vendor_stop', 'rm_init_rm', 'rm_init_private_state',
        'rm_shutdown_rm')) {
    if (@($symbols | Where-Object {
            $_ -match ('\s{0}\s*$' -f [regex]::Escape($requiredSymbol))
        }).Count -eq 0) {
        Fail-Gate "required symbol is absent: $requiredSymbol"
    }
}

$undefined = @(& $nmCommand.Source '-u' '-P' '--' $kernelElf 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0) { Fail-Gate 'nm undefined-symbol scan failed' }
if (@($undefined | Where-Object { $_.Trim() }).Count -ne 0) {
    Fail-Gate "final Phase 4 image has unresolved symbols: $($undefined[0])"
}

$linkRows = @(Get-Content -LiteralPath $linkList |
    Where-Object { $_ -and $_[0] -ne '#' })
if (@($linkRows | Where-Object { $_ -like '*phase3/vendor-rm.o' }).Count -ne 1 -or
    @($linkRows | Where-Object {
        $_ -like '*phase4/objects/nv_rm_vendor_bridge.o'
    }).Count -ne 1 -or
    @($linkRows | Where-Object {
        $_ -like '*phase4/objects/nv_rm_workqueue.o'
    }).Count -ne 1 -or
    @($linkRows | Where-Object {
        $_ -like '*phase4/objects/vendor_abi/*.o'
    }).Count -ne 11) {
    Fail-Gate 'Phase 4 link list does not contain the expected RM/ABI closure'
}
Require-File $workQueueObject 'compiled System RM workqueue adapter'
Require-File $abiWorkQueueObject 'compiled fail-closed OS ABI object'

$workQueueNmArgs = @('-g', '-S', '--defined-only', '--', $workQueueObject)
$workQueueSymbols = @(& $nmCommand.Source @workQueueNmArgs 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0) { Fail-Gate 'workqueue adapter symbol scan failed' }
$abiWorkQueueNmArgs = @('-g', '-S', '--defined-only', '--', $abiWorkQueueObject)
$abiWorkQueueSymbols = @(& $nmCommand.Source @abiWorkQueueNmArgs 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0) { Fail-Gate 'fail-closed OS ABI symbol scan failed' }
$finalWorkQueueNmArgs = @('-g', '-S', '--defined-only', '--', $kernelElf)
$finalWorkQueueSymbols = @(& $nmCommand.Source @finalWorkQueueNmArgs 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0) { Fail-Gate 'final workqueue symbol scan failed' }
foreach ($workQueueSymbol in @(
        'os_queue_work_item', 'os_flush_work_queue',
        'os_is_queue_flush_ongoing')) {
    $adapterRow = @($workQueueSymbols | Where-Object {
        $_ -match ('\s{0}\s*$' -f [regex]::Escape($workQueueSymbol))
    })
    $unsupportedRow = @($abiWorkQueueSymbols | Where-Object {
        $_ -match ('\s{0}\s*$' -f [regex]::Escape($workQueueSymbol))
    })
    $finalRow = @($finalWorkQueueSymbols | Where-Object {
        $_ -match ('\s{0}\s*$' -f [regex]::Escape($workQueueSymbol))
    })
    if ($adapterRow.Count -ne 1 -or $unsupportedRow.Count -ne 1 -or
        $finalRow.Count -ne 1) {
        Fail-Gate "workqueue symbol closure is incomplete: $workQueueSymbol"
    }

    $adapterMatch = [regex]::Match($adapterRow[0],
        '^\s*[0-9a-fA-F]+\s+([0-9a-fA-F]+)\s+[A-Za-z]\s+')
    $unsupportedMatch = [regex]::Match($unsupportedRow[0],
        '^\s*[0-9a-fA-F]+\s+([0-9a-fA-F]+)\s+[A-Za-z]\s+')
    $finalMatch = [regex]::Match($finalRow[0],
        '^\s*[0-9a-fA-F]+\s+([0-9a-fA-F]+)\s+[A-Za-z]\s+')
    if (-not $adapterMatch.Success -or -not $unsupportedMatch.Success -or
        -not $finalMatch.Success) {
        Fail-Gate "workqueue symbol sizes are unreadable: $workQueueSymbol"
    }

    $adapterSize = [Convert]::ToUInt64($adapterMatch.Groups[1].Value, 16)
    $unsupportedSize = [Convert]::ToUInt64($unsupportedMatch.Groups[1].Value, 16)
    $finalSize = [Convert]::ToUInt64($finalMatch.Groups[1].Value, 16)
    if ($finalSize -ne $adapterSize -or $adapterSize -le $unsupportedSize) {
        Fail-Gate "final image does not select the real workqueue adapter: $workQueueSymbol"
    }
}

$bridgeText = Get-Content -LiteralPath $bridgeSource -Raw
foreach ($requiredText in @(
        'rm_init_rm(', 'rm_is_supported_device(', 'rm_init_private_state(',
        'rm_free_private_state(', 'rm_shutdown_rm(', 'os_pci_init_handle(',
        'objCreate(&probe_gpu', 'gpuBindHalLegacy(',
        'gpu_arch->chipArch', 'gpu_arch->chipImpl',
        'NV_PMC_BOOT_42_ARCHITECTURE_GB200', 'GPU_IMPLEMENTATION_GB205',
        'HAL_IMPL_GB205', 'gpu->pHal')) {
    if ($bridgeText -notmatch [regex]::Escape($requiredText)) {
        Fail-Gate "vendor bridge evidence is absent: $requiredText"
    }
}
if ($bridgeText -match '(?im)(chipInfo\.(platformId|implementationId)|pGpuArch)\s*=') {
    Fail-Gate 'vendor bridge writes an architecture/chip override'
}
if ($bridgeText -match '(?im)\b(gsp|gmmu|pma|gpu\s*va)\b\s*[:=]') {
    Fail-Gate 'Phase 4 bridge contains a System-side GPU-owned implementation'
}
$disableIndex = $bridgeText.IndexOf(
    'rm_disable_adapter(context->stack, context->state);',
    [System.StringComparison]::Ordinal)
$irqStopIndex = $bridgeText.IndexOf(
    'nv_rm_vendor_stop_irq(context);',
    [System.StringComparison]::Ordinal)
$shutdownIndex = $bridgeText.IndexOf(
    'rm_shutdown_adapter(context->stack, context->state);',
    [System.StringComparison]::Ordinal)
if ($disableIndex -lt 0 -or $irqStopIndex -lt 0 -or $shutdownIndex -lt 0 -or
    $disableIndex -ge $irqStopIndex -or $irqStopIndex -ge $shutdownIndex) {
    Fail-Gate 'Phase 4 RM teardown is not disable -> IRQ stop -> shutdown'
}

$workQueueText = Get-Content -LiteralPath $workQueueSource -Raw
foreach ($requiredWorkQueueText in @(
        'os_queue_work_item(', 'os_flush_work_queue(',
        'os_is_queue_flush_ongoing(', 'rm_execute_work_item(',
        'nv_port_work_queue(', 'nv_port_work_drain(',
        'deferred_run(64U)', 'deferred worker')) {
    if ($workQueueText -notmatch [regex]::Escape($requiredWorkQueueText)) {
        Fail-Gate "workqueue adapter evidence is absent: $requiredWorkQueueText"
    }
}
if ($workQueueText -match '(?im)\b(scheduler|gsp|gmmu|pma|gpu\s*va)\b\s*[:=]') {
    Fail-Gate 'workqueue adapter contains a forbidden private subsystem'
}

$driverText = Get-Content -LiteralPath $driverSource -Raw
if ($driverText -notmatch 'OS_NVIDIA_RM_NOT_LINKED' -or
    $driverText -notmatch 'return\s+K_ENOSYS') {
    Fail-Gate 'normal-image vendor boundary is not fail-closed'
}

# P3 Gate 会检查供应商代码树。此处额外的差异检查会明确记录阶段 4 证据，
# 防止本地对供应商源码的修改被重新构建的对象掩盖。
$vendorRelative = 'third_party/nvidia/595.84/open-gpu-kernel-modules'
$vendorDiff = @(& git -C $repoRoot diff --name-only -- $vendorRelative 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0) { Fail-Gate 'git vendor diff check failed' }
if (@($vendorDiff | Where-Object { $_.Trim() }).Count -ne 0) {
    Fail-Gate "vendor source tree is modified: $($vendorDiff[0])"
}

Write-Output 'P4_STATIC=PASS'

if ([string]::IsNullOrWhiteSpace($HardwareLog)) {
    Write-Output 'HARDWARE_GATE=UNAVAILABLE'
    Write-Output 'GATE_P4=UNAVAILABLE'
    exit 2
}

$hardwareLogPath = [System.IO.Path]::GetFullPath($HardwareLog)
Require-File $hardwareLogPath 'System hardware serial log'
$hardwareText = Get-Content -LiteralPath $hardwareLogPath -Raw

if ($hardwareText -match '(?i)qemu|std\s+vga|bochs|virtual\s+machine') {
    Fail-Gate 'QEMU or virtual-machine log cannot satisfy the real-hardware Gate'
}
foreach ($requiredHardwareText in @(
        'OS_NVIDIA_PCI_FOUND\s+\S+\s+ID=10de:2f04',
        'OS_NVIDIA_BAR0_READY', 'OS_NVIDIA_BAR1_METADATA',
        'OS_NVIDIA_PHASE4_RM_READY',
        'OS_NVIDIA_PHASE4_GPU_OBJECT_READY',
        'OS_NVIDIA_ARCH=[0-9a-fA-F]+\s+IMPL=[0-9a-fA-F]+\s+HAL=[0-9a-fA-F]+',
        'OS_NVIDIA_PHASE4_HAL_READY\s+BLACKWELL\s+GB205',
        'OS_NVIDIA_PHASE4_GATE_READY')) {
    if ($hardwareText -notmatch $requiredHardwareText) {
        Fail-Gate "hardware log evidence is absent: $requiredHardwareText"
    }
}
if ($hardwareText -match '(?i)fake|override|hard.?code|not_linked|probe_fail') {
    Fail-Gate 'hardware log contains a fake/override/failure marker'
}

Write-Output 'HARDWARE_GATE=PASS'
Write-Output 'GATE_P4=PASS'
