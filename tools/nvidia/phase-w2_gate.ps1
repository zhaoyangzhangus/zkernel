[CmdletBinding()]
param(
    [string]$Phase12Build = 'build/nvidia/phase12',
    [string]$Phase12RealTestLog = 'build-realtest/real-hardware/20260909-121533/realtest.log'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

function Resolve-RepoPath([string]$Path) {
    if ([IO.Path]::IsPathRooted($Path)) { return [IO.Path]::GetFullPath($Path) }
    return [IO.Path]::GetFullPath((Join-Path $repoRoot $Path))
}

function Fail-Gate([string]$Message) { throw "W2 Gate failed: $Message" }

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

$phase12Gate = Join-Path $repoRoot 'tools\nvidia\phase12_gate.ps1'
$phase12Args = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
                $phase12Gate, '-Build', $Phase12Build,
                '-RealTestLog', $Phase12RealTestLog)
$phase12Output = @(& powershell.exe @phase12Args 2>&1 |
    ForEach-Object { $_.ToString() })
foreach ($line in $phase12Output) { Write-Output $line }
if ($LASTEXITCODE -ne 0 -or
    @($phase12Output | Where-Object { $_ -eq 'GATE_P12=PASS' }).Count -ne 1) {
    Fail-Gate 'previous Phase 12 Gate is not passing'
}

$gpuHeader = Join-Path $repoRoot 'include\kernel\gpu.h'
$gpuSource = Join-Path $repoRoot 'kernel\drivers\gpu\core\core.c'
$displaySource = Join-Path $repoRoot 'kernel\drivers\display\core.c'
$graphicsSyscall = Join-Path $repoRoot 'kernel\syscall\graphics.c'
$nvkmsBuild = Join-Path $repoRoot 'tools\nvidia\nvkms_build.ps1'
$gpuObject = Join-Path $repoRoot 'build\kernel\gpu-core.o'
$displayObject = Join-Path $repoRoot 'build\kernel\display-core.o'
$graphicsObject = Join-Path $repoRoot 'build\kernel\syscall-graphics.o'
foreach ($required in @(
        @($gpuHeader, 'generic allocation contract'),
        @($gpuSource, 'GPU core allocation implementation'),
        @($displaySource, 'display allocation consumer'),
        @($graphicsSyscall, 'GPU map syscall'),
        @($nvkmsBuild, 'official NVKMS target builder'),
        @($gpuObject, 'rebuilt GPU core object'),
        @($displayObject, 'rebuilt display core object'),
        @($graphicsObject, 'rebuilt graphics syscall object'))) {
    Require-File $required[0] $required[1]
}

$gpuText = Get-Content -LiteralPath $gpuHeader -Raw
$gpuSourceText = Get-Content -LiteralPath $gpuSource -Raw
$displayText = Get-Content -LiteralPath $displaySource -Raw
$graphicsText = Get-Content -LiteralPath $graphicsSyscall -Raw
foreach ($requiredText in @(
        'GPU_ALLOCATION_BACKEND_CPU_WB',
        'GPU_ALLOCATION_BACKEND_CPU_WC',
        'GPU_ALLOCATION_BACKEND_NVIDIA_GPU',
        'gpu_allocation_backend_ops_t',
        'gpu_allocation_create_with_backend',
        'gpu_allocation_pin',
        'gpu_allocation_cpu_map',
        'gpu_allocation_get_physical')) {
    Require-Text $gpuText $requiredText 'generic GPU allocation backend contract'
}
foreach ($requiredText in @(
        'backend_ops->release',
        'gpu_allocation_backend',
        'gpu_allocation_unpin',
        'gpu_allocation_cpu_unmap')) {
    Require-Text $gpuSourceText $requiredText 'GPU core backend lifecycle dispatch'
}
foreach ($requiredText in @(
        'gpu_allocation_cpu_map',
        'gpu_allocation_cpu_unmap',
        'gpu_allocation_pin',
        'gpu_allocation_unpin')) {
    Require-Text $displayText $requiredText 'display uses generic allocation operations'
}
Require-Text $graphicsText 'gpu_allocation_get_physical' 'user mapping uses generic physical accessor'

# 通用 Window/系统调用使用方不得检查后端私有指针或 RM/KMS 句柄。在当前 W2 约定中，
# 只有 GPU 核心拥有此字段。
$consumerPaths = @((Join-Path $repoRoot 'kernel\graphics'),
                   (Join-Path $repoRoot 'kernel\syscall'))
$rgArguments = @('-n', '--fixed-strings', '--', 'backend_private') + $consumerPaths
$forbiddenConsumerMatches = @(& rg.exe @rgArguments 2>$null |
    ForEach-Object { $_.ToString() })
if ($forbiddenConsumerMatches.Count -ne 0) {
    Fail-Gate 'backend_private is visible to generic Window or Ring3 syscall code'
}

# 保留供应商源文件不可修改的严格边界，同时允许目标构建器读取只读源码树。
$vendorRoot = Join-Path $repoRoot 'third_party\nvidia\595.84\open-gpu-kernel-modules'
$gitArguments = @('-C', $repoRoot, 'diff', '--name-only', '--',
                  'third_party/nvidia/595.84/open-gpu-kernel-modules')
$vendorDiff = @(& git @gitArguments 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0 -or $vendorDiff.Count -ne 0) {
    Fail-Gate '595.84 vendor source tree is modified'
}
$writableVendorFiles = @(Get-ChildItem -LiteralPath $vendorRoot -Recurse -File |
    Where-Object { -not $_.IsReadOnly })
if ($writableVendorFiles.Count -ne 0) {
    Fail-Gate "vendor source is writable: $($writableVendorFiles[0].FullName)"
}

Write-Output 'W2_PREVIOUS_GATE=PASS'
Write-Output 'W2_GENERIC_BUFFER_CONTRACT=PASS'
Write-Output 'W2_BACKEND_OPAQUE_BOUNDARY=PASS'
Write-Output 'W2_VENDOR_IMMUTABILITY=PASS'
Write-Output 'GATE_W2=PASS'
