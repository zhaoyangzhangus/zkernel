# 使用应用私有的 glibc 加载器重建历史独立诊断程序。
[CmdletBinding()]
param(
    [string]$Build = 'build-phase7',
    [string]$GlibcRoot = '',
    [string]$Compiler = 'x86_64-elf-gcc'
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$buildRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot $Build))
if ([string]::IsNullOrWhiteSpace($GlibcRoot)) {
    $GlibcRoot = Join-Path $buildRoot 'host-deps\glibc'
}
$glibc = (Resolve-Path $GlibcRoot).Path
$dev = Join-Path $glibc 'libc6-dev-extract\usr'
$run = Join-Path $glibc 'libc6-extract\lib\x86_64-linux-gnu'
$devLib = Join-Path $dev 'lib\x86_64-linux-gnu'
$output = Join-Path $buildRoot 'phase7-glibc\p7-glibc'
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $output) | Out-Null
$arguments = @(
    '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
    '-D_GNU_SOURCE', '-D__linux__', '-D__x86_64__',
    '-isystem', (Join-Path $dev 'include'),
    '-isystem', (Join-Path $dev 'include\x86_64-linux-gnu'),
    '-nostdlib', '-nodefaultlibs', '-nostartfiles',
    '-Wl,--build-id=none', '-Wl,--allow-shlib-undefined', '-Wl,-z,now',
    '-Wl,-dynamic-linker,/usr/apps/p7-glibc/lib/ld-linux-x86-64.so.2',
    '-Wl,-rpath,/usr/apps/p7-glibc/lib',
    ('-Wl,-rpath-link,{0}' -f $run.Replace('\', '/')),
    (Join-Path $devLib 'crt1.o'), (Join-Path $devLib 'crti.o'),
    (Join-Path $PSScriptRoot 'phase7_glibc_test.c'),
    (Join-Path $run 'libc.so.6'), (Join-Path $devLib 'libc_nonshared.a'),
    (Join-Path $devLib 'crtn.o'), '-o', $output)
$arguments = @($arguments | ForEach-Object { $_.Replace('\', '/') })
& $Compiler @arguments
if ($LASTEXITCODE -ne 0) { throw 'Phase 7 standalone probe build failed' }
& (Join-Path $PSScriptRoot 'phase7_stage.ps1') -Build $Build -GlibcRoot $glibc -TestBinary $output
Write-Output 'PHASE7_HOST_BUILD=PASS'
