[CmdletBinding()]
param(
    [switch]$SkipCompile,
    [switch]$SkipLink
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$vendorRoot = Join-Path $repoRoot 'third_party\nvidia\595.84'
$vendorSourceRoot = Join-Path $vendorRoot 'open-gpu-kernel-modules'
$rmSourceRoot = Join-Path $vendorSourceRoot 'src\nvidia'
$outputRoot = Join-Path $repoRoot 'build\nvidia\phase3'
$objectRoot = Join-Path $outputRoot 'objects\rm'
$abiSourceRoot = Join-Path $repoRoot 'kernel\drivers\gpu\nvidia\vendor_abi'
$abiObjectRoot = Join-Path $outputRoot 'objects\vendor_abi'
$systemInputRoot = Join-Path $repoRoot 'build\kernel'
$systemObjectRoot = Join-Path $outputRoot 'objects\system'
$generatedRoot = Join-Path $outputRoot 'generated\rm'
$sourceReport = Join-Path $outputRoot 'vendor_rm_sources.txt'
$objectReport = Join-Path $outputRoot 'vendor_rm_objects.txt'
$compileLog = Join-Path $outputRoot 'vendor_rm_compile.log'
$linkLog = Join-Path $outputRoot 'vendor_rm_link.log'
$unresolvedReport = Join-Path $outputRoot 'vendor_rm_unresolved.txt'
$linkObject = Join-Path $outputRoot 'vendor-rm.o'
$objectList = Join-Path $outputRoot 'vendor_rm_objects.list'
$abiSourceReport = Join-Path $outputRoot 'vendor_abi_sources.txt'
$abiObjectReport = Join-Path $outputRoot 'vendor_abi_objects.txt'
$abiCompileLog = Join-Path $outputRoot 'vendor_abi_compile.log'
$abiObjectList = Join-Path $outputRoot 'vendor_abi_objects.list'
$abiLinkObjectList = Join-Path $outputRoot 'vendor_abi_link_objects.list'
$systemObjectReport = Join-Path $outputRoot 'system_probe_objects.txt'
$systemObjectList = Join-Path $outputRoot 'system_probe_objects.list'
$systemLinkLog = Join-Path $outputRoot 'vendor_rm_system_link.log'
$systemLinkObject = Join-Path $outputRoot 'vendor-rm-system.o'
$tab = [char]9

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

function Get-RelativeRepoPath([string]$FullPath) {
    $repoRootFull = [System.IO.Path]::GetFullPath($repoRoot)
    $fullPathNormalized = [System.IO.Path]::GetFullPath($FullPath)
    $prefix = $repoRootFull + [System.IO.Path]::DirectorySeparatorChar
    if (-not $fullPathNormalized.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "path escaped repository root: $FullPath"
    }
    return $fullPathNormalized.Substring($prefix.Length).Replace('\', '/')
}

if (-not (Test-Path -LiteralPath $vendorSourceRoot -PathType Container)) {
    throw "vendor source root is missing: $vendorSourceRoot"
}
if (-not (Test-Path -LiteralPath $abiSourceRoot -PathType Container)) {
    throw "vendor ABI source root is missing: $abiSourceRoot"
}
New-Item -ItemType Directory -Path $objectRoot -Force | Out-Null
New-Item -ItemType Directory -Path $abiObjectRoot -Force | Out-Null
New-Item -ItemType Directory -Path $systemObjectRoot -Force | Out-Null
New-Item -ItemType Directory -Path $generatedRoot -Force | Out-Null

$versionPath = Join-Path $vendorRoot 'VENDOR_VERSION'
$manifestPath = Join-Path $vendorRoot 'VENDOR_SHA256SUMS'
if (-not (Test-Path -LiteralPath $versionPath -PathType Leaf)) { throw 'missing VENDOR_VERSION' }
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) { throw 'missing VENDOR_SHA256SUMS' }
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
$vendorFiles = @(Get-ChildItem -LiteralPath $vendorSourceRoot -Recurse -File)
$manifestEntries = New-Object 'System.Collections.Generic.Dictionary[string,string]' ([System.StringComparer]::OrdinalIgnoreCase)
foreach ($line in @(Get-Content -LiteralPath $manifestPath)) {
    if ([string]::IsNullOrWhiteSpace($line) -or $line.StartsWith('#')) { continue }
    $manifestMatch = [regex]::Match($line, '^([0-9a-fA-F]{64})  (.+)$')
    if (-not $manifestMatch.Success) { throw "invalid manifest line: $line" }
    $manifestEntries.Add($manifestMatch.Groups[2].Value, $manifestMatch.Groups[1].Value.ToLowerInvariant())
}
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
    if (-not $file.IsReadOnly) { throw "vendor source is not immutable: $($file.FullName)" }
}

$sourceListPath = Join-Path $rmSourceRoot 'srcs.mk'
if (-not (Test-Path -LiteralPath $sourceListPath -PathType Leaf)) {
    throw "RM source list is missing: $sourceListPath"
}

$records = New-Object 'System.Collections.Generic.List[object]'
$sourceIndex = 0
$sourceListRelative = Get-RelativeVendorPath $sourceListPath
foreach ($line in @(Get-Content -LiteralPath $sourceListPath)) {
    $sourceMatch = [regex]::Match($line, '^\s*(SRCS|SRCS_CXX)\s*\+=\s+(.+?)\s*$')
    if (-not $sourceMatch.Success) { continue }
    $sourceKind = $sourceMatch.Groups[1].Value
    $sourceEntry = $sourceMatch.Groups[2].Value.Trim()
    if ($sourceEntry.Contains('$(')) {
        throw "unexpanded source-list entry: $sourceListRelative -> $sourceEntry"
    }
    $sourceFullPath = [System.IO.Path]::GetFullPath((Join-Path $rmSourceRoot ($sourceEntry -replace '/', '\')))
    if (-not (Test-Path -LiteralPath $sourceFullPath -PathType Leaf)) {
        throw "source-list file is missing: $sourceListRelative -> $sourceEntry"
    }
    $extension = [System.IO.Path]::GetExtension($sourceFullPath).ToLowerInvariant()
    if (($sourceKind -eq 'SRCS' -and $extension -ne '.c') -or
        ($sourceKind -eq 'SRCS_CXX' -and $extension -ne '.cpp')) {
        throw "source-list language mismatch: $sourceListRelative -> $sourceEntry"
    }
    $sourceIndex++
    $records.Add([PSCustomObject]@{
        Id = 'rm-{0:D4}' -f $sourceIndex
        Component = 'rm'
        Language = $(if ($sourceKind -eq 'SRCS_CXX') { 'cxx' } else { 'c' })
        Source = Get-RelativeVendorPath $sourceFullPath
        SourceFullPath = $sourceFullPath
        Origin = ('{0}:{1}' -f $sourceListRelative, $sourceIndex)
        Object = Join-Path $objectRoot ('{0:D4}.o' -f $sourceIndex)
        Status = 'planned'
        ExitCode = ''
        DiagnosticCount = 0
        NmStatus = 'not-run'
    })
}

# 这是上游 Makefile 生成的同一对符号。它在不可变的供应商代码树之外生成，
# 只作为链接输入。
$nvidSource = Join-Path $generatedRoot 'g_nvid_string.c'
Write-AsciiLines $nvidSource @(
    'const char NVRM_ID[] = "nvidia id: UNIX Open Kernel Module for System RM, baseline 595.84";',
    'const char *const pNVRM_ID = NVRM_ID + 11;'
)
$sourceIndex++
$records.Add([PSCustomObject]@{
    Id = 'rm-{0:D4}' -f $sourceIndex
    Component = 'rm-generated'
    Language = 'c'
    Source = 'build/nvidia/phase3/generated/rm/g_nvid_string.c'
    SourceFullPath = $nvidSource
    Origin = 'phase3-generated:g_nvid_string.c'
    Object = Join-Path $objectRoot ('{0:D4}.o' -f $sourceIndex)
    Status = 'planned'
    ExitCode = ''
    DiagnosticCount = 0
    NmStatus = 'not-run'
})

$sourceReportLines = New-Object 'System.Collections.Generic.List[string]'
$sourceReportLines.Add('# System Phase 3 RM vendor source selection')
$sourceReportLines.Add('# Authority: upstream src/nvidia/srcs.mk plus generated NVRM_ID link input')
$sourceReportLines.Add('# kernel-open/ and src/nvidia-modeset/ are not part of the Phase 3 RM closure')
$sourceReportLines.Add(('# component{0}language{0}source{0}origin' -f $tab))
foreach ($record in $records) {
    $sourceReportLines.Add(('{0}{4}{1}{4}{2}{4}{3}' -f $record.Component, $record.Language, $record.Source, $record.Origin, $tab))
}
Write-AsciiLines $sourceReport $sourceReportLines

$cpuOpsysHeader = Join-Path $vendorSourceRoot 'src\common\sdk\nvidia\inc\cpuopsys.h'
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
$includeArgs = @($rmIncludesRelative | ForEach-Object { '-I' + (Join-Path $vendorSourceRoot $_) })
$includeArgs += '-I' + $generatedRoot
$abiIncludeArgs = @(
    ('-I' + (Join-Path $repoRoot 'include'));
    ('-I' + (Join-Path $repoRoot 'kernel\drivers\gpu\nvidia\port\include'));
    ('-I' + (Join-Path $abiSourceRoot 'include'));
    ('-I' + (Join-Path $vendorSourceRoot 'kernel-open'));
    ('-I' + (Join-Path $vendorSourceRoot 'kernel-open\common\inc'))
)
$abiIncludeArgs += $includeArgs

$defines = @(
    '-DNVRM', '-DNV_LINUX', '-DNV_UNIX', '-DNV_X86_64', '-DNV_ARCH_BITS=64',
    '-D_LANGUAGE_C', '-D__NO_CTYPE', '-DLOCK_VAL_ENABLED=0',
    '-DPORT_ATOMIC_64_BIT_SUPPORTED=1', '-DPORT_IS_KERNEL_BUILD=1',
    '-DPORT_IS_CHECKED_BUILD=0', '-DPORT_MODULE_atomic=1', '-DPORT_MODULE_core=1',
    '-DPORT_MODULE_cpu=1', '-DPORT_MODULE_crypto=1', '-DPORT_MODULE_debug=1',
    '-DPORT_MODULE_memory=1', '-DPORT_MODULE_safe=1', '-DPORT_MODULE_string=1',
    '-DPORT_MODULE_sync=1', '-DPORT_MODULE_thread=1', '-DPORT_MODULE_util=1',
    '-DPORT_MODULE_example=0', '-DPORT_MODULE_mmio=0', '-DPORT_MODULE_time=0',
    '-DRS_STANDALONE=0', '-DRS_STANDALONE_TEST=0', '-DRS_COMPATABILITY_MODE=1',
    '-DRS_PROVIDES_API_STATE=0', '-DNV_CONTAINERS_NO_TEMPLATES',
    '-DINCLUDE_NVLINK_LIB', '-DINCLUDE_NVSWITCH_LIB',
    '-DNV_PRINTF_STRINGS_ALLOWED=0', '-DNV_ASSERT_FAILED_USES_STRINGS=0',
    '-DPORT_ASSERT_FAILED_USES_STRINGS=0'
)
$abiDefines = $defines + '-DOS_KERNEL_BUILD'

# 与上游 RM 配置保持一致。优先使用仓库中的 freestanding ELF 交叉工具链，
# 使可重定位链接的对象格式与上游 Linux 构建相同。本机编译器仅作为诊断回退。
$commonFlags = @(
    '-fno-strict-aliasing', '-fno-omit-frame-pointer', '-Wformat=2',
    '-Wno-unused-parameter', '-O2', '-ffreestanding', '-fno-stack-protector',
    '-fno-common', '-fno-pic', '-ffunction-sections', '-fdata-sections',
    '-msoft-float', '-mno-red-zone', '-mno-mmx', '-mno-sse', '-mno-sse2', '-mno-3dnow',
    '-Werror-implicit-function-declaration', '-Wwrite-strings', '-Wundef'
)
$cFlags = @('-include', $cpuOpsysHeader) + $includeArgs + $defines + $commonFlags + @('--std=gnu11')
$compilerCommand = Get-Command x86_64-elf-gcc -ErrorAction SilentlyContinue
if ($null -eq $compilerCommand) { $compilerCommand = Get-Command gcc -ErrorAction Stop }
$cxxCompilerCommand = Get-Command x86_64-elf-g++ -ErrorAction SilentlyContinue
if ($null -eq $cxxCompilerCommand) { $cxxCompilerCommand = Get-Command g++ -ErrorAction Stop }
$linkerCommand = Get-Command x86_64-elf-ld -ErrorAction SilentlyContinue
if ($null -eq $linkerCommand) { $linkerCommand = Get-Command ld -ErrorAction Stop }
$objcopyCommand = Get-Command x86_64-elf-objcopy -ErrorAction SilentlyContinue
if ($null -eq $objcopyCommand) { $objcopyCommand = Get-Command objcopy -ErrorAction Stop }
$peObjcopyCommand = Get-Command x86_64-w64-mingw32-objcopy -ErrorAction SilentlyContinue
if ($null -eq $peObjcopyCommand) { $peObjcopyCommand = Get-Command objcopy -ErrorAction Stop }
$nmCommand = Get-Command x86_64-elf-nm -ErrorAction SilentlyContinue
if ($null -eq $nmCommand) { $nmCommand = Get-Command nm -ErrorAction Stop }
$compiler = $compilerCommand.Source
$cxxCompiler = $cxxCompilerCommand.Source
$linker = $linkerCommand.Source
$objcopy = $objcopyCommand.Source
$peObjcopy = $peObjcopyCommand.Source
$nm = $nmCommand.Source
$useKernelCodeModel = [System.IO.Path]::GetFileName($compiler) -like 'x86_64-elf-gcc*'
if ($useKernelCodeModel) { $cFlags += '-mcmodel=kernel' }
$abiCFlags = @('-include', $cpuOpsysHeader) + $abiIncludeArgs + $abiDefines + $commonFlags + @('--std=gnu11')
$abiAsmFlags = @(
    '-ffreestanding', '-fno-stack-protector', '-fno-common', '-fno-pic',
    '-mno-red-zone', '-mno-mmx', '-mno-sse', '-mno-sse2', '-mno-3dnow'
)
if ($useKernelCodeModel) { $abiAsmFlags += '-mcmodel=kernel' }

$compileLogLines = New-Object 'System.Collections.Generic.List[string]'
$compileLogLines.Add('# System Phase 3 RM vendor compile log')
$compileLogLines.Add('# profile=upstream-NVRM-System-host-probe')
$compileLogLines.Add('# vendor semantic patches=0; immutable vendor tree verified')
$compileLogLines.Add(('# compiler={0}' -f $compiler))
$compileLogLines.Add(('# linker={0}; nm={1}; objcopy={2}' -f $linker, $nm, $objcopy))
$compileLogLines.Add(('# kernel_code_model={0}' -f $useKernelCodeModel))
$compileLogLines.Add(('# rm_sources_including_generated={0}' -f $records.Count))

if (-not $SkipCompile) {
    for ($recordIndex = 0; $recordIndex -lt $records.Count; $recordIndex++) {
        $record = $records[$recordIndex]
        $args = @($cFlags)
        if ($record.Language -eq 'cxx') {
            $args += @('--std=gnu++11', '-fno-operator-names', '-fno-rtti', '-fno-exceptions', '-fcheck-new')
        }
        $args += @('-c', $record.SourceFullPath, '-o', $record.Object)
        New-Item -ItemType Directory -Path (Split-Path -Parent $record.Object) -Force | Out-Null
        $selectedCompiler = if ($record.Language -eq 'cxx') { $cxxCompiler } else { $compiler }
        $compileLogLines.Add(('--- {0} {1} ---' -f $record.Id, $record.Source))
        $previousErrorActionPreference = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            $diagnostics = @(& $selectedCompiler @args 2>&1 | ForEach-Object { $_.ToString() })
            $exitCode = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $previousErrorActionPreference
        }
        foreach ($diagnostic in $diagnostics) { $compileLogLines.Add($diagnostic) }
        $record.ExitCode = $exitCode
        $record.DiagnosticCount = $diagnostics.Count
        if (($exitCode -eq 0) -and (Test-Path -LiteralPath $record.Object -PathType Leaf)) {
            $record.Status = 'compiled'
        } else {
            $record.Status = 'compile_failed'
            if (Test-Path -LiteralPath $record.Object -PathType Leaf) {
                Remove-Item -LiteralPath $record.Object -Force
            }
        }
        $compileLogLines.Add(('status={0};exit={1};diagnostics={2}' -f $record.Status, $record.ExitCode, $record.DiagnosticCount))
        if ((($recordIndex + 1) % 25) -eq 0 -or ($recordIndex + 1) -eq $records.Count) {
            Write-Output ('PROGRESS {0}/{1}: compiled={2}, failed={3}' -f ($recordIndex + 1), $records.Count, @($records | Where-Object Status -eq 'compiled').Count, @($records | Where-Object Status -eq 'compile_failed').Count)
        }
    }
} else {
    $compileLogLines.Add('# compile skipped by -SkipCompile')
    foreach ($record in $records) {
        if (Test-Path -LiteralPath $record.Object -PathType Leaf) { $record.Status = 'compiled' }
        else { $record.Status = 'compile_failed'; $record.ExitCode = 'skipped-missing' }
    }
}
Write-AsciiLines $compileLog $compileLogLines

$objectReportLines = New-Object 'System.Collections.Generic.List[string]'
$objectReportLines.Add('# System Phase 3 RM vendor object inventory')
$objectReportLines.Add(('# status{0}component{0}language{0}id{0}source{0}object{0}exit_code{0}diagnostic_count' -f $tab))
foreach ($record in $records) {
    $objectReportLines.Add(('{0}{8}{1}{8}{2}{8}{3}{8}{4}{8}{5}{8}{6}{8}{7}' -f $record.Status, $record.Component, $record.Language, $record.Id, $record.Source, $record.Object, $record.ExitCode, $record.DiagnosticCount, $tab))
}
Write-AsciiLines $objectReport $objectReportLines

$compiledRecords = @($records | Where-Object Status -eq 'compiled')
if ($compiledRecords.Count -eq 0) { throw 'no RM vendor object compiled' }
Write-AsciiLines $objectList @($compiledRecords | ForEach-Object { $_.Object.Replace('\', '/') })

# 在不可变的供应商代码树之外编译 System 侧 ABI 依赖闭包。C 适配器使用
# 供应商 ABI 头文件编译；汇编适配器有意限制为安全失败返回路径，并由
# nvabi_contracts.c 检查类型。
$abiRecords = New-Object 'System.Collections.Generic.List[object]'
$abiIndex = 0
# 阶段 3 是封闭的基线 ABI 依赖图。仅供阶段 5 使用的适配器由
# phase5_build.ps1 链接，避免新增固件、定时器或 DMA 支持时悄然改变
# 阶段 3 Gate 的输入。
$phase5OnlyAbiNames = @(
    'nvabi_firmware.c',
    'nvabi_dma.c',
    'nvabi_timer.c'
)
$abiFiles = @(Get-ChildItem -LiteralPath $abiSourceRoot -Recurse -File |
    Where-Object {
        $_.Extension -in @('.c', '.S') -and
        $_.Name -notin $phase5OnlyAbiNames
    } | Sort-Object FullName)
foreach ($abiFile in $abiFiles) {
    $abiIndex++
    $abiRecords.Add([PSCustomObject]@{
        Id = 'abi-{0:D4}' -f $abiIndex
        Component = 'vendor_abi'
        Language = $(if ($abiFile.Extension -eq '.S') { 'asm' } else { 'c' })
        Source = Get-RelativeRepoPath $abiFile.FullName
        SourceFullPath = $abiFile.FullName
        Object = Join-Path $abiObjectRoot ('{0:D4}.o' -f $abiIndex)
        Status = 'planned'
        ExitCode = ''
        DiagnosticCount = 0
        NmStatus = 'not-run'
    })
}
if ($abiRecords.Count -eq 0) { throw 'no vendor ABI source found' }

$abiSourceReportLines = New-Object 'System.Collections.Generic.List[string]'
$abiSourceReportLines.Add('# System Phase 3 vendor ABI source selection')
$abiSourceReportLines.Add('# C prototypes are imported from immutable NVIDIA 595.84 headers')
$abiSourceReportLines.Add('# Phase 5-only adapters are intentionally excluded from this baseline graph')
$abiSourceReportLines.Add(('# id{0}component{0}language{0}source' -f $tab))
foreach ($record in $abiRecords) {
    $abiSourceReportLines.Add(('{0}{4}{1}{4}{2}{4}{3}' -f $record.Id, $record.Component, $record.Language, $record.Source, $tab))
}
Write-AsciiLines $abiSourceReport ( $abiSourceReportLines )

$abiCompileLogLines = New-Object 'System.Collections.Generic.List[string]'
$abiCompileLogLines.Add('# System Phase 3 vendor ABI compile log')
$abiCompileLogLines.Add('# unsupported adapters fail closed and log symbol plus caller stage')
$abiCompileLogLines.Add(('# compiler={0}; object_format=ELF' -f $compiler))
$abiCompileLogLines.Add(('# abi_sources={0}' -f $abiRecords.Count))
if (-not $SkipCompile) {
    for ($abiRecordIndex = 0; $abiRecordIndex -lt $abiRecords.Count; $abiRecordIndex++) {
        $record = $abiRecords[$abiRecordIndex]
        $args = if ($record.Language -eq 'asm') { @($abiAsmFlags) } else { @($abiCFlags) }
        $args += @('-c', $record.SourceFullPath, '-o', $record.Object)
        New-Item -ItemType Directory -Path (Split-Path -Parent $record.Object) -Force | Out-Null
        $abiCompileLogLines.Add(('--- {0} {1} ---' -f $record.Id, $record.Source))
        $previousErrorActionPreference = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            $diagnostics = @(& $compiler @args 2>&1 | ForEach-Object { $_.ToString() })
            $exitCode = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $previousErrorActionPreference
        }
        foreach ($diagnostic in $diagnostics) { $abiCompileLogLines.Add($diagnostic) }
        $record.ExitCode = $exitCode
        $record.DiagnosticCount = $diagnostics.Count
        if (($exitCode -eq 0) -and (Test-Path -LiteralPath $record.Object -PathType Leaf)) {
            $record.Status = 'compiled'
        } else {
            $record.Status = 'compile_failed'
            if (Test-Path -LiteralPath $record.Object -PathType Leaf) {
                Remove-Item -LiteralPath $record.Object -Force
            }
        }
        $abiCompileLogLines.Add(('status={0};exit={1};diagnostics={2}' -f $record.Status, $record.ExitCode, $record.DiagnosticCount))
        if ((($abiRecordIndex + 1) % 10) -eq 0 -or ($abiRecordIndex + 1) -eq $abiRecords.Count) {
            Write-Output ('ABI_PROGRESS {0}/{1}: compiled={2}, failed={3}' -f ($abiRecordIndex + 1), $abiRecords.Count, @($abiRecords | Where-Object Status -eq 'compiled').Count, @($abiRecords | Where-Object Status -eq 'compile_failed').Count)
        }
    }
} else {
    $abiCompileLogLines.Add('# compile skipped by -SkipCompile')
    foreach ($record in $abiRecords) {
        if (Test-Path -LiteralPath $record.Object -PathType Leaf) { $record.Status = 'compiled' }
        else { $record.Status = 'compile_failed'; $record.ExitCode = 'skipped-missing' }
    }
}
Write-AsciiLines $abiCompileLog $abiCompileLogLines

$abiObjectReportLines = New-Object 'System.Collections.Generic.List[string]'
$abiObjectReportLines.Add('# System Phase 3 vendor ABI object inventory')
$abiObjectReportLines.Add(('# status{0}component{0}language{0}id{0}source{0}object{0}exit_code{0}diagnostic_count' -f $tab))
foreach ($record in $abiRecords) {
    $abiObjectReportLines.Add(('{0}{8}{1}{8}{2}{8}{3}{8}{4}{8}{5}{8}{6}{8}{7}' -f $record.Status, $record.Component, $record.Language, $record.Id, $record.Source, $record.Object, $record.ExitCode, $record.DiagnosticCount, $tab))
}
Write-AsciiLines $abiObjectReport $abiObjectReportLines
$abiCompiledRecords = @($abiRecords | Where-Object Status -eq 'compiled')
$abiCompileFailures = @($abiRecords | Where-Object Status -eq 'compile_failed').Count
if ($abiCompileFailures -ne 0) { throw "vendor ABI compile failed: $abiCompileFailures source(s)" }
Write-AsciiLines $abiObjectList @($abiCompiledRecords | ForEach-Object { $_.Object.Replace('\', '/') })
$abiLinkRecords = @($abiCompiledRecords | Where-Object { $_.Source -notlike '*vendor_abi/nvabi_contracts.c' })
if ($abiLinkRecords.Count -eq 0) { throw 'no linkable vendor ABI object compiled' }

$linkLogLines = New-Object 'System.Collections.Generic.List[string]'
$linkLogLines.Add('# System Phase 3 RM vendor relocatable link log')
$linkLogLines.Add('# link=all successfully compiled RM objects + upstream export roots')
$linkExitCode = 0
if (-not $SkipLink) {
    $linkerScript = Join-Path $rmSourceRoot 'nv-kernel.ld'
    $exportsFile = Join-Path $rmSourceRoot 'exports_link_command.txt'
    $linkArgs = @('--gc-sections', '-T', $linkerScript, '-r', '-o', $linkObject, ('@' + $objectList), ('@' + $exportsFile))
    $previousErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $linkDiagnostics = @(& $linker @linkArgs 2>&1 | ForEach-Object { $_.ToString() })
        $linkExitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }
    foreach ($line in $linkDiagnostics) { $linkLogLines.Add($line) }
    if ($linkExitCode -eq 0 -and (Test-Path -LiteralPath $linkObject -PathType Leaf)) {
        $objcopyArgs = @('--localize-symbol=memset', '--localize-symbol=memcpy', '--remove-section=.note.gnu.property', $linkObject)
        $previousErrorActionPreference = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            $objcopyDiagnostics = @(& $objcopy @objcopyArgs 2>&1 | ForEach-Object { $_.ToString() })
            $objcopyExitCode = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $previousErrorActionPreference
        }
        foreach ($line in $objcopyDiagnostics) { $linkLogLines.Add($line) }
        if ($objcopyExitCode -ne 0) { $linkExitCode = $objcopyExitCode }
    }
} else {
    $linkLogLines.Add('# link skipped by -SkipLink')
    if (-not (Test-Path -LiteralPath $linkObject -PathType Leaf)) { $linkExitCode = 1 }
}
$linkLogLines.Add(('status={0};exit={1};object={2}' -f $(if ($linkExitCode -eq 0) { 'linked' } else { 'link_failed' }), $linkExitCode, $linkObject))
Write-AsciiLines $linkLog $linkLogLines

# 此探测运行前，阶段 3 Gate 已构建生产内核。常规 MinGW 对象采用 PE/COFF 格式；
# 将副本转换到构建目录后，即可用它检查 NVIDIA ELF RM 对象是否符合真实的
# System 支持闭包，而无需修改任何生产对象或供应商源码。
$systemRecords = New-Object 'System.Collections.Generic.List[object]'
$systemIndex = 0
$systemFiles = @()
if (Test-Path -LiteralPath $systemInputRoot -PathType Container) {
    $systemFiles = @(Get-ChildItem -LiteralPath $systemInputRoot -File -Filter '*.o' | Sort-Object Name)
}
foreach ($systemFile in $systemFiles) {
    $systemIndex++
    $systemRecords.Add([PSCustomObject]@{
        Id = 'system-{0:D4}' -f $systemIndex
        Source = Get-RelativeRepoPath $systemFile.FullName
        SourceFullPath = $systemFile.FullName
        Object = Join-Path $systemObjectRoot ('{0:D4}.elf.o' -f $systemIndex)
        Status = 'planned'
        ExitCode = ''
        DiagnosticCount = 0
    })
}

$systemLinkLogLines = New-Object 'System.Collections.Generic.List[string]'
$systemLinkLogLines.Add('# System Phase 3 RM -> System support closure probe')
$systemLinkLogLines.Add('# bridge=build-only PE/COFF-to-ELF object conversion; production sources are unchanged')
$systemLinkLogLines.Add(('# pe_objcopy={0}; input_objects={1}' -f $peObjcopy, $systemRecords.Count))
$systemLinkExitCode = 0
if (-not $SkipLink) {
    if ($systemRecords.Count -eq 0) {
        $systemLinkLogLines.Add('status=link_failed;reason=production kernel object directory is empty')
        $systemLinkExitCode = 1
    } else {
        for ($systemRecordIndex = 0; $systemRecordIndex -lt $systemRecords.Count; $systemRecordIndex++) {
            $record = $systemRecords[$systemRecordIndex]
            New-Item -ItemType Directory -Path (Split-Path -Parent $record.Object) -Force | Out-Null
            $systemLinkLogLines.Add(('--- convert {0} {1} ---' -f $record.Id, $record.Source))
            $previousErrorActionPreference = $ErrorActionPreference
            $ErrorActionPreference = 'Continue'
            try {
                $diagnostics = @(& $peObjcopy -O elf64-x86-64 -- $record.SourceFullPath $record.Object 2>&1 | ForEach-Object { $_.ToString() })
                $exitCode = $LASTEXITCODE
            } finally {
                $ErrorActionPreference = $previousErrorActionPreference
            }
            foreach ($diagnostic in $diagnostics) { $systemLinkLogLines.Add($diagnostic) }
            $record.ExitCode = $exitCode
            $record.DiagnosticCount = $diagnostics.Count
            if (($exitCode -eq 0) -and (Test-Path -LiteralPath $record.Object -PathType Leaf)) {
                $record.Status = 'converted'
            } else {
                $record.Status = 'convert_failed'
                $systemLinkExitCode = 1
            }
        }
    }
} else {
    $systemLinkLogLines.Add('# conversion and link skipped by -SkipLink')
    foreach ($record in $systemRecords) {
        if (Test-Path -LiteralPath $record.Object -PathType Leaf) { $record.Status = 'converted' }
        else { $record.Status = 'convert_failed'; $record.ExitCode = 'skipped-missing' }
    }
}

$systemObjectReportLines = New-Object 'System.Collections.Generic.List[string]'
$systemObjectReportLines.Add('# System Phase 3 build-only object-format bridge inventory')
$systemObjectReportLines.Add(('# status{0}id{0}source{0}object{0}exit_code{0}diagnostic_count' -f $tab))
foreach ($record in $systemRecords) {
    $systemObjectReportLines.Add(('{0}{6}{1}{6}{2}{6}{3}{6}{4}{6}{5}' -f $record.Status, $record.Id, $record.Source, $record.Object, $record.ExitCode, $record.DiagnosticCount, $tab))
}
Write-AsciiLines $systemObjectReport $systemObjectReportLines
$systemConvertedRecords = @($systemRecords | Where-Object Status -eq 'converted')
if ($systemLinkExitCode -eq 0) {
    if ($systemConvertedRecords.Count -ne $systemRecords.Count) {
        $systemLinkExitCode = 1
    } else {
        Write-AsciiLines $systemObjectList @($systemConvertedRecords | ForEach-Object { $_.Object.Replace('\', '/') })
        Write-AsciiLines $abiLinkObjectList @($abiLinkRecords | ForEach-Object { $_.Object.Replace('\', '/') })
        $linkerScript = Join-Path $rmSourceRoot 'nv-kernel.ld'
        $exportsFile = Join-Path $rmSourceRoot 'exports_link_command.txt'
        $systemLinkArgs = @(
            '--gc-sections', '--allow-multiple-definition', '-T', $linkerScript,
            '-r', '-o', $systemLinkObject,
            ('@' + $objectList), ('@' + $abiLinkObjectList),
            ('@' + $systemObjectList), ('@' + $exportsFile)
        )
        $previousErrorActionPreference = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            $systemLinkDiagnostics = @(& $linker @systemLinkArgs 2>&1 | ForEach-Object { $_.ToString() })
            $systemLinkExitCode = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $previousErrorActionPreference
        }
        foreach ($line in $systemLinkDiagnostics) { $systemLinkLogLines.Add($line) }
        if ($systemLinkExitCode -eq 0 -and (Test-Path -LiteralPath $systemLinkObject -PathType Leaf)) {
            $objcopyArgs = @('--localize-symbol=memset', '--localize-symbol=memcpy', '--remove-section=.note.gnu.property', $systemLinkObject)
            $previousErrorActionPreference = $ErrorActionPreference
            $ErrorActionPreference = 'Continue'
            try {
                $objcopyDiagnostics = @(& $objcopy @objcopyArgs 2>&1 | ForEach-Object { $_.ToString() })
                $objcopyExitCode = $LASTEXITCODE
            } finally {
                $ErrorActionPreference = $previousErrorActionPreference
            }
            foreach ($line in $objcopyDiagnostics) { $systemLinkLogLines.Add($line) }
            if ($objcopyExitCode -ne 0) { $systemLinkExitCode = $objcopyExitCode }
        }
    }
}
$systemLinkLogLines.Add(('status={0};exit={1};object={2}' -f $(if ($systemLinkExitCode -eq 0) { 'linked' } else { 'link_failed' }), $systemLinkExitCode, $systemLinkObject))
Write-AsciiLines $systemLinkLog $systemLinkLogLines

$undefined = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::Ordinal)
$defined = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::Ordinal)
$nmLogLines = New-Object 'System.Collections.Generic.List[string]'
foreach ($record in $compiledRecords) {
    $nmLines = @(& $nm -P -- $record.Object 2>&1 | ForEach-Object { $_.ToString() })
    if ($LASTEXITCODE -ne 0) { $record.NmStatus = 'nm_failed'; continue }
    $record.NmStatus = 'ok'
    foreach ($nmLine in $nmLines) {
        $fields = $nmLine.Trim() -split '\s+'
        if ($fields.Count -lt 2) { continue }
        $symbolName = $fields[0]
        $symbolType = $fields[1]
        if ($symbolType -eq 'U' -or $symbolType -eq 'w' -or $symbolType -eq 'v') {
            [void]$undefined.Add($symbolName)
        } elseif ($symbolType -ne '?') {
            [void]$defined.Add($symbolName)
        }
    }
}

# 最终的 RM 与 System 链接对象是 Gate 的输入。可重定位链接会保留未解析的
# 重定位，因此可以检查完整依赖图。
$linkedUndefined = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::Ordinal)
$linkedNmExitCode = 1
if (Test-Path -LiteralPath $systemLinkObject -PathType Leaf) {
    $linkedNmLines = @(& $nm -u -P -- $systemLinkObject 2>&1 | ForEach-Object { $_.ToString() })
    $linkedNmExitCode = $LASTEXITCODE
    foreach ($nmLine in $linkedNmLines) {
        $fields = $nmLine.Trim() -split '\s+'
        if ($fields.Count -ge 1 -and -not [string]::IsNullOrWhiteSpace($fields[0])) {
            [void]$linkedUndefined.Add($fields[0])
        }
    }
}
# 如果最终对象不可用或 nm 执行失败，则保留逐对象诊断作为保守回退。
# 成功执行且结果为空的 `nm -u` 才表示未解析符号数为零；不得用原始供应商
# 符号清单替代该结果。
if (-not (Test-Path -LiteralPath $systemLinkObject -PathType Leaf) -or $linkedNmExitCode -ne 0) {
    foreach ($symbol in $undefined) {
        if (-not $defined.Contains($symbol)) { [void]$linkedUndefined.Add($symbol) }
    }
}

$unresolvedLines = New-Object 'System.Collections.Generic.List[string]'
$unresolvedLines.Add('# System Phase 3 RM vendor unresolved symbol inventory after ABI closure')
$unresolvedLines.Add(('# rm_compiled={0};abi_compiled={1};abi_linkable={2};system_objects={3};rm_link_exit={4};system_link_exit={5};linked_nm_exit={6};linked_unresolved={7}' -f $compiledRecords.Count, $abiCompiledRecords.Count, $abiLinkRecords.Count, $systemConvertedRecords.Count, $linkExitCode, $systemLinkExitCode, $linkedNmExitCode, $linkedUndefined.Count))
$unresolvedLines.Add('# The final input is the build-only RM-plus-System relocatable link.')
$unresolvedLines.Add(('[unresolved_external_symbols]'))
$unresolvedLines.Add(('# symbol'))
foreach ($symbol in @($linkedUndefined | Sort-Object)) { $unresolvedLines.Add($symbol) }
Write-AsciiLines $unresolvedReport $unresolvedLines

Write-Output ('SOURCE_REPORT={0}' -f $sourceReport)
Write-Output ('OBJECT_REPORT={0}' -f $objectReport)
Write-Output ('COMPILE_LOG={0}' -f $compileLog)
Write-Output ('LINK_LOG={0}' -f $linkLog)
Write-Output ('ABI_SOURCE_REPORT={0}' -f $abiSourceReport)
Write-Output ('ABI_OBJECT_REPORT={0}' -f $abiObjectReport)
Write-Output ('ABI_COMPILE_LOG={0}' -f $abiCompileLog)
Write-Output ('OS_OBJECT_REPORT={0}' -f $systemObjectReport)
Write-Output ('OS_LINK_LOG={0}' -f $systemLinkLog)
Write-Output ('UNRESOLVED_REPORT={0}' -f $unresolvedReport)
Write-Output ('SUMMARY selected={0};compiled={1};compile_failed={2};abi_selected={3};abi_compiled={4};abi_linkable={5};abi_compile_failed={6};system_objects={7};rm_link_exit={8};system_link_exit={9};linked_unresolved={10}' -f $records.Count, $compiledRecords.Count, @($records | Where-Object Status -eq 'compile_failed').Count, $abiRecords.Count, $abiCompiledRecords.Count, $abiLinkRecords.Count, $abiCompileFailures, $systemConvertedRecords.Count, $linkExitCode, $systemLinkExitCode, $linkedUndefined.Count)

if (@($records | Where-Object Status -eq 'compile_failed').Count -ne 0) { exit 2 }
if ($linkExitCode -ne 0) { exit 3 }
if ($abiCompileFailures -ne 0) { exit 4 }
if ($systemLinkExitCode -ne 0) { exit 5 }
