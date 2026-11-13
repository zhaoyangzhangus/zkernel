[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$vendorRoot = Join-Path $repoRoot 'third_party\nvidia\595.84'
$vendorSourceRoot = Join-Path $vendorRoot 'open-gpu-kernel-modules'
$outputRoot = Join-Path $repoRoot 'build\nvidia'
$sourceReport = Join-Path $outputRoot 'vendor_sources.txt'
$objectReport = Join-Path $outputRoot 'vendor_objects.txt'
$unresolvedReport = Join-Path $outputRoot 'unresolved_symbols.txt'
$compileLog = Join-Path $outputRoot 'compile.log'
$tab = [char]9
$categoryOrder = @('memory', 'DMA', 'PCI', 'MMIO', 'IRQ', 'sync', 'atomic', 'time', 'thread/worker', 'VM/user', 'cache', 'firmware', 'VBIOS', 'registry', 'misc')

function Fail-Gate([string]$Message) {
    throw "P1 Gate failed: $Message"
}

function Get-RelativeVendorPath([string]$FullPath) {
    $vendorRootFull = [System.IO.Path]::GetFullPath($vendorRoot)
    $fullPathNormalized = [System.IO.Path]::GetFullPath($FullPath)
    $prefix = $vendorRootFull + [System.IO.Path]::DirectorySeparatorChar
    if (-not $fullPathNormalized.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        Fail-Gate "path escaped vendor root: $FullPath"
    }
    return $fullPathNormalized.Substring($prefix.Length).Replace('\', '/')
}

function Get-SymbolCategory([string]$SymbolName) {
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

function Read-DataLines([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { Fail-Gate "missing report: $Path" }
    return @(Get-Content -LiteralPath $Path | Where-Object { $_ -and $_[0] -ne '#' })
}

if (-not (Test-Path -LiteralPath $vendorSourceRoot -PathType Container)) { Fail-Gate 'vendor source root is missing' }
foreach ($requiredReport in @($sourceReport, $objectReport, $unresolvedReport, $compileLog)) {
    if (-not (Test-Path -LiteralPath $requiredReport -PathType Leaf)) { Fail-Gate "missing Phase 1 report: $requiredReport" }
}

$versionText = Get-Content -LiteralPath (Join-Path $vendorRoot 'VENDOR_VERSION') -Raw
foreach ($requiredVersionLine in @(
        'version=595.84',
        'tag=595.84',
        'tag_object=3f74968ea1486de6a21698eb22773fcd2f24d35c',
        'commit=722ae84526a09ed672fbe75448e2909834ba4cce',
        'semantic_vendor_patches=0')) {
    if ($versionText -notmatch [regex]::Escape($requiredVersionLine)) {
        Fail-Gate "VENDOR_VERSION missing: $requiredVersionLine"
    }
}

$manifestPath = Join-Path $vendorRoot 'VENDOR_SHA256SUMS'
$manifestEntries = New-Object 'System.Collections.Generic.Dictionary[string,string]' ([System.StringComparer]::OrdinalIgnoreCase)
foreach ($line in @(Get-Content -LiteralPath $manifestPath)) {
    if ([string]::IsNullOrWhiteSpace($line) -or $line.StartsWith('#')) { continue }
    $manifestMatch = [regex]::Match($line, '^([0-9a-fA-F]{64})  (.+)$')
    if (-not $manifestMatch.Success) { Fail-Gate "invalid manifest line: $line" }
    try {
        $manifestEntries.Add($manifestMatch.Groups[2].Value, $manifestMatch.Groups[1].Value.ToLowerInvariant())
    } catch {
        Fail-Gate "duplicate manifest entry: $($manifestMatch.Groups[2].Value)"
    }
}
$vendorFiles = @(Get-ChildItem -LiteralPath $vendorSourceRoot -Recurse -File)
if ($vendorFiles.Count -ne $manifestEntries.Count) { Fail-Gate 'vendor file count does not match manifest' }
foreach ($file in $vendorFiles) {
    $relativePath = Get-RelativeVendorPath $file.FullName
    if (-not $manifestEntries.ContainsKey($relativePath)) { Fail-Gate "unmanifested vendor file: $relativePath" }
    $actualHash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualHash -ne $manifestEntries[$relativePath]) { Fail-Gate "vendor hash mismatch: $relativePath" }
    if (-not $file.IsReadOnly) { Fail-Gate "vendor file is writable: $relativePath" }
}
if (@(Get-ChildItem -LiteralPath $vendorSourceRoot -Recurse -Force -Directory -Filter '.git').Count -ne 0) {
    Fail-Gate 'vendor source contains repository metadata'
}

# 直接根据不可变的上游源文件清单重新计算预期的源码选择，
# 并逐字节与生成的报告进行比较。
$sourceLists = @(
    [PSCustomObject]@{ Component = 'rm'; ListPath = (Join-Path $vendorSourceRoot 'src\nvidia\srcs.mk') },
    [PSCustomObject]@{ Component = 'modeset'; ListPath = (Join-Path $vendorSourceRoot 'src\nvidia-modeset\srcs.mk') }
)
$expectedSources = New-Object 'System.Collections.Generic.List[string]'
foreach ($sourceList in $sourceLists) {
    $listDirectory = Split-Path -Parent $sourceList.ListPath
    $listRelative = Get-RelativeVendorPath $sourceList.ListPath
    $lineNumber = 0
    foreach ($line in @(Get-Content -LiteralPath $sourceList.ListPath)) {
        $lineNumber++
        $sourceMatch = [regex]::Match($line, '^\s*(SRCS|SRCS_CXX)\s*\+=\s+(.+?)\s*$')
        if (-not $sourceMatch.Success) { continue }
        $sourceKind = $sourceMatch.Groups[1].Value
        $sourceEntry = $sourceMatch.Groups[2].Value.Trim()
        if ($sourceEntry.Contains('$(')) { Fail-Gate ('unexpanded source list entry: {0}:{1}' -f $listRelative, $lineNumber) }
        $sourceFullPath = [System.IO.Path]::GetFullPath((Join-Path $listDirectory ($sourceEntry -replace '/', '\')))
        if (-not (Test-Path -LiteralPath $sourceFullPath -PathType Leaf)) { Fail-Gate "missing selected source: $sourceEntry" }
        $extension = [System.IO.Path]::GetExtension($sourceFullPath).ToLowerInvariant()
        if (($sourceKind -eq 'SRCS' -and $extension -ne '.c') -or
            ($sourceKind -eq 'SRCS_CXX' -and $extension -ne '.cpp')) {
            Fail-Gate ('source language mismatch: {0}:{1}' -f $listRelative, $lineNumber)
        }
        $language = if ($sourceKind -eq 'SRCS_CXX') { 'cxx' } else { 'c' }
        $sourceRelative = Get-RelativeVendorPath $sourceFullPath
        $expectedSources.Add(('{0}{4}{1}{4}{2}{4}{3}' -f $sourceList.Component, $language, $sourceRelative, ('{0}:{1}' -f $listRelative, $lineNumber), $tab))
    }
}
$sourceReportLines = @(Get-Content -LiteralPath $sourceReport)
$sourceData = @($sourceReportLines | Where-Object { $_ -and $_[0] -ne '#' })
if ($sourceData.Count -ne $expectedSources.Count) { Fail-Gate "selected source count mismatch" }
for ($i = 0; $i -lt $expectedSources.Count; $i++) {
    if ($sourceData[$i] -ne $expectedSources[$i]) { Fail-Gate "non-deterministic source row at index $i" }
}
$sourceListHash = (Get-FileHash -LiteralPath $sourceReport -Algorithm SHA256).Hash.ToLowerInvariant()

$objectData = Read-DataLines $objectReport
if ($objectData.Count -ne $expectedSources.Count) { Fail-Gate 'object/source row count mismatch' }
$objectRecords = New-Object 'System.Collections.Generic.List[object]'
$seenIds = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::Ordinal)
$seenObjects = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)
$compileCount = 0
$failureCount = 0
for ($i = 0; $i -lt $objectData.Count; $i++) {
    $fields = $objectData[$i] -split "`t", -1
    if ($fields.Count -ne 8) { Fail-Gate "malformed object row at index $i" }
    if ($fields[4] -ne (($expectedSources[$i] -split "`t", -1)[2])) { Fail-Gate "object/source mapping mismatch at index $i" }
    if (-not $seenIds.Add($fields[3])) { Fail-Gate "duplicate object id: $($fields[3])" }
    if (-not $seenObjects.Add($fields[5])) { Fail-Gate "duplicate object path: $($fields[5])" }
    $objectFullPath = Join-Path $outputRoot ($fields[5] -replace '/', '\')
    if ($fields[0] -eq 'compiled') {
        $compileCount++
        if ($fields[6] -ne '0' -or $fields[7] -ne 'ok') { Fail-Gate "compiled row has bad status: $($fields[3])" }
        if (-not (Test-Path -LiteralPath $objectFullPath -PathType Leaf)) { Fail-Gate "compiled object missing: $($fields[3])" }
    } elseif ($fields[0] -eq 'compile_failed') {
        $failureCount++
        if ([int]$fields[6] -eq 0 -or $fields[7] -ne 'not-run') { Fail-Gate "failed row has bad status: $($fields[3])" }
        if (Test-Path -LiteralPath $objectFullPath) { Fail-Gate "failed object unexpectedly exists: $($fields[3])" }
    } else {
        Fail-Gate "unaccounted object status: $($fields[0])"
    }
    $objectRecords.Add([PSCustomObject]@{
        Status = $fields[0]
        Component = $fields[1]
        Language = $fields[2]
        Id = $fields[3]
        Source = $fields[4]
        Object = $fields[5]
        ObjectFullPath = $objectFullPath
        NmStatus = $fields[7]
    })
}
if (($compileCount + $failureCount) -ne $expectedSources.Count) { Fail-Gate 'not every selected source has a terminal object status' }

$objectHeader = @(Get-Content -LiteralPath $objectReport | Where-Object { $_ -like '# source_list_sha256=*' })
if ($objectHeader.Count -ne 1 -or $objectHeader[0] -ne "# source_list_sha256=$sourceListHash") { Fail-Gate 'object report source-list hash mismatch' }

$compileLogLines = @(Get-Content -LiteralPath $compileLog)
foreach ($record in $objectRecords) {
    if (@($compileLogLines | Where-Object { $_ -like "--- $($record.Id) *" }).Count -ne 1) { Fail-Gate "compile log lacks one section for $($record.Id)" }
}

$unresolvedLines = @(Get-Content -LiteralPath $unresolvedReport)
$unresolvedHeader = @($unresolvedLines | Where-Object { $_ -like '# source_list_sha256=*' })
if ($unresolvedHeader.Count -ne 1 -or $unresolvedHeader[0] -ne "# source_list_sha256=$sourceListHash") { Fail-Gate 'unresolved report source-list hash mismatch' }
$summaryLine = @($unresolvedLines | Where-Object { $_ -like '# selected_sources=*' })
if ($summaryLine.Count -ne 1) { Fail-Gate 'unresolved report summary is missing' }
$summaryMatch = [regex]::Match($summaryLine[0], '^# selected_sources=(\d+);compiled_objects=(\d+);compile_failures=(\d+);resolved_partial_symbols=(\d+);unresolved_symbols=(\d+)$')
if (-not $summaryMatch.Success) { Fail-Gate 'malformed unresolved report summary' }
if ([int]$summaryMatch.Groups[1].Value -ne $expectedSources.Count -or
    [int]$summaryMatch.Groups[2].Value -ne $compileCount -or
    [int]$summaryMatch.Groups[3].Value -ne $failureCount) {
    Fail-Gate 'unresolved report compile counts mismatch'
}

$compileFailureStart = [Array]::IndexOf($unresolvedLines, '[compile_failures]')
$unresolvedStart = [Array]::IndexOf($unresolvedLines, '[unresolved_external_symbols]')
$nmFailureStart = [Array]::IndexOf($unresolvedLines, '[nm_failures]')
if ($compileFailureStart -lt 0 -or $unresolvedStart -lt 0 -or $nmFailureStart -lt 0 -or
    $compileFailureStart -ge $unresolvedStart -or $unresolvedStart -ge $nmFailureStart) {
    Fail-Gate 'unresolved report sections are malformed'
}
$categoryStart = [Array]::IndexOf($unresolvedLines, '[category_summary]')
if ($categoryStart -lt 0 -or $categoryStart -ge $compileFailureStart) { Fail-Gate 'category summary is missing or misplaced' }
$reportedCategoryCounts = @{}
for ($i = $categoryStart + 1; $i -lt $compileFailureStart; $i++) {
    if ($unresolvedLines[$i].StartsWith('#') -or [string]::IsNullOrWhiteSpace($unresolvedLines[$i])) { continue }
    $fields = $unresolvedLines[$i] -split "`t", -1
    if ($fields.Count -ne 2 -or $reportedCategoryCounts.ContainsKey($fields[0])) { Fail-Gate 'malformed/duplicate category summary row' }
    if ($fields[0] -notin $categoryOrder -or [int]$fields[1] -lt 0) { Fail-Gate 'invalid category summary row' }
    $reportedCategoryCounts.Add($fields[0], [int]$fields[1])
}
foreach ($category in $categoryOrder) {
    if (-not $reportedCategoryCounts.ContainsKey($category)) { Fail-Gate "category summary missing: $category" }
}
$failedIds = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::Ordinal)
for ($i = $compileFailureStart + 1; $i -lt $unresolvedStart; $i++) {
    if ($unresolvedLines[$i].StartsWith('#') -or [string]::IsNullOrWhiteSpace($unresolvedLines[$i])) { continue }
    $fields = $unresolvedLines[$i] -split "`t", -1
    if ($fields.Count -ne 6 -or -not $failedIds.Add($fields[0])) { Fail-Gate 'malformed/duplicate compile failure row' }
}
if ($failedIds.Count -ne $failureCount) { Fail-Gate 'compile failures are not fully inventoried' }
foreach ($record in @($objectRecords | Where-Object Status -eq 'compile_failed')) {
    if (-not $failedIds.Contains($record.Id)) { Fail-Gate "compile failure missing from unresolved report: $($record.Id)" }
}

$reportedUnresolved = New-Object 'System.Collections.Generic.Dictionary[string,object]' ([System.StringComparer]::Ordinal)
$reportedTypes = New-Object 'System.Collections.Generic.Dictionary[string,string]' ([System.StringComparer]::Ordinal)
$reportedCategories = New-Object 'System.Collections.Generic.Dictionary[string,string]' ([System.StringComparer]::Ordinal)
$compiledIdSet = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::Ordinal)
foreach ($record in @($objectRecords | Where-Object Status -eq 'compiled')) { [void]$compiledIdSet.Add($record.Id) }
for ($i = $unresolvedStart + 1; $i -lt $nmFailureStart; $i++) {
    if ($unresolvedLines[$i].StartsWith('#') -or [string]::IsNullOrWhiteSpace($unresolvedLines[$i])) { continue }
    $fields = $unresolvedLines[$i] -split "`t", -1
    if ($fields.Count -ne 5 -or $reportedUnresolved.ContainsKey($fields[1])) { Fail-Gate 'malformed/duplicate unresolved symbol row' }
    if ($fields[0] -notin $categoryOrder -or (Get-SymbolCategory $fields[1]) -ne $fields[0]) { Fail-Gate "invalid unresolved category: $($fields[1])" }
    $ids = @($fields[3] -split ',')
    if ([int]$fields[2] -ne $ids.Count -or [int]$fields[2] -le 0) { Fail-Gate "bad unresolved reference count: $($fields[1])" }
    $idSet = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::Ordinal)
    foreach ($id in $ids) {
        if (-not $idSet.Add($id) -or -not $compiledIdSet.Contains($id)) { Fail-Gate "unresolved symbol references invalid object: $($fields[1])" }
    }
    if ($fields[4] -notin @('U','w','v')) { Fail-Gate "invalid unresolved symbol type: $($fields[1])" }
    $reportedUnresolved.Add($fields[1], $idSet)
    $reportedTypes.Add($fields[1], $fields[4])
    $reportedCategories.Add($fields[1], $fields[0])
}
$nmFailureRows = @($unresolvedLines | Select-Object -Skip ($nmFailureStart + 1) | Where-Object { $_ -and $_[0] -ne '#' })
if ($nmFailureRows.Count -ne 0) { Fail-Gate 'nm failures are present' }

# 从已编译对象中独立重新计算符号定义和未定义引用，
# 以确认 unresolved_symbols.txt 已完整列出所有项目。
$definitions = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::Ordinal)
$actualUnresolved = New-Object 'System.Collections.Generic.Dictionary[string,object]' ([System.StringComparer]::Ordinal)
$actualTypes = New-Object 'System.Collections.Generic.Dictionary[string,string]' ([System.StringComparer]::Ordinal)
foreach ($record in @($objectRecords | Where-Object Status -eq 'compiled')) {
    $previousErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $nmLines = @(& nm -P -- $record.ObjectFullPath 2>&1 | ForEach-Object { $_.ToString() })
        $nmExitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }
    if ($nmExitCode -ne 0) { Fail-Gate "nm failed during independent scan: $($record.Id)" }
    foreach ($nmLine in $nmLines) {
        $trimmed = $nmLine.Trim()
        if ([string]::IsNullOrWhiteSpace($trimmed)) { continue }
        $fields = $trimmed -split '\s+'
        if ($fields.Count -lt 2) { Fail-Gate "nm output cannot be parsed: $($record.Id)" }
        $symbol = $fields[0]
        $type = $fields[1]
        if ($type -in @('U','w','v')) {
            if (-not $actualUnresolved.ContainsKey($symbol)) {
                $actualUnresolved.Add($symbol, (New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::Ordinal)))
                $actualTypes.Add($symbol, $type)
            }
            [void]$actualUnresolved[$symbol].Add($record.Id)
        } elseif ($type -ne '?') {
            [void]$definitions.Add($symbol)
        }
    }
}
$actualNames = @($actualUnresolved.Keys | Where-Object { -not $definitions.Contains($_) } | Sort-Object)
if ($actualNames.Count -ne $reportedUnresolved.Count) { Fail-Gate 'unresolved symbol count is not exhaustive' }
foreach ($symbol in $actualNames) {
    if (-not $reportedUnresolved.ContainsKey($symbol)) { Fail-Gate "unreported unresolved symbol: $symbol" }
    $actualIds = @($actualUnresolved[$symbol] | Sort-Object)
    $reportedIds = @($reportedUnresolved[$symbol] | Sort-Object)
    if (($actualIds -join ',') -ne ($reportedIds -join ',') -or
        $actualTypes[$symbol] -ne $reportedTypes[$symbol] -or
        (Get-SymbolCategory $symbol) -ne $reportedCategories[$symbol]) {
        Fail-Gate "unresolved symbol references mismatch: $symbol"
    }
}

foreach ($category in $categoryOrder) {
    $actualCategoryCount = @($actualNames | Where-Object { (Get-SymbolCategory $_) -eq $category }).Count
    if ($actualCategoryCount -ne $reportedCategoryCounts[$category]) { Fail-Gate "category count mismatch: $category" }
}
if ([int]$summaryMatch.Groups[5].Value -ne ($reportedCategoryCounts.Values | Measure-Object -Sum).Sum) {
    Fail-Gate 'category summary total does not match unresolved count'
}

Write-Output ('GATE_P1=PASS selected_sources={0} compiled_objects={1} compile_failures={2} unresolved_symbols={3}' -f $expectedSources.Count, $compileCount, $failureCount, $reportedUnresolved.Count)
