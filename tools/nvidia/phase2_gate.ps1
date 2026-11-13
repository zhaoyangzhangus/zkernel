[CmdletBinding()]
param(
    [string]$Make = 'make'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$vendorSourceRoot = Join-Path $repoRoot 'third_party\nvidia\595.84\open-gpu-kernel-modules'
$portRoot = Join-Path $repoRoot 'kernel\drivers\gpu\nvidia\port'
$phase1Gate = Join-Path $PSScriptRoot 'phase1_gate.ps1'

function Fail-Gate([string]$Message) {
    throw "P2 Gate failed: $Message"
}

if (-not (Test-Path -LiteralPath $vendorSourceRoot -PathType Container)) {
    Fail-Gate 'immutable NVIDIA vendor source is missing'
}
if (-not (Test-Path -LiteralPath $portRoot -PathType Container)) {
    Fail-Gate 'NVIDIA Port source root is missing'
}

# P2 位于 P1 之后。重新执行 P1 校验，避免已更改的供应商代码树或
# 已失效的源码清单悄然成为后续阶段的输入。
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $phase1Gate
if ($LASTEXITCODE -ne 0) { Fail-Gate 'P1 Gate did not pass' }

$portNames = @(
    'memory', 'atomic', 'cache', 'sync', 'time', 'thread', 'worker', 'vm',
    'pci', 'mmio', 'vbios', 'dma', 'irq', 'firmware', 'registry'
)
foreach ($name in $portNames) {
    $sourcePath = Join-Path $portRoot ("nv_port_{0}.c" -f $name)
    if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) {
        Fail-Gate "missing Port source: $name"
    }
}

$kernelOutput = @(& $Make kernel 2>&1 | ForEach-Object { $_.ToString() })
$kernelExitCode = $LASTEXITCODE
foreach ($line in $kernelOutput) { Write-Output $line }
if ($kernelExitCode -ne 0) { Fail-Gate 'production kernel build failed' }

$testOutput = @(& $Make nvidia-port-test 2>&1 | ForEach-Object { $_.ToString() })
$testExitCode = $LASTEXITCODE
foreach ($line in $testOutput) { Write-Output $line }
if ($testExitCode -ne 0) { Fail-Gate 'Port test executable failed' }

$requiredMarkers = @(
    'PORT_TESTS=PASS',
    'SMP_STRESS=PASS',
    'VM_MAP_UNMAP_LEAK=PASS',
    'DMA_DESCRIPTOR=PASS',
    'IRQ_VECTOR_STRESS=PASS'
)
foreach ($marker in $requiredMarkers) {
    if (-not @($testOutput | Where-Object { $_ -eq $marker }).Count) {
        Fail-Gate "missing required test result: $marker"
    }
}
if (@($testOutput | Where-Object { $_ -match '=FAIL$' }).Count -ne 0) {
    Fail-Gate 'a required test reported FAIL'
}

$writableVendorFiles = @(
    Get-ChildItem -LiteralPath $vendorSourceRoot -Recurse -File |
        Where-Object { -not $_.IsReadOnly }
)
if ($writableVendorFiles.Count -ne 0) {
    Fail-Gate "vendor source contains writable files: $($writableVendorFiles[0].FullName)"
}

$gitVendorPath = 'third_party/nvidia/595.84'
$unstagedVendorChanges = @(& git -C $repoRoot diff --name-only -- $gitVendorPath 2>&1 |
    Where-Object { $_ -and $_.ToString().Trim() })
$stagedVendorChanges = @(& git -C $repoRoot diff --cached --name-only -- $gitVendorPath 2>&1 |
    Where-Object { $_ -and $_.ToString().Trim() })
if ($unstagedVendorChanges.Count -ne 0 -or $stagedVendorChanges.Count -ne 0) {
    Fail-Gate 'vendor source has Git modifications'
}

Write-Output 'GATE_P2=PASS'
