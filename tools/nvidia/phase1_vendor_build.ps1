[CmdletBinding()]
param(
    [switch]$SkipCompile
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$vendorRoot = Join-Path $repoRoot 'third_party\nvidia\595.84'
$vendorSourceRoot = Join-Path $vendorRoot 'open-gpu-kernel-modules'
$outputRoot = Join-Path $repoRoot 'build\nvidia'
$objectRoot = Join-Path $outputRoot 'objects\host-unix-probe'
$generatedRoot = Join-Path $outputRoot 'generated'
$rmGeneratedRoot = Join-Path $generatedRoot 'rm'
$modesetGeneratedRoot = Join-Path $generatedRoot 'modeset'
$sourceListReport = Join-Path $outputRoot 'vendor_sources.txt'
$objectReport = Join-Path $outputRoot 'vendor_objects.txt'
$unresolvedReport = Join-Path $outputRoot 'unresolved_symbols.txt'
$compileLog = Join-Path $outputRoot 'compile.log'
$tab = [char]9
$categoryOrder = @('memory', 'DMA', 'PCI', 'MMIO', 'IRQ', 'sync', 'atomic', 'time', 'thread/worker', 'VM/user', 'cache', 'firmware', 'VBIOS', 'registry', 'misc')

function Write-AsciiLines([string]$Path, [System.Collections.IEnumerable]$Lines) {
    [System.IO.File]::WriteAllLines($Path, [string[]]$Lines, [System.Text.Encoding]::ASCII)
}

function Get-RelativeVendorPath([string]$FullPath) {
    $vendorRootFull = [System.IO.Path]::GetFullPath($vendorRoot)
    $fullPathNormalized = [System.IO.Path]::GetFullPath($FullPath)
    $prefix = $vendorRootFull + [System.IO.Path]::DirectorySeparatorChar
    if (-not $fullPathNormalized.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "path escaped vendor root: $FullPath"
    }
    return $fullPathNormalized.Substring($prefix.Length).Replace('\', '/')
}

function Get-SymbolCategory([string]$SymbolName) {
    # 仅按名称进行确定性分类。它根据符号名称描述缺失的边界，绝不提供实现。
    if ($SymbolName -match '(?i)(dma|scatter|iommu|sg_)') { return 'DMA' }
    if ($SymbolName -match '(?i)(irq|intr|interrupt|isr|msi)') { return 'IRQ' }
    if ($SymbolName -match '(?i)(atomic|cmpxchg|compare_exchange|cas)') { return 'atomic' }
    if ($SymbolName -match '(?i)(mutex|semaphore|sema|spinlock|rwlock|sync|event|condition|fence|completion|lock|wait|wake)') { return 'sync' }
    if ($SymbolName -match '(?i)(clock|timer|delay|sleep|jiff|tick|time)') { return 'time' }
    if ($SymbolName -match '(?i)(thread|kthread|worker|workqueue|task|sched|yield)') { return 'thread/worker' }
    if ($SymbolName -match '(?i)(vaspace|gpuva|gmmu|mmap|munmap|virtual|user|address_space|copy_to|copy_from)') { return 'VM/user' }
    if ($SymbolName -match '(?i)(cache|clflush|flush_cache|invalidate)') { return 'cache' }
    if ($SymbolName -match '(?i)(vbios|v-bios|video_bios)') { return 'VBIOS' }
    if ($SymbolName -match '(?i)(firmware|fw_|gsp|rpc|falcon|sec2|pmu|fsp|ucode|microcode|boot)') { return 'firmware' }
    if ($SymbolName -match '(?i)(pci|pcie|bar[0-9]?|msix)') { return 'PCI' }
    if ($SymbolName -match '(?i)(mmio|ioremap|io_space|ioport|register|read_reg|write_reg|rdcr|wrcr)') { return 'MMIO' }
    if ($SymbolName -match '(?i)(registry|regkey|reg_str)') { return 'registry' }
    if ($SymbolName -match '(?i)(mem|alloc|free|heap|page|phys|vidmem|sysmem|fbmem|buffer|buf|pool|memory)') { return 'memory' }
    return 'misc'
}

if (-not (Test-Path -LiteralPath $vendorSourceRoot -PathType Container)) {
    throw "vendor source root is missing: $vendorSourceRoot"
}
New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null
New-Item -ItemType Directory -Path $rmGeneratedRoot -Force | Out-Null
New-Item -ItemType Directory -Path $modesetGeneratedRoot -Force | Out-Null

# 在读取导入的基线以构建依赖图之前先进行校验。
# 这样也能尽早发现只读供应商代码树遭到意外修改。
$versionPath = Join-Path $vendorRoot 'VENDOR_VERSION'
$manifestPath = Join-Path $vendorRoot 'VENDOR_SHA256SUMS'
if (-not (Test-Path -LiteralPath $versionPath -PathType Leaf)) { throw "missing VENDOR_VERSION" }
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) { throw "missing VENDOR_SHA256SUMS" }
$versionText = Get-Content -LiteralPath $versionPath -Raw
foreach ($requiredVersionLine in @(
        'version=595.84',
        'tag=595.84',
        'tag_object=3f74968ea1486de6a21698eb22773fcd2f24d35c',
        'commit=722ae84526a09ed672fbe75448e2909834ba4cce',
        'semantic_vendor_patches=0')) {
    if ($versionText -notmatch [regex]::Escape($requiredVersionLine)) {
        throw "VENDOR_VERSION missing: $requiredVersionLine"
    }
}

$manifestEntries = New-Object 'System.Collections.Generic.Dictionary[string,string]' ([System.StringComparer]::OrdinalIgnoreCase)
foreach ($line in @(Get-Content -LiteralPath $manifestPath)) {
    if ([string]::IsNullOrWhiteSpace($line) -or $line.StartsWith('#')) { continue }
    $manifestMatch = [regex]::Match($line, '^([0-9a-fA-F]{64})  (.+)$')
    if (-not $manifestMatch.Success) { throw "invalid manifest line: $line" }
    $manifestEntries.Add($manifestMatch.Groups[2].Value, $manifestMatch.Groups[1].Value.ToLowerInvariant())
}
$vendorFiles = @(Get-ChildItem -LiteralPath $vendorSourceRoot -Recurse -File)
if ($vendorFiles.Count -ne $manifestEntries.Count) {
    throw "vendor/manifest file count mismatch: $($vendorFiles.Count) vs $($manifestEntries.Count)"
}
foreach ($file in $vendorFiles) {
    $relativePath = Get-RelativeVendorPath $file.FullName
    if (-not $manifestEntries.ContainsKey($relativePath)) {
        throw "vendor file absent from manifest: $relativePath"
    }
    $actualHash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualHash -ne $manifestEntries[$relativePath]) {
        throw "vendor hash mismatch: $relativePath"
    }
}
$mutableVendorFiles = @($vendorFiles | Where-Object { -not $_.IsReadOnly })
if ($mutableVendorFiles.Count -ne 0) {
    throw "vendor source is not immutable: $($mutableVendorFiles[0].FullName)"
}

# 上游源文件清单是跨操作系统 RM 和模式设置部分的权威依据。
# 此处有意不选取 kernel-open/：它是 Linux kbuild/ABI 前端参考，
# 并非 System 实现。
$sourceLists = @(
    [PSCustomObject]@{ Component = 'rm'; ListPath = (Join-Path $vendorSourceRoot 'src\nvidia\srcs.mk') },
    [PSCustomObject]@{ Component = 'modeset'; ListPath = (Join-Path $vendorSourceRoot 'src\nvidia-modeset\srcs.mk') }
)
$records = New-Object 'System.Collections.Generic.List[object]'
$componentIndexes = @{ rm = 0; modeset = 0 }
foreach ($sourceList in $sourceLists) {
    if (-not (Test-Path -LiteralPath $sourceList.ListPath -PathType Leaf)) {
        throw "source list is missing: $($sourceList.ListPath)"
    }
    $listDirectory = Split-Path -Parent $sourceList.ListPath
    $listRelative = Get-RelativeVendorPath $sourceList.ListPath
    $lineNumber = 0
    foreach ($line in @(Get-Content -LiteralPath $sourceList.ListPath)) {
        $lineNumber++
        $sourceMatch = [regex]::Match($line, '^\s*(SRCS|SRCS_CXX)\s*\+=\s+(.+?)\s*$')
        if (-not $sourceMatch.Success) { continue }
        $sourceKind = $sourceMatch.Groups[1].Value
        $sourceEntry = $sourceMatch.Groups[2].Value.Trim()
        if ($sourceEntry.Contains('$(')) { throw ('unexpanded source-list entry: {0}:{1}' -f $listRelative, $lineNumber) }
        $sourceFullPath = [System.IO.Path]::GetFullPath((Join-Path $listDirectory ($sourceEntry -replace '/', '\')))
        if (-not (Test-Path -LiteralPath $sourceFullPath -PathType Leaf)) {
            throw ('source-list file is missing: {0}:{1} -> {2}' -f $listRelative, $lineNumber, $sourceEntry)
        }
        $extension = [System.IO.Path]::GetExtension($sourceFullPath).ToLowerInvariant()
        if (($sourceKind -eq 'SRCS' -and $extension -ne '.c') -or
            ($sourceKind -eq 'SRCS_CXX' -and $extension -ne '.cpp')) {
            throw ('source-list language mismatch: {0}:{1} -> {2}' -f $listRelative, $lineNumber, $sourceEntry)
        }
        $componentIndexes[$sourceList.Component]++
        $sourceId = '{0}-{1:D4}' -f $sourceList.Component, $componentIndexes[$sourceList.Component]
        $objectRelative = 'objects/host-unix-probe/{0}/{1:D4}.o' -f $sourceList.Component, $componentIndexes[$sourceList.Component]
        $objectFullPath = Join-Path $outputRoot ($objectRelative.Replace('/', '\'))
        New-Item -ItemType Directory -Path (Split-Path -Parent $objectFullPath) -Force | Out-Null
        $records.Add([PSCustomObject]@{
            Id = $sourceId
            Component = $sourceList.Component
            Language = $(if ($sourceKind -eq 'SRCS_CXX') { 'cxx' } else { 'c' })
            Source = Get-RelativeVendorPath $sourceFullPath
            SourceFullPath = $sourceFullPath
            Origin = ('{0}:{1}' -f $listRelative, $lineNumber)
            Object = $objectRelative
            ObjectFullPath = $objectFullPath
            Status = 'planned'
            ExitCode = ''
            DiagnosticCount = 0
            NmStatus = 'not-run'
        })
    }
}

$sourceReportLines = New-Object 'System.Collections.Generic.List[string]'
$sourceReportLines.Add('# System Phase 1 deterministic vendor source selection')
$sourceReportLines.Add('# Authority: upstream src/nvidia/srcs.mk and src/nvidia-modeset/srcs.mk')
$sourceReportLines.Add('# kernel-open/ is excluded from this OS-agnostic inventory')
$sourceReportLines.Add(('# component{0}language{0}source{0}origin' -f $tab))
foreach ($record in $records) {
    $sourceReportLines.Add(('{0}{4}{1}{4}{2}{4}{3}' -f $record.Component, $record.Language, $record.Source, $record.Origin, $tab))
}
Write-AsciiLines $sourceListReport $sourceReportLines
$sourceListHash = (Get-FileHash -LiteralPath $sourceListReport -Algorithm SHA256).Hash.ToLowerInvariant()

# 生成的版本头文件属于构建产物，不算对供应商代码的修改。
Write-AsciiLines (Join-Path $rmGeneratedRoot 'version.h') @('#define NVIDIA_VERSION "595.84"')
Write-AsciiLines (Join-Path $modesetGeneratedRoot 'version.h') @('#define NVIDIA_VERSION "595.84"')

$rmIncludesRelative = @(
    'src\nvidia\kernel\inc',
    'src\nvidia\interface',
    'src\common\sdk\nvidia\inc',
    'src\common\sdk\nvidia\inc\hw',
    'src\nvidia\arch\nvalloc\common\inc',
    'src\nvidia\arch\nvalloc\common\inc\gsp',
    'src\nvidia\arch\nvalloc\common\inc\deprecated',
    'src\nvidia\arch\nvalloc\unix\include',
    'src\nvidia\inc',
    'src\nvidia\inc\os',
    'src\common\shared\inc',
    'src\common\inc',
    'src\common\uproc\os\libos-v2.0.0\include',
    'src\common\uproc\os\common\include',
    'src\common\inc\swref',
    'src\common\inc\swref\published',
    'src\nvidia\generated',
    'src\common\nvswitch\kernel\inc',
    'src\common\nvswitch\interface',
    'src\common\nvswitch\common\inc',
    'src\common\inc\displayport',
    'src\common\nvlink\interface',
    'src\common\nvlink\inband\interface',
    'src\nvidia\src\mm\uvm\interface',
    'src\nvidia\inc\libraries',
    'src\nvidia\src\libraries',
    'src\nvidia\inc\kernel',
    'src\nvidia\src\libraries\libspdm\3.5.0\include',
    'src\nvidia\src\libraries\libspdm\3.5.0\include\hal',
    'src\nvidia\src\libraries\libspdm\3.5.0\os_stub\include',
    'src\nvidia\src\libraries\libspdm\3.5.0\os_stub',
    'src\nvidia\src\libraries\libspdm\3.5.0\os_stub\cryptlib_null',
    'src\nvidia\src\libraries\libspdm\nvidia',
    'src\common\mbedtls\3.6.2\include',
    'src\common\mbedtls\3.6.2\nvidia'
)
$modesetIncludesRelative = @(
    'src\common\sdk\nvidia\inc',
    'src\common\sdk\nvidia\inc\hw',
    'src\common\shared\inc',
    'src\common\inc',
    'src\common\softfloat\nvidia',
    'src\common\softfloat\source\include',
    'src\common\softfloat\source\8086-SSE',
    'src\common\unix\common\utils\interface',
    'src\common\unix\common\inc',
    'src\common\modeset',
    'src\nvidia-modeset\os-interface\include',
    'src\nvidia-modeset\kapi\interface',
    'src\nvidia\arch\nvalloc\unix\include',
    'src\nvidia-modeset\interface',
    'src\nvidia-modeset\include',
    'src\nvidia-modeset\kapi\include',
    'src\nvidia-modeset\generated',
    'src\common\displayport\inc',
    'src\common\displayport\inc\dptestutil',
    'src\common\inc\displayport',
    'src\common\unix\nvidia-3d\interface',
    'src\common\unix\nvidia-push\interface',
    'src\common\unix\nvidia-3d\include',
    'src\common\unix\nvidia-push\include',
    'src\common\unix\xzminidec\interface',
    'src\common\unix\nvidia-headsurface',
    'src\nvidia-modeset\src\shaders'
)
$missingIncludeDirs = New-Object 'System.Collections.Generic.List[string]'
$allIncludeRelative = @($rmIncludesRelative + $modesetIncludesRelative)
foreach ($relativeInclude in $allIncludeRelative) {
    if (-not (Test-Path -LiteralPath (Join-Path $vendorSourceRoot $relativeInclude) -PathType Container)) {
        if (-not $missingIncludeDirs.Contains($relativeInclude)) { $missingIncludeDirs.Add($relativeInclude) }
    }
}

$cpuOpsysHeader = Join-Path $vendorSourceRoot 'src\common\sdk\nvidia\inc\cpuopsys.h'
$commonProbeDefines = @(
    '-DNV_LINUX', '-DNV_UNIX', '-DNV_X86_64', '-DNV_ARCH_BITS=64',
    '-D_LANGUAGE_C', '-D__NO_CTYPE', '-DLOCK_VAL_ENABLED=0',
    '-DPORT_ATOMIC_64_BIT_SUPPORTED=1', '-DPORT_IS_KERNEL_BUILD=1',
    '-DPORT_IS_CHECKED_BUILD=0', '-DPORT_MODULE_atomic=1',
    '-DPORT_MODULE_core=1', '-DPORT_MODULE_cpu=1', '-DPORT_MODULE_crypto=1',
    '-DPORT_MODULE_debug=1', '-DPORT_MODULE_memory=1', '-DPORT_MODULE_safe=1',
    '-DPORT_MODULE_string=1', '-DPORT_MODULE_sync=1', '-DPORT_MODULE_thread=1',
    '-DPORT_MODULE_util=1', '-DPORT_MODULE_example=0', '-DPORT_MODULE_mmio=0',
    '-DPORT_MODULE_time=0', '-DRS_STANDALONE=0', '-DRS_STANDALONE_TEST=0',
    '-DRS_COMPATABILITY_MODE=1', '-DRS_PROVIDES_API_STATE=0',
    '-DNV_CONTAINERS_NO_TEMPLATES', '-DINCLUDE_NVLINK_LIB',
    '-DINCLUDE_NVSWITCH_LIB', '-DNV_PRINTF_STRINGS_ALLOWED=1',
    '-DNV_ASSERT_FAILED_USES_STRINGS=1', '-DPORT_ASSERT_FAILED_USES_STRINGS=1'
)
$commonHostFlags = @(
    '-fno-strict-aliasing', '-fno-omit-frame-pointer', '-Wformat=2',
    '-Wno-unused-parameter', '-O2', '-ffreestanding', '-fno-stack-protector',
    '-msoft-float', '-mno-red-zone', '-mno-mmx', '-mno-sse', '-mno-sse2',
    '-mno-3dnow', '-fno-pic', '-fno-pie', '-ffunction-sections', '-fdata-sections'
)

$rmIncludeArgs = @('-I' + $rmGeneratedRoot) + ($rmIncludesRelative | ForEach-Object { '-I' + (Join-Path $vendorSourceRoot $_) })
$modesetIncludeArgs = @('-I' + $modesetGeneratedRoot) + ($modesetIncludesRelative | ForEach-Object { '-I' + (Join-Path $vendorSourceRoot $_) })
$rmCFlags = @('-include', $cpuOpsysHeader) + $rmIncludeArgs + $commonProbeDefines + $commonHostFlags + @(
    '-Werror-implicit-function-declaration', '-Wwrite-strings', '-Wundef', '--std=gnu11'
)
$modesetBaseFlags = @('-include', $cpuOpsysHeader) + $modesetIncludeArgs + @(
    '-DNV_LINUX', '-DNV_UNIX', '-DNV_X86_64', '-DNV_ARCH_BITS=64',
    '-DNDEBUG', '-D_LANGUAGE_C', '-D__NO_CTYPE', '-DNV_CPU_INTRINSICS_KERNEL',
    '-DNVHDMIPKT_RM_CALLS_INTERNAL=0', '-DNVHDMIPKT_NVKMS', '-DSOFTFLOAT_ROUND_ODD',
    '-DSOFTFLOAT_FAST_DIV32TO16', '-DSOFTFLOAT_FAST_DIV64TO32', '-DNVT_USE_NVKMS',
    '-DNV_SMG_IN_NVKMS'
) + $commonHostFlags + @(
    '-Wformat', '-Wreturn-type', '-Wswitch', '-Wunused-local-typedefs',
    '-Wchar-subscripts', '-Wparentheses', '-Wpointer-arith', '-Wcast-qual',
    '-Wall', '-Wextra', '-Wno-sign-compare', '-Wno-unused-parameter',
    '-Wno-missing-field-initializers', '-Wno-format-zero-length',
    '-Wmissing-declarations'
)
$rmCompiler = (Get-Command gcc -ErrorAction Stop).Source
$modesetCompiler = (Get-Command g++ -ErrorAction Stop).Source
$compilerVersion = (& $rmCompiler --version 2>&1 | Select-Object -First 1).ToString()

$compileLogLines = New-Object 'System.Collections.Generic.List[string]'
$compileLogLines.Add('# System Phase 1 vendor compile log')
$compileLogLines.Add('# profile=host-unix-probe')
$compileLogLines.Add('# vendor semantic patches=0')
$compileLogLines.Add(('# compiler={0}' -f $compilerVersion))
$compileLogLines.Add('# -mcmodel=kernel omitted because the Windows host compiler rejects it; -fno-pie added')
$compileLogLines.Add('# NV_LINUX/NV_UNIX are probe defines matching the upstream Unix build profile; no kernel-open code is compiled')
foreach ($missingInclude in $missingIncludeDirs) { $compileLogLines.Add(('# missing optional/upstream include directory: {0}' -f $missingInclude)) }

if (-not $SkipCompile) {
    for ($recordIndex = 0; $recordIndex -lt $records.Count; $recordIndex++) {
        $record = $records[$recordIndex]
        $args = if ($record.Component -eq 'rm') { @($rmCFlags) } else { @($modesetBaseFlags) }
        if ($record.Component -eq 'modeset' -and $record.Language -eq 'c') {
            $args += @('-Wimplicit', '-Wstrict-prototypes', '-Wmissing-prototypes', '-std=gnu11')
        } elseif ($record.Component -eq 'modeset') {
            $args += @('-std=gnu++11', '-fno-operator-names', '-fno-rtti', '-fno-exceptions', '-fcheck-new')
        }
        $args += @('-c', $record.SourceFullPath, '-o', $record.ObjectFullPath)
        $compiler = if ($record.Language -eq 'c') { $rmCompiler } else { $modesetCompiler }
        $compileLogLines.Add(('--- {0} {1} {2} ---' -f $record.Id, $record.Component, $record.Source))
        $previousErrorActionPreference = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            $diagnostics = @(& $compiler @args 2>&1 | ForEach-Object { $_.ToString() })
            $exitCode = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $previousErrorActionPreference
        }
        foreach ($diagnostic in $diagnostics) { $compileLogLines.Add($diagnostic) }
        $record.ExitCode = $exitCode
        $record.DiagnosticCount = $diagnostics.Count
        if (($exitCode -eq 0) -and (Test-Path -LiteralPath $record.ObjectFullPath -PathType Leaf)) {
            $record.Status = 'compiled'
        } else {
            $record.Status = 'compile_failed'
            if (Test-Path -LiteralPath $record.ObjectFullPath -PathType Leaf) {
                Remove-Item -LiteralPath $record.ObjectFullPath -Force
            }
        }
        $compileLogLines.Add(('status={0};exit={1};diagnostics={2}' -f $record.Status, $record.ExitCode, $record.DiagnosticCount))
        if ((($recordIndex + 1) % 25) -eq 0 -or ($recordIndex + 1) -eq $records.Count) {
            Write-Output ('PROGRESS {0}/{1}: compiled={2}, failed={3}' -f ($recordIndex + 1), $records.Count, @($records | Where-Object Status -eq 'compiled').Count, @($records | Where-Object Status -eq 'compile_failed').Count)
        }
    }
} else {
    $compileLogLines.Add('# compile skipped by -SkipCompile')
}
Write-AsciiLines $compileLog $compileLogLines

$definitions = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::Ordinal)
$undefinedBySymbol = New-Object 'System.Collections.Generic.Dictionary[string,object]' ([System.StringComparer]::Ordinal)
$undefinedTypeBySymbol = New-Object 'System.Collections.Generic.Dictionary[string,string]' ([System.StringComparer]::Ordinal)
$nmFailures = New-Object 'System.Collections.Generic.List[string]'
$compiledRecords = @($records | Where-Object { $_.Status -eq 'compiled' })
foreach ($record in $compiledRecords) {
    $previousErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $nmLines = @(& nm -P -- $record.ObjectFullPath 2>&1 | ForEach-Object { $_.ToString() })
        $nmExitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }
    if ($nmExitCode -ne 0) {
        $record.NmStatus = 'nm_failed'
        $nmFailures.Add(('{0}{3}{1}{3}exit={2}' -f $record.Id, $record.Source, $nmExitCode, $tab))
        continue
    }
    $record.NmStatus = 'ok'
    foreach ($nmLine in $nmLines) {
        $trimmedNmLine = $nmLine.Trim()
        if ([string]::IsNullOrWhiteSpace($trimmedNmLine)) { continue }
        $nmFields = $trimmedNmLine -split '\s+'
        if ($nmFields.Count -lt 2) {
            $nmFailures.Add(('{0}{3}{1}{3}unparsed={2}' -f $record.Id, $record.Source, $trimmedNmLine, $tab))
            continue
        }
        $symbolName = $nmFields[0]
        $symbolType = $nmFields[1]
        if ($symbolType -in @('U', 'w', 'v')) {
            if (-not $undefinedBySymbol.ContainsKey($symbolName)) {
                $undefinedBySymbol.Add($symbolName, (New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::Ordinal)))
                $undefinedTypeBySymbol.Add($symbolName, $symbolType)
            }
            [void]$undefinedBySymbol[$symbolName].Add($record.Id)
        } elseif ($symbolType -ne '?') {
            [void]$definitions.Add($symbolName)
        }
    }
}

# 在运行 nm 后重写对象报告，以确保其中每个对象的 nm 状态都是最终结果。
$objectReportLines = New-Object 'System.Collections.Generic.List[string]'
$objectReportLines.Add('# System Phase 1 vendor object inventory')
$objectReportLines.Add(('# source_list_sha256={0}' -f $sourceListHash))
$objectReportLines.Add(('# status{0}component{0}language{0}id{0}source{0}object{0}exit_code{0}nm_status' -f $tab))
foreach ($record in $records) {
    $objectReportLines.Add(('{0}{8}{1}{8}{2}{8}{3}{8}{4}{8}{5}{8}{6}{8}{7}' -f $record.Status, $record.Component, $record.Language, $record.Id, $record.Source, $record.Object, $record.ExitCode, $record.NmStatus, $tab))
}
Write-AsciiLines $objectReport $objectReportLines

$unresolvedNames = @($undefinedBySymbol.Keys | Where-Object { -not $definitions.Contains($_) } | Sort-Object)
$resolvedUndefinedCount = $undefinedBySymbol.Count - $unresolvedNames.Count
$unresolvedReportLines = New-Object 'System.Collections.Generic.List[string]'
$unresolvedReportLines.Add('# System Phase 1 unresolved external symbol inventory')
$unresolvedReportLines.Add(('# source_list_sha256={0}' -f $sourceListHash))
$unresolvedReportLines.Add(('# selected_sources={0};compiled_objects={1};compile_failures={2};resolved_partial_symbols={3};unresolved_symbols={4}' -f $records.Count, $compiledRecords.Count, @($records | Where-Object Status -eq 'compile_failed').Count, $resolvedUndefinedCount, $unresolvedNames.Count))
$unresolvedReportLines.Add('# classification=deterministic symbol-name heuristic; no implementation is supplied')
$unresolvedReportLines.Add('# This is a partial host-object inventory; it is not a fake linked driver and does not provide OS/GPU services.')
$categoryCounts = @{}
foreach ($category in $categoryOrder) { $categoryCounts[$category] = 0 }
foreach ($symbolName in $unresolvedNames) { $categoryCounts[(Get-SymbolCategory $symbolName)]++ }
$unresolvedReportLines.Add('[category_summary]')
$unresolvedReportLines.Add(('# category{0}symbol_count' -f $tab))
foreach ($category in $categoryOrder) {
    $unresolvedReportLines.Add(('{0}{2}{1}' -f $category, $categoryCounts[$category], $tab))
}
$unresolvedReportLines.Add('[compile_failures]')
$unresolvedReportLines.Add(('# id{0}component{0}language{0}source{0}exit_code{0}diagnostic_count' -f $tab))
foreach ($record in @($records | Where-Object Status -eq 'compile_failed')) {
    $unresolvedReportLines.Add(('{0}{6}{1}{6}{2}{6}{3}{6}{4}{6}{5}' -f $record.Id, $record.Component, $record.Language, $record.Source, $record.ExitCode, $record.DiagnosticCount, $tab))
}
$unresolvedReportLines.Add('[unresolved_external_symbols]')
$unresolvedReportLines.Add(('# category{0}symbol{0}reference_count{0}reference_object_ids{0}type' -f $tab))
foreach ($symbolName in $unresolvedNames) {
    $objectIds = @($undefinedBySymbol[$symbolName] | Sort-Object)
    $unresolvedReportLines.Add(('{0}{5}{1}{5}{2}{5}{3}{5}{4}' -f (Get-SymbolCategory $symbolName), $symbolName, $objectIds.Count, ($objectIds -join ','), $undefinedTypeBySymbol[$symbolName], $tab))
}
$unresolvedReportLines.Add('[nm_failures]')
foreach ($nmFailure in @($nmFailures | Sort-Object)) { $unresolvedReportLines.Add($nmFailure) }
Write-AsciiLines $unresolvedReport $unresolvedReportLines

Write-Output ('SOURCE_LIST={0}' -f $sourceListReport)
Write-Output ('OBJECT_REPORT={0}' -f $objectReport)
Write-Output ('UNRESOLVED_REPORT={0}' -f $unresolvedReport)
Write-Output ('SUMMARY selected={0};compiled={1};compile_failed={2};unresolved={3};nm_failures={4}' -f $records.Count, $compiledRecords.Count, @($records | Where-Object Status -eq 'compile_failed').Count, $unresolvedNames.Count, $nmFailures.Count)
