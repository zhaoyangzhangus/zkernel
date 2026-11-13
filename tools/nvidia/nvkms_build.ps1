[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# 此构建有意与常规内核 makefile 分开。上游 NVKMS 源文件清单是模式设置实现的权威来源；
# 此脚本只使用 System ELF 工具链编译这些源文件。Linux nvidia-drm/kbuild 包装器不属于此目标。
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$vendorRoot = Join-Path $repoRoot 'third_party\nvidia\595.84'
$vendorSourceRoot = Join-Path $vendorRoot 'open-gpu-kernel-modules'
$sourceListPath = Join-Path $vendorSourceRoot 'src\nvidia-modeset\srcs.mk'
$outputRoot = Join-Path $repoRoot 'build\nvidia\modeset-target'
$objectRoot = Join-Path $outputRoot 'objects'
$sourceReportPath = Join-Path $outputRoot 'sources.tsv'
$objectReportPath = Join-Path $outputRoot 'objects.list'
$statusReportPath = Join-Path $outputRoot 'compile.status'
$compileLogPath = Join-Path $outputRoot 'compile.log'
$sourceHashPath = Join-Path $outputRoot 'source-list.sha256'

function Write-AsciiLines([string]$Path, [System.Collections.IEnumerable]$Lines) {
    $contents = if ($null -eq $Lines) { [string[]]@() } else { [string[]]@($Lines) }
    [System.IO.File]::WriteAllLines($Path, $contents,
                                    [System.Text.Encoding]::ASCII)
}

function Get-RelativeVendorPath([string]$FullPath) {
    $root = [System.IO.Path]::GetFullPath($vendorSourceRoot)
    $path = [System.IO.Path]::GetFullPath($FullPath)
    $prefix = $root + [System.IO.Path]::DirectorySeparatorChar
    if (-not $path.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "path escaped NVIDIA vendor source root: $FullPath"
    }
    return $path.Substring($prefix.Length).Replace('\', '/')
}

function Get-Sha256Text([string]$Text) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($Text)
        return ([System.BitConverter]::ToString($sha.ComputeHash($bytes))).Replace('-', '').ToLowerInvariant()
    } finally {
        $sha.Dispose()
    }
}

if (-not (Test-Path -LiteralPath $sourceListPath -PathType Leaf)) {
    throw "official NVKMS source list is missing: $sourceListPath"
}
if (-not (Test-Path -LiteralPath (Join-Path $vendorRoot 'VENDOR_VERSION') -PathType Leaf) -or
    -not (Test-Path -LiteralPath (Join-Path $vendorRoot 'VENDOR_SHA256SUMS') -PathType Leaf)) {
    throw '595.84 vendor provenance files are missing'
}

$versionText = Get-Content -LiteralPath (Join-Path $vendorRoot 'VENDOR_VERSION') -Raw
foreach ($required in @('version=595.84', 'tag=595.84',
                         'semantic_vendor_patches=0')) {
    if ($versionText -notmatch [regex]::Escape($required)) {
        throw "VENDOR_VERSION missing: $required"
    }
}

$gcc = (Get-Command x86_64-elf-gcc -ErrorAction Stop).Source
$gxx = (Get-Command x86_64-elf-g++ -ErrorAction Stop).Source

# Windows ELF 工具链也会在顶层 bin 目录中提供带前缀的启动器名称。
# 在此宿主机上，Windows 应用控制规则可能会拒绝这些启动器路径，即使目标工具实际存在。
# 优先使用目标 bin 目录中的 binutils，并告诉 GCC 汇编器的位置；这样可以保持构建确定性，
# 同时不更改任何供应商输入。
$toolchainBin = Split-Path -Parent $gcc
$toolchainRoot = Split-Path -Parent $toolchainBin
$targetToolchainBin = Join-Path $toolchainRoot 'x86_64-elf\bin'
if (Test-Path -LiteralPath $targetToolchainBin -PathType Container) {
    $env:COMPILER_PATH = $targetToolchainBin
}
$targetLd = Join-Path $targetToolchainBin 'ld.exe'
$targetNm = Join-Path $targetToolchainBin 'nm.exe'
$targetObjcopy = Join-Path $targetToolchainBin 'objcopy.exe'
$ld = if (Test-Path -LiteralPath $targetLd -PathType Leaf) {
    $targetLd
} else {
    (Get-Command x86_64-elf-ld -ErrorAction Stop).Source
}
$xz = Join-Path ${env:ProgramFiles} 'Git\mingw64\bin\xz.exe'
if (-not (Test-Path -LiteralPath $xz -PathType Leaf)) {
    $xz = (Get-Command xz -ErrorAction Stop).Source
}
$nmCommand = if (Test-Path -LiteralPath $targetNm -PathType Leaf) {
    Get-Command $targetNm -ErrorAction Stop
} else {
    Get-Command x86_64-elf-nm -ErrorAction Stop
}
$objcopy = if (Test-Path -LiteralPath $targetObjcopy -PathType Leaf) {
    $targetObjcopy
} else {
    (Get-Command x86_64-elf-objcopy -ErrorAction Stop).Source
}
$gccVersion = (& $gcc --version 2>&1 | Select-Object -First 1).ToString()
$gxxVersion = (& $gxx --version 2>&1 | Select-Object -First 1).ToString()
# 捆绑的 xz 支持上游 Makefile 使用的压缩选项，但没有版本查询选项；因此将可执行文件路径
# 纳入签名。
$xzVersion = 'w64devkit-xz'

New-Item -ItemType Directory -Path $objectRoot -Force | Out-Null

# 让此包含路径集合与官方 Phase 1 源清单保持同步。所有路径都指向只读供应商输入；生成的
# 头文件仅限于下方目标本地的版本头文件。
$includeRelative = @(
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
    'src\nvidia-modeset\src\shaders',
    'src\nvidia\inc',
    'src\nvidia\inc\os',
    'src\nvidia\kernel\inc',
    'src\nvidia\interface',
    'src\nvidia\inc\kernel'
)

$includeArgs = New-Object 'System.Collections.Generic.List[string]'
[void]$includeArgs.Add('-I' + (Join-Path $repoRoot 'user\libc\include'))
[void]$includeArgs.Add('-I' + (Join-Path $repoRoot 'include'))
$missingIncludeRelative = New-Object 'System.Collections.Generic.List[string]'
foreach ($relative in $includeRelative) {
    $full = Join-Path $vendorSourceRoot $relative
    if (-not (Test-Path -LiteralPath $full -PathType Container)) {
        # 上游 Linux 构建只会为此导入源码快照中不存在的配置创建少量可选/生成包含目录。
        # 此处会记录这些目录，但绝不伪造它们。
        [void]$missingIncludeRelative.Add($relative)
        continue
    }
    [void]$includeArgs.Add('-I' + $full)
}

$defines = @(
    '-D__linux__', '-D__unix__', '-DNV_LINUX', '-DNV_UNIX',
    '-DNV_X86_64', '-DNV_ARCH_BITS=64', '-D_LANGUAGE_C', '-D__NO_CTYPE',
    '-DNV_CPU_INTRINSICS_KERNEL', '-DNVHDMIPKT_RM_CALLS_INTERNAL=0',
    '-DNVHDMIPKT_NVKMS', '-DSOFTFLOAT_ROUND_ODD',
    '-DSOFTFLOAT_FAST_DIV32TO16', '-DSOFTFLOAT_FAST_DIV64TO32',
    '-DNVT_USE_NVKMS', '-DNV_SMG_IN_NVKMS', '-DNV_PUSH_IN_KERNEL',
    '-DNV_XZ_CUSTOM_MEM_HOOKS', '-DNV_XZ_USE_NVTYPES', '-DXZ_DEC_SINGLE',
    # System libc 提供供应商头文件所需的定宽类型；这些别名提供快速宽度名称，不修改 libc
    # 或任何 NVIDIA 源文件。
    '-Duint_least8_t=uint8_t', '-Duint_least16_t=uint16_t',
    '-Duint_least32_t=uint32_t', '-Duint_least64_t=uint64_t',
    '-Dint_least8_t=int8_t', '-Dint_least16_t=int16_t',
    '-Dint_least32_t=int32_t', '-Dint_least64_t=int64_t',
    '-Duint_fast8_t=uint8_t', '-Duint_fast16_t=uint16_t',
    '-Duint_fast32_t=uint32_t', '-Duint_fast64_t=uint64_t',
    '-Dint_fast8_t=int8_t', '-Dint_fast16_t=int16_t',
    '-Dint_fast32_t=int32_t', '-Dint_fast64_t=int64_t', '-DNULL=0'
)
$commonFlags = @(
    '-O2', '-ffreestanding', '-fno-builtin', '-fno-stack-protector',
    '-fno-stack-check', '-fno-asynchronous-unwind-tables',
    '-fno-unwind-tables', '-mno-red-zone', '-mno-mmx', '-mabi=sysv',
    '-fshort-wchar', '-fno-pic', '-mcmodel=kernel', '-ffunction-sections',
    '-fdata-sections', '-Wall', '-Wextra', '-Wno-unused-parameter',
    '-Wno-sign-compare', '-Wno-missing-field-initializers',
    '-Wno-format-zero-length', '-Wno-unused-local-typedefs',
    '-Wno-missing-declarations'
)
$cFlags = @($includeArgs + $defines + $commonFlags +
           @('-Wimplicit', '-Wstrict-prototypes', '-Wno-missing-prototypes',
             '-std=gnu11'))
$cxxFlags = @($includeArgs + $defines + $commonFlags +
              @('-fno-operator-names', '-fno-rtti', '-fno-exceptions',
                '-fcheck-new', '-std=gnu++11'))

$records = New-Object 'System.Collections.Generic.List[object]'
$index = 0
$listDirectory = Split-Path -Parent $sourceListPath
foreach ($line in @(Get-Content -LiteralPath $sourceListPath)) {
    $match = [regex]::Match($line, '^\s*(SRCS|SRCS_CXX)\s*\+=\s+(.+?)\s*$')
    if (-not $match.Success) { continue }
    $kind = $match.Groups[1].Value
    $entry = $match.Groups[2].Value.Trim()
    if ($entry.Contains('$(')) { throw "unexpanded NVKMS source-list entry: $entry" }
    $fullSource = [System.IO.Path]::GetFullPath(
        (Join-Path $listDirectory ($entry -replace '/', '\')))
    if (-not (Test-Path -LiteralPath $fullSource -PathType Leaf)) {
        throw "NVKMS source-list file is missing: $entry"
    }
    $extension = [System.IO.Path]::GetExtension($fullSource).ToLowerInvariant()
    if (($kind -eq 'SRCS' -and $extension -ne '.c') -or
        ($kind -eq 'SRCS_CXX' -and $extension -ne '.cpp')) {
        throw "NVKMS source language mismatch: $entry"
    }
    $index++
    $id = 'nvkms-{0:D4}' -f $index
    $object = Join-Path $objectRoot ('{0:D4}.o' -f $index)
    $signature = $object + '.sig'
    $language = if ($kind -eq 'SRCS_CXX') { 'cxx' } else { 'c' }
    $records.Add([PSCustomObject]@{
        Id = $id
        Language = $language
        Source = Get-RelativeVendorPath $fullSource
        SourceFullPath = $fullSource
        Object = $object
        Signature = $signature
        Status = 'planned'
        Action = 'planned'
        ExitCode = ''
        DiagnosticCount = 0
    })
}
if ($records.Count -ne 230) {
    throw "official NVKMS source count changed unexpectedly: $($records.Count) (expected 230)"
}

$sourceReport = New-Object 'System.Collections.Generic.List[string]'
$sourceReport.Add('# id\tlanguage\tsource\tobject')
foreach ($record in $records) {
    $sourceReport.Add(('{0}`t{1}`t{2}`t{3}' -f $record.Id, $record.Language,
                       $record.Source, $record.Object))
}
Write-AsciiLines $sourceReportPath $sourceReport
$sourceListHash = (Get-FileHash -LiteralPath $sourceListPath -Algorithm SHA256).Hash.ToLowerInvariant()
Write-AsciiLines $sourceHashPath @($sourceListHash)

$compileLog = New-Object 'System.Collections.Generic.List[string]'
$compileLog.Add('# System official NVIDIA 595.84 NVKMS target compilation')
$compileLog.Add('# vendor source: src/nvidia-modeset/srcs.mk')
$compileLog.Add('# kernel-open Linux wrapper: excluded')
$compileLog.Add(('# gcc={0}' -f $gccVersion))
$compileLog.Add(('# gxx={0}' -f $gxxVersion))
foreach ($missingInclude in $missingIncludeRelative) {
    $compileLog.Add(('# missing optional/upstream include directory: {0}' -f
                     $missingInclude))
}

$allCompiled = $true
for ($recordIndex = 0; $recordIndex -lt $records.Count; ++$recordIndex) {
    $record = $records[$recordIndex]
    $compiler = if ($record.Language -eq 'c') { $gcc } else { $gxx }
    $flags = if ($record.Language -eq 'c') { $cFlags } else { $cxxFlags }
    $signatureText = @($record.Source, $sourceListHash, $compiler,
                       ($flags -join "`n")) -join "`n"
    $signatureValue = Get-Sha256Text $signatureText
    $needsCompile = -not (Test-Path -LiteralPath $record.Object -PathType Leaf) -or
                    -not (Test-Path -LiteralPath $record.Signature -PathType Leaf) -or
                    ((Get-Content -LiteralPath $record.Signature -Raw).Trim() -ne $signatureValue)
    if (-not $needsCompile) {
        $record.Status = 'compiled'
        $record.Action = 'skipped'
        $record.ExitCode = 0
        $compileLog.Add(('--- {0} skipped ---' -f $record.Id))
    } else {
        New-Item -ItemType Directory -Path (Split-Path -Parent $record.Object) -Force | Out-Null
        $args = @($flags + @('-c', $record.SourceFullPath, '-o', $record.Object))
        $compileLog.Add(('--- {0} compile {1} ---' -f $record.Id, $record.Source))
        $oldPreference = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            $diagnostics = @(& $compiler @args 2>&1 | ForEach-Object { $_.ToString() })
            $exitCode = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $oldPreference
        }
        foreach ($diagnostic in $diagnostics) { $compileLog.Add($diagnostic) }
        $record.ExitCode = $exitCode
        $record.DiagnosticCount = $diagnostics.Count
        if ($exitCode -eq 0 -and (Test-Path -LiteralPath $record.Object -PathType Leaf)) {
            [System.IO.File]::WriteAllText($record.Signature, $signatureValue,
                                           [System.Text.Encoding]::ASCII)
            $record.Status = 'compiled'
            $record.Action = 'compiled'
        } else {
            $record.Status = 'compile_failed'
            $record.Action = 'failed'
            $allCompiled = $false
            if (Test-Path -LiteralPath $record.Object -PathType Leaf) {
                Remove-Item -LiteralPath $record.Object -Force
            }
        }
        $compileLog.Add(('status={0};exit={1};diagnostics={2}' -f
                         $record.Status, $record.ExitCode, $record.DiagnosticCount))
    }
    if (($recordIndex + 1) % 25 -eq 0 -or ($recordIndex + 1) -eq $records.Count) {
        $compiledCount = @($records | Where-Object Status -eq 'compiled').Count
        $failedCount = @($records | Where-Object Status -eq 'compile_failed').Count
        Write-Output ('PROGRESS {0}/{1}: compiled={2}, failed={3}' -f
                      ($recordIndex + 1), $records.Count, $compiledCount, $failedCount)
    }
}
Write-AsciiLines $compileLogPath $compileLog

$statusLines = New-Object 'System.Collections.Generic.List[string]'
$statusLines.Add('# id\taction\tstatus\texit\tdiagnostics\tsource')
foreach ($record in $records) {
    $statusLines.Add(('{0}`t{1}`t{2}`t{3}`t{4}`t{5}' -f
                       $record.Id, $record.Action, $record.Status,
                       $record.ExitCode, $record.DiagnosticCount, $record.Source))
}
Write-AsciiLines $statusReportPath $statusLines

if (-not $allCompiled) {
    throw 'official NVKMS target compilation failed; no target Gate may pass'
}

$objectLines = New-Object 'System.Collections.Generic.List[string]'
foreach ($record in $records) { $objectLines.Add($record.Object) }

# 官方 Makefile 会压缩并嵌入五种架构的着色器数据块。
# 它们是供应商输入，只生成到 build/ 中；若省略它们，真实 NVKMS 核心会有固件/着色器数据
# 符号无法解析。
function Quote-ProcessArgument([string]$Value) {
    return '"' + $Value.Replace('"', '\"') + '"'
}

function Invoke-BinaryStdout([string]$FileName, [string]$Arguments,
                             [string]$OutputPath) {
    $startInfo = New-Object System.Diagnostics.ProcessStartInfo
    $startInfo.FileName = $FileName
    $startInfo.Arguments = $Arguments
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $startInfo
    if (-not $process.Start()) { throw "failed to start $FileName" }
    $stream = [System.IO.File]::Open($OutputPath,
        [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write,
        [System.IO.FileShare]::Read)
    try {
        $process.StandardOutput.BaseStream.CopyTo($stream)
    } finally {
        $stream.Dispose()
    }
    $stderr = $process.StandardError.ReadToEnd()
    $process.WaitForExit()
    if ($process.ExitCode -ne 0) {
        throw "${FileName} failed: $stderr"
    }
}

$shaderObjects = New-Object 'System.Collections.Generic.List[string]'
$shaderNames = @('turing', 'ampere', 'hopper', 'blackwell', 'gb20x')
foreach ($shaderName in $shaderNames) {
    $shaderInput = Join-Path $vendorSourceRoot ('src\nvidia-modeset\src\shaders\g_{0}_shaders' -f $shaderName)
    $shaderCompressed = Join-Path $outputRoot ('{0}_shaders.xz' -f $shaderName)
    $shaderObject = $shaderCompressed + '.o'
    $shaderSignature = $shaderObject + '.sig'
    $shaderSignatureText = @(
        $shaderName,
        (Get-FileHash -LiteralPath $shaderInput -Algorithm SHA256).Hash,
        $xz, $xzVersion, '-ce -C none', $ld, $objcopy, 'embed-basename-v1'
    ) -join "`n"
    $shaderSignatureValue = Get-Sha256Text $shaderSignatureText
    $shaderNeedsBuild = -not (Test-Path -LiteralPath $shaderObject -PathType Leaf) -or
                        -not (Test-Path -LiteralPath $shaderSignature -PathType Leaf) -or
                        ((Get-Content -LiteralPath $shaderSignature -Raw).Trim() -ne
                         $shaderSignatureValue)
    if ($shaderNeedsBuild) {
        $xzArguments = '-c -e -C none {0}' -f (Quote-ProcessArgument $shaderInput)
        Invoke-BinaryStdout $xz $xzArguments $shaderCompressed
        $compressedName = Split-Path -Leaf $shaderCompressed
        $objectName = Split-Path -Leaf $shaderObject
        Push-Location $outputRoot
        try {
            $ldArguments = @('-r', '-z', 'noexecstack', '--format=binary',
                             $compressedName, '-o', $objectName)
            & $ld @ldArguments
        } finally {
            Pop-Location
        }
        if ($LASTEXITCODE -ne 0) { throw "ld failed for $shaderCompressed" }
        $objcopyArguments = @('--rename-section',
                              '.data=.rodata,contents,alloc,load,data,readonly',
                              $shaderObject)
        & $objcopy @objcopyArguments
        if ($LASTEXITCODE -ne 0) { throw "objcopy failed for $shaderObject" }
        [System.IO.File]::WriteAllText($shaderSignature, $shaderSignatureValue,
                                       [System.Text.Encoding]::ASCII)
        $shaderAction = 'compiled'
    } else {
        $shaderAction = 'skipped'
    }
    $shaderObjects.Add($shaderObject)
    $compileLog.Add(('shader={0};action={1};object={2}' -f
                     $shaderName, $shaderAction, $shaderObject))
}
foreach ($shaderObject in $shaderObjects) { $objectLines.Add($shaderObject) }

# 上游 Makefile 还会根据最终对象集生成 NVKMS 身份字符串。在目标本地翻译单元中保留相同的
# 导出符号名；该字符串是元数据，不是就绪标记。
$identitySource = Join-Path $outputRoot 'nvkms-id.c'
$identityObject = Join-Path $outputRoot 'nvkms-id.o'
$identitySignature = $identityObject + '.sig'
$identityText = @(
    'nvidia id: UNIX Open Kernel Mode Setting Driver',
    $sourceListHash, $gcc, ($cFlags -join "`n")
) -join "`n"
$identityValue = Get-Sha256Text $identityText
$identitySourceText = @(
    'const char NV_KMS_ID[] = "nvidia id: UNIX Open Kernel Mode Setting Driver";',
    'const char *const pNV_KMS_ID = NV_KMS_ID + 11;'
)
if (-not (Test-Path -LiteralPath $identitySource -PathType Leaf) -or
    ((Get-Content -LiteralPath $identitySource -Raw).Trim() -ne
     ($identitySourceText -join "`r`n").Trim())) {
    Write-AsciiLines $identitySource $identitySourceText
}
$identityNeedsBuild = -not (Test-Path -LiteralPath $identityObject -PathType Leaf) -or
                      -not (Test-Path -LiteralPath $identitySignature -PathType Leaf) -or
                      ((Get-Content -LiteralPath $identitySignature -Raw).Trim() -ne
                       $identityValue)
if ($identityNeedsBuild) {
    & $gcc @($cFlags + @('-c', $identitySource, '-o', $identityObject))
    if ($LASTEXITCODE -ne 0) { throw 'gcc failed for generated NVKMS identity source' }
    [System.IO.File]::WriteAllText($identitySignature, $identityValue,
                                   [System.Text.Encoding]::ASCII)
    $identityAction = 'compiled'
} else {
    $identityAction = 'skipped'
}
$objectLines.Add($identityObject)
$compileLog.Add(('identity=NV_KMS_ID;action={0};object={1}' -f
                 $identityAction, $identityObject))

# 官方核心有意停留在 OS 接口边界。将 System 实现编译为目标本地对象并纳入可重定位的 NVKMS
# 镜像；这样可保持供应商源文件不可修改，同时让每个 nvkms_* 依赖都出现在同一份未定义符号报告中。
$systemGlueSource = Join-Path $repoRoot 'kernel\drivers\display\nvidia\nv_nvkms_os.c'
$systemGlueObject = Join-Path $outputRoot 'system\nv_nvkms_os.o'
$systemGlueSignature = $systemGlueObject + '.sig'
if (-not (Test-Path -LiteralPath $systemGlueSource -PathType Leaf)) {
    throw "System NVKMS OS glue is missing: $systemGlueSource"
}
New-Item -ItemType Directory -Path (Split-Path -Parent $systemGlueObject) -Force |
    Out-Null
$systemFlags = New-Object 'System.Collections.Generic.List[string]'
foreach ($flag in $cFlags) { [void]$systemFlags.Add([string]$flag) }
[void]$systemFlags.Add(('-I{0}' -f (Join-Path $repoRoot 'kernel\drivers\gpu\nvidia\port\include')))
[void]$systemFlags.Add(('-I{0}' -f (Join-Path $repoRoot 'kernel\drivers\gpu\nvidia\rm\include')))
[void]$systemFlags.Add('-DOS_KERNEL_BUILD')
[void]$systemFlags.Add('-Werror')
# 目标使用现有的独立运行 libc 头文件。其中 stdatomic 接口提供 atomic_ullong，但没有内核
# VM 头文件使用的可选快速宽度别名；仅在本次编译中于本地绑定该别名。
[void]$systemFlags.Add('-Datomic_uint_fast64_t=atomic_ullong')
$compileLog.Add(('system_os_glue_flags={0}' -f ($systemFlags -join ' ')))
$systemSignatureText = @(
    'system-nvkms-os-glue-v1',
    (Get-FileHash -LiteralPath $systemGlueSource -Algorithm SHA256).Hash,
    $gcc, ($systemFlags -join "`n")
) -join "`n"
$systemSignatureValue = Get-Sha256Text $systemSignatureText
$systemNeedsBuild = -not (Test-Path -LiteralPath $systemGlueObject -PathType Leaf) -or
                    -not (Test-Path -LiteralPath $systemGlueSignature -PathType Leaf) -or
                    ((Get-Content -LiteralPath $systemGlueSignature -Raw).Trim() -ne
                     $systemSignatureValue)
if ($systemNeedsBuild) {
    $oldPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $systemCompileArgs = @($systemFlags.ToArray()) + @(
            '-c', $systemGlueSource, '-o', $systemGlueObject)
        $systemDiagnostics = @(& $gcc @systemCompileArgs 2>&1 |
            ForEach-Object { $_.ToString() })
        $systemExitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $oldPreference
    }
    foreach ($diagnostic in $systemDiagnostics) { $compileLog.Add($diagnostic) }
    if ($systemExitCode -ne 0 -or
        -not (Test-Path -LiteralPath $systemGlueObject -PathType Leaf)) {
        Write-AsciiLines $compileLogPath $compileLog
        if (Test-Path -LiteralPath $systemGlueObject -PathType Leaf) {
            Remove-Item -LiteralPath $systemGlueObject -Force
        }
        throw 'System NVKMS OS glue compilation failed'
    }
    [System.IO.File]::WriteAllText($systemGlueSignature, $systemSignatureValue,
                                   [System.Text.Encoding]::ASCII)
    $systemGlueAction = 'compiled'
} else {
    $systemGlueAction = 'skipped'
}
$objectLines.Add($systemGlueObject)
$compileLog.Add(('system_os_glue=nv_nvkms_os;action={0};object={1}' -f
                 $systemGlueAction, $systemGlueObject))
$systemBackendSource = Join-Path $repoRoot 'kernel\drivers\display\nvidia\nv_nvkms_backend.c'
$systemBackendObject = Join-Path $outputRoot 'system\nv_nvkms_backend.o'
$systemBackendSignature = $systemBackendObject + '.sig'
if (-not (Test-Path -LiteralPath $systemBackendSource -PathType Leaf)) {
    throw "System NVKMS display backend is missing: $systemBackendSource"
}
$backendSignatureText = @(
    'system-nvkms-display-backend-v1',
    (Get-FileHash -LiteralPath $systemBackendSource -Algorithm SHA256).Hash,
    $gcc, ($systemFlags -join "`n")
) -join "`n"
$backendSignatureValue = Get-Sha256Text $backendSignatureText
$backendNeedsBuild = -not (Test-Path -LiteralPath $systemBackendObject -PathType Leaf) -or
                     -not (Test-Path -LiteralPath $systemBackendSignature -PathType Leaf) -or
                     ((Get-Content -LiteralPath $systemBackendSignature -Raw).Trim() -ne
                      $backendSignatureValue)
if ($backendNeedsBuild) {
    $oldPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $backendCompileArgs = @($systemFlags.ToArray()) + @(
            '-c', $systemBackendSource, '-o', $systemBackendObject)
        $backendDiagnostics = @(& $gcc @backendCompileArgs 2>&1 |
            ForEach-Object { $_.ToString() })
        $backendExitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $oldPreference
    }
    foreach ($diagnostic in $backendDiagnostics) { $compileLog.Add($diagnostic) }
    if ($backendExitCode -ne 0 -or
        -not (Test-Path -LiteralPath $systemBackendObject -PathType Leaf)) {
        Write-AsciiLines $compileLogPath $compileLog
        if (Test-Path -LiteralPath $systemBackendObject -PathType Leaf) {
            Remove-Item -LiteralPath $systemBackendObject -Force
        }
        throw 'System NVKMS display backend compilation failed'
    }
    [System.IO.File]::WriteAllText($systemBackendSignature,
                                   $backendSignatureValue,
                                   [System.Text.Encoding]::ASCII)
    $systemBackendAction = 'compiled'
} else {
    $systemBackendAction = 'skipped'
}
$objectLines.Add($systemBackendObject)
$compileLog.Add(('system_display_backend=nv_nvkms_backend;action={0};object={1}' -f
                 $systemBackendAction, $systemBackendObject))
$rmBridgeObject = Join-Path $repoRoot 'build\nvidia\phase8\objects\nv_rm_vendor_bridge.o'
$rmBridgeCommand = $rmBridgeObject + '.cmd'
$rmBridgeSource = Join-Path $repoRoot 'kernel\drivers\gpu\nvidia\rm\nv_rm_vendor_bridge.c'
if (-not (Test-Path -LiteralPath $rmBridgeObject -PathType Leaf) -or
    -not (Test-Path -LiteralPath $rmBridgeCommand -PathType Leaf)) {
    throw 'verified Phase 8 RM bridge object is required for the NVKMS link'
}
$rmBridgeCommandLines = @(Get-Content -LiteralPath $rmBridgeCommand)
if ($rmBridgeCommandLines.Count -lt 2 -or
    (Get-FileHash -LiteralPath $rmBridgeSource -Algorithm SHA256).Hash -ne
    $rmBridgeCommandLines[1]) {
    throw 'Phase 8 RM bridge object signature is stale'
}
$objectLines.Add($rmBridgeObject)
$compileLog.Add(('rm_bridge=nv_rm_vendor_bridge;action=verified;object={0}' -f
                 $rmBridgeObject))
$linkObjectLines = @($objectLines | ForEach-Object {
    $_.ToString().Replace('\', '/')
})
Write-AsciiLines $objectReportPath $linkObjectLines

$relocatableObject = Join-Path $outputRoot 'vkms-core.o'
$relocatableArguments = @('-r', '-z', 'noexecstack', '-o',
                          $relocatableObject,
                          ('@' + $objectReportPath.Replace('\', '/')))
& $ld @relocatableArguments
if ($LASTEXITCODE -ne 0 -or
    -not (Test-Path -LiteralPath $relocatableObject -PathType Leaf)) {
    throw 'NVKMS relocatable link failed'
}
$compileLog.Add(('relocatable=vkms-core;object={0}' -f $relocatableObject))

$undefinedSymbols = New-Object 'System.Collections.Generic.HashSet[string]'
foreach ($objectPath in $objectLines) {
    $nmLines = @(& $nmCommand.Source '-u' '-P' '--' $objectPath 2>&1 |
                 ForEach-Object { $_.ToString() })
    if ($LASTEXITCODE -ne 0) { throw "nm failed for $objectPath" }
    foreach ($line in $nmLines) {
        $parts = $line -split '\s+'
        if ($parts.Count -gt 0 -and $parts[0]) { [void]$undefinedSymbols.Add($parts[0]) }
    }
}
$undefinedOutput = @($undefinedSymbols | Sort-Object)
if ($undefinedOutput.Count -eq 0) { $undefinedOutput = @('# none') }
Write-AsciiLines (Join-Path $outputRoot 'undefined-symbols.list') $undefinedOutput

Write-Output ('NVKMS_TARGET_OBJECTS={0}' -f $records.Count)
Write-Output ('NVKMS_TARGET_COMPILED={0}' -f @($records | Where-Object Status -eq 'compiled').Count)
Write-Output ('NVKMS_TARGET_SKIPPED={0}' -f @($records | Where-Object Action -eq 'skipped').Count)
Write-Output ('NVKMS_TARGET_UNDEFINED={0}' -f $undefinedSymbols.Count)
Write-Output 'NVKMS_TARGET_BUILD=PASS'
