[CmdletBinding()]
param(
    [string]$Build = 'build-phase8',
    [string]$GlibcRoot = '',
    [string]$TestBinary = ''
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'app-layout.ps1')
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$buildRoot = (Resolve-Path (Join-Path $projectRoot $Build)).Path
if ([string]::IsNullOrWhiteSpace($GlibcRoot)) {
    $GlibcRoot = Join-Path $projectRoot 'build-phase7\host-deps\glibc'
}
if ([string]::IsNullOrWhiteSpace($TestBinary)) {
    $TestBinary = Join-Path $buildRoot 'phase8-frontend\p8-frontend'
}
$glibcRootPath = (Resolve-Path $GlibcRoot).Path
$testBinaryPath = (Resolve-Path $TestBinary).Path
$runtimeRoot = Join-Path $glibcRootPath 'libc6-extract'
$espRoot = Join-Path $buildRoot 'esp'
Assert-AppGuestLayout $espRoot
Assert-AppInterpreter $testBinaryPath 'p8-frontend'
$testsRoot = Join-Path $espRoot 'tests'
$loaderRoot = Join-Path $espRoot 'usr\apps\p8-frontend\lib'
$libRoot = $loaderRoot

foreach ($required in @(
    $testBinaryPath,
    (Join-Path $runtimeRoot 'lib\x86_64-linux-gnu\ld-linux-x86-64.so.2'),
    (Join-Path $runtimeRoot 'lib\x86_64-linux-gnu\libc.so.6')
)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Missing Phase 8 test input: $required"
    }
}

New-Item -ItemType Directory -Force -Path $testsRoot, $loaderRoot, $libRoot |
    Out-Null
Copy-Item -LiteralPath $testBinaryPath -Destination (Join-Path $testsRoot 'p8-frontend') -Force
Copy-Item -LiteralPath (Join-Path $runtimeRoot 'lib\x86_64-linux-gnu\ld-linux-x86-64.so.2') -Destination (Join-Path $loaderRoot 'ld-linux-x86-64.so.2') -Force
Copy-Item -LiteralPath (Join-Path $runtimeRoot 'lib\x86_64-linux-gnu\libc.so.6') -Destination (Join-Path $libRoot 'libc.so.6') -Force

Copy-Item -LiteralPath $testBinaryPath -Destination (Join-Path $espRoot 'usr\apps\p8-frontend\p8-frontend') -Force

Write-Host 'PHASE8_STAGE=PASS'
Write-Host "PHASE8_BINARY=$testsRoot\p8-frontend"
Write-Host "PHASE8_LOADER=$loaderRoot\ld-linux-x86-64.so.2"
Write-Host "PHASE8_LIBC=$libRoot\libc.so.6"
