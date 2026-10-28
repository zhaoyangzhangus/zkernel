param(
    [string]$Build = 'build-phase7',
    [string]$GlibcRoot = '',
    [string]$TestBinary = ''
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'app-layout.ps1')
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$buildRoot = (Resolve-Path (Join-Path $projectRoot $Build)).Path
if ([string]::IsNullOrWhiteSpace($GlibcRoot)) {
    $GlibcRoot = Join-Path $buildRoot 'host-deps\glibc'
}
if ([string]::IsNullOrWhiteSpace($TestBinary)) {
    $TestBinary = Join-Path $buildRoot 'phase7-glibc\p7-glibc'
}
$glibcRootPath = (Resolve-Path $GlibcRoot).Path
$testBinaryPath = (Resolve-Path $TestBinary).Path
$runtimeRoot = Join-Path $glibcRootPath 'libc6-extract'
$devRoot = Join-Path $glibcRootPath 'libc6-dev-extract'
$espRoot = Join-Path $buildRoot 'esp'
Assert-AppGuestLayout $espRoot
Assert-AppInterpreter $testBinaryPath 'p7-glibc'
$testsRoot = Join-Path $espRoot 'usr\apps\p7-glibc'
$loaderRoot = Join-Path $espRoot 'usr\apps\p7-glibc\lib'
$libRoot = $loaderRoot

foreach ($required in @(
    $testBinaryPath,
    (Join-Path $runtimeRoot 'lib\x86_64-linux-gnu\ld-linux-x86-64.so.2'),
    (Join-Path $runtimeRoot 'lib\x86_64-linux-gnu\libc.so.6'),
    (Join-Path $projectRoot 'tools\nvidia\phase7-data.txt')
)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Missing Phase 7 test input: $required"
    }
}

New-Item -ItemType Directory -Force -Path $testsRoot, $loaderRoot, $libRoot | Out-Null
Copy-Item -LiteralPath $testBinaryPath -Destination (Join-Path $testsRoot 'p7-glibc') -Force
Copy-Item -LiteralPath (Join-Path $projectRoot 'tools\nvidia\phase7-data.txt') -Destination (Join-Path $testsRoot 'p7-data.txt') -Force
Copy-Item -LiteralPath (Join-Path $runtimeRoot 'lib\x86_64-linux-gnu\ld-linux-x86-64.so.2') -Destination (Join-Path $loaderRoot 'ld-linux-x86-64.so.2') -Force
Copy-Item -LiteralPath (Join-Path $runtimeRoot 'lib\x86_64-linux-gnu\libc.so.6') -Destination (Join-Path $libRoot 'libc.so.6') -Force

New-Item -ItemType Directory -Force -Path (Join-Path $espRoot 'tests') | Out-Null
Copy-Item -LiteralPath $testBinaryPath -Destination (Join-Path $espRoot 'tests\p7-glibc') -Force

Write-Host "PHASE7_STAGE=PASS"
Write-Host "PHASE7_BINARY=$testsRoot\p7-glibc"
Write-Host "PHASE7_LOADER=$loaderRoot\ld-linux-x86-64.so.2"
Write-Host "PHASE7_LIBC=$libRoot\libc.so.6"
