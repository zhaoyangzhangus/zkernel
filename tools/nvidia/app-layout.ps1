# 主机缓存保留导入结构，便于仅重命名迁移；部署时每个应用使用平铺的私有依赖。
function Test-LegacyGuestPath([string]$Relative) {
    $path = $Relative.Replace('\', '/').ToLowerInvariant()
    return $path -match '^(compat/|lib64/|lib/x86_64-linux-gnu/|usr/share/glvnd/|etc/glvnd/)'
}

function Install-NvidiaApp([string]$UserspaceRoot, [string]$EspRoot,
                           [string]$AppName) {
    if ($AppName -notmatch '^[a-zA-Z0-9][a-zA-Z0-9_-]*$') {
        throw "Invalid application name: $AppName"
    }
    Assert-AppGuestLayout $EspRoot
    $appRoot = Join-Path $EspRoot ("usr\apps\{0}" -f $AppName)
    $sourceLib = Join-Path $UserspaceRoot 'lib\x86_64-linux-gnu'
    Require-Directory $sourceLib 'imported NVIDIA/glibc library closure'
    foreach ($file in @(Get-ChildItem -LiteralPath $sourceLib -File)) {
        Copy-FileIfChanged $file.FullName (Join-Path $appRoot ('lib\' + $file.Name))
    }
    $license = Join-Path $PSScriptRoot '..\..\build\nvidia\official-595.84\extracted-gitbash\LICENSE'
    Require-File $license 'official NVIDIA license'
    Copy-FileIfChanged $license (Join-Path $appRoot 'licenses\NVIDIA-LICENSE')
    # 导入缓存中的外部平台清单不能作为 EGL 厂商清单；仅发布指向本应用驱动的 NVIDIA 配置。
    $json = '{"file_format_version":"1.0.0","ICD":{"library_path":"/usr/apps/' +
            $AppName + '/lib/libEGL_nvidia.so.0"}}' + "`n"
    $manifest = Join-Path $appRoot 'egl\10_nvidia.json'
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $manifest) | Out-Null
    if ((Test-Path -LiteralPath $manifest -PathType Leaf) -and
        [IO.File]::ReadAllText($manifest) -eq $json) {
        $script:stageSkipped++
    } else {
        [IO.File]::WriteAllText($manifest, $json, [Text.Encoding]::ASCII)
        $script:stageCopied++
    }
}

function Assert-AppGuestLayout([string]$EspRoot) {
    foreach ($relative in @('compat', 'lib64', 'lib\x86_64-linux-gnu',
                            'usr\share\glvnd', 'etc\glvnd')) {
        if (Test-Path -LiteralPath (Join-Path $EspRoot $relative)) {
            throw "Legacy guest layout in $EspRoot ($relative); rebuild in a clean ESP staging directory."
        }
    }
}

function Assert-AppInterpreter([string]$Binary, [string]$AppName) {
    $readelf = Get-Command x86_64-elf-readelf -ErrorAction SilentlyContinue
    if ($null -eq $readelf) { $readelf = Get-Command readelf -ErrorAction Stop }
    $program = @(& $readelf.Source '-lW' '--' $Binary 2>&1)
    if ($LASTEXITCODE -ne 0 -or
        ($program -join "`n") -notmatch [regex]::Escape("/usr/apps/$AppName/lib/ld-linux-x86-64.so.2")) {
        throw "Rebuild $Binary with PT_INTERP=/usr/apps/$AppName/lib/ld-linux-x86-64.so.2"
    }
    $dynamic = @(& $readelf.Source '-dW' '--' $Binary 2>&1)
    if ($LASTEXITCODE -ne 0 -or
        ($dynamic -join "`n") -notmatch ("\((RPATH|RUNPATH)\).*" + [regex]::Escape("[/usr/apps/$AppName/lib]"))) {
        throw "Rebuild $Binary with RPATH=/usr/apps/$AppName/lib"
    }
}
