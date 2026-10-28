param(
    [string]$Build = 'build-realtest',
    [switch]$FailureTest,
    [switch]$Uas,
    [switch]$InputTest,
    [switch]$FileManagerTest,
    [switch]$SkipBuild,
    [int]$TimeoutSeconds = 300,
    [int]$Cpu = 4,
    [int]$MemoryMiB = 4096,
    [int]$MonitorPort = 45555,
    [string]$QemuPath = '',
    [string]$FirmwarePath = ''
)

$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

if ([IO.Path]::IsPathRooted($Build) -or
    $Build -match '(^|[\\/])\.\.(?:[\\/]|$)') {
    throw "Build directory must be relative to the project: $Build"
}
if ($TimeoutSeconds -lt 1 -or $Cpu -lt 1 -or $MemoryMiB -lt 256) {
    throw 'TimeoutSeconds, Cpu, and MemoryMiB must be positive.'
}
if ($InputTest -and $FailureTest) {
    throw 'InputTest cannot be combined with FailureTest.'
}
if ($InputTest -and $FileManagerTest) {
    throw 'InputTest cannot be combined with FileManagerTest.'
}
if ($FileManagerTest -and ($FailureTest -or $Uas)) {
    throw 'FileManagerTest cannot be combined with FailureTest or Uas.'
}
if (($InputTest -or $FileManagerTest) -and
    ($MonitorPort -lt 1 -or $MonitorPort -gt 65535)) {
    throw "MonitorPort is outside the TCP port range: $MonitorPort"
}

$buildDirectory = Join-Path $projectRoot $Build
$espPath = Join-Path $buildDirectory 'esp'
$mode = if ($FailureTest) {
    'failure'
} elseif ($FileManagerTest) {
    'success-fileman'
} elseif ($InputTest) {
    'success-input'
} else {
    'success'
}
$expectedMarker = if ($FailureTest) { 'FAIL' } else { 'PASS' }
$imagePath = Join-Path $buildDirectory ("qemu-realtest-$mode.img")
$legacyBootImagePath = Join-Path $buildDirectory ("qemu-realtest-$mode-boot.img")
$serialLog = Join-Path $buildDirectory ("qemu-realtest-$mode-serial.log")
$qemuLog = Join-Path $buildDirectory ("qemu-realtest-$mode.log")
$qemuErrorLog = Join-Path $buildDirectory ("qemu-realtest-$mode.stderr.log")
$extractedLog = Join-Path $buildDirectory ("realtest-$mode.log")
$loaderExtractedLog = Join-Path $buildDirectory ("realtest-$mode-loader.log")
$liveExtractedLog = Join-Path $buildDirectory ("realtest-$mode-live.log")

function Find-Executable([string]$explicitPath, [string[]]$candidates, [string]$name) {
    if ($explicitPath) {
        if (-not (Test-Path -LiteralPath $explicitPath)) {
            throw "Executable not found: $explicitPath"
        }
        return (Resolve-Path -LiteralPath $explicitPath).Path
    }
    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path -LiteralPath $candidate)) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    $command = Get-Command $name -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    throw "$name was not found."
}

$qemu = Find-Executable $QemuPath @(
    'C:\Program Files\qemu\qemu-system-x86_64.exe',
    'C:\Program Files\QEMU\qemu-system-x86_64.exe'
) 'qemu-system-x86_64.exe'
$qemuDirectory = Split-Path -Parent $qemu
$qemuImg = Find-Executable '' @(
    (Join-Path $qemuDirectory 'qemu-img.exe'),
    'C:\Program Files\qemu\qemu-img.exe'
) 'qemu-img.exe'
$firmware = Find-Executable $FirmwarePath @(
    (Join-Path $qemuDirectory 'share\edk2-x86_64-code.fd'),
    'C:\Program Files\qemu\share\edk2-x86_64-code.fd'
) 'edk2-x86_64-code.fd'
$make = Find-Executable '' @(
    'C:\Program Files\w64devkit\bin\make.exe',
    'C:\w64devkit\bin\make.exe'
) 'make.exe'

$nativeBin = @(
    'C:\Program Files\w64devkit\bin',
    'C:\w64devkit\bin',
    (Join-Path $projectRoot 'w64devkit\bin')
) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
$elfBin = @(
    'C:\Program Files\x86_64-elf-tools-windows\bin',
    'C:\x86_64-elf-tools-windows\bin',
    (Join-Path $projectRoot 'x86_64-elf-tools\bin')
) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if ($nativeBin) { $env:Path = "$nativeBin;$env:Path" }
if ($elfBin) { $env:Path = "$elfBin;$env:Path" }

New-Item -ItemType Directory -Force -Path $buildDirectory | Out-Null
if (-not $SkipBuild -and -not $FailureTest) {
    $buildScript = Join-Path $projectRoot 'tools\build-windows.ps1'
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $buildScript `
        -Build $Build -Debug 2 -Serial -RealTest
    if ($LASTEXITCODE -ne 0) { throw "Real-test build failed ($LASTEXITCODE)." }
}
elseif (-not $SkipBuild -and $FailureTest) {
    $kernelTarget = "$Build/esp/EFI/OS/kernel.elf"
    $makeArguments = @(
        '-f', 'GNUmakefile', '-B',
        "BUILD=$Build", 'TOOLPREFIX=x86_64-w64-mingw32-',
        'ELFTOOLPREFIX=x86_64-elf-', 'HOSTCC=gcc', 'DEBUG=2',
        'OS_DEBUG_SERIAL=1', 'OS_REALTEST=1',
        'OS_REALTEST_FAILURE_TEST=1', $kernelTarget
    )
    Push-Location $projectRoot
    try {
        & $make @makeArguments
        if ($LASTEXITCODE -ne 0) { throw "Failure-test kernel build failed ($LASTEXITCODE)." }
    }
    finally {
        Pop-Location
    }
}

foreach ($requiredPath in @(
    (Join-Path $espPath 'EFI\BOOT\BOOTX64.EFI'),
    (Join-Path $espPath 'EFI\OS\kernel.elf'),
    (Join-Path $espPath 'EFI\OS\loader.conf')
)) {
    if (-not (Test-Path -LiteralPath $requiredPath)) {
        throw "Missing real-test boot file: $requiredPath"
    }
}

Remove-Item -LiteralPath $imagePath,$legacyBootImagePath -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $serialLog,$qemuLog,$qemuErrorLog,$extractedLog,
    $loaderExtractedLog,$liveExtractedLog -Force -ErrorAction SilentlyContinue
& $qemuImg convert -O raw "fat:ro:$espPath" $imagePath
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $imagePath)) {
    throw 'qemu-img could not create the writable FAT test image.'
}

$storageArguments = @()
if ($Uas) {
    # 此环境中的 EDK2 无法从 usb-uas 启动。仅在加载固件时使用 BOT 磁盘，随后在内核入口标记处移除它，
    # 使内核只测试一个 USB 设备：UAS 被测设备。
    Copy-Item -LiteralPath $imagePath -Destination $legacyBootImagePath -Force
    $storageArguments += @(
        '-device', 'usb-uas,id=vm-usb-uas,bus=vm-xhci.0,port=1',
        '-device', 'scsi-hd,bus=vm-usb-uas.0,scsi-id=0,lun=0,drive=vm-realtest',
        '-drive', ('if=none,id=vm-boot,format=raw,file={0}' -f
            $legacyBootImagePath),
        '-device', 'usb-storage,id=vm-usb-boot,drive=vm-boot,bus=vm-xhci.0,port=2'
    )
} else {
    $storageArguments += @(
        '-device', 'usb-storage,id=vm-usb-storage,drive=vm-realtest,bus=vm-xhci.0,port=1'
    )
}

$inputArguments = if ($InputTest -or $FileManagerTest) {
    @(
        # 在有设备的 Hub 前保留一个空 Hub。这模拟实体 USB3/USB2 companion 布局；若只处理第一个根 Hub，
        # 键盘就会被隐藏。
        '-device', 'usb-hub,id=vm-empty-hub,bus=vm-xhci.0,port=2,port-power=on',
        '-device', 'usb-hub,id=vm-input-hub,bus=vm-xhci.0,port=3,port-power=on',
        '-device', 'usb-kbd,id=vm-input-kbd,bus=vm-xhci.0,port=3.1',
        '-device', 'usb-mouse,id=vm-input-mouse,bus=vm-xhci.0,port=3.2'
    )
} else {
    @()
}
$monitorArguments = if ($InputTest -or $FileManagerTest -or $Uas) {
    @('-monitor', "tcp:127.0.0.1:$MonitorPort,server=on,wait=off")
} else {
    @('-monitor', 'none')
}

$qemuArguments = @(
    '-machine', 'q35',
    '-vga', 'none',
    '-device', 'VGA,vgamem_mb=64',
    '-m', "${MemoryMiB}M",
    '-cpu', 'qemu64,rdrand=on',
    '-smp', "$Cpu",
    '-drive', ('if=pflash,format=raw,readonly=on,file={0}' -f $firmware),
    '-boot', 'order=c'
) + @(
    '-drive', ('if=none,id=vm-realtest,format=raw,cache=directsync,file={0}' -f $imagePath),
    '-device', 'qemu-xhci,id=vm-xhci,msix=on'
) + $storageArguments + $inputArguments + @(
    '-serial', "file:$serialLog"
) + $monitorArguments + @(
    '-display', 'none',
    '-no-reboot',
    '-no-shutdown',
    '-d', 'guest_errors',
    '-D', $qemuLog,
    '-accel', 'tcg,thread=multi'
)

$processArguments = @(
    $qemuArguments | ForEach-Object {
        $argument = [string]$_
        if ($argument -match '\s' -and $argument -notmatch '^".*"$') {
            '"' + $argument + '"'
        }
        else { $argument }
    }
)
$qemuProcess = Start-Process -FilePath $qemu -ArgumentList $processArguments `
    -WorkingDirectory $projectRoot -WindowStyle Hidden -PassThru `
    -RedirectStandardError $qemuErrorLog
$marker = $null
$extractScript = Join-Path $projectRoot 'tools\extract-fat-file.ps1'
$nextImageCheck = Get-Date
$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
$hmpClient = $null
$hmpWriter = $null
$inputInjected = $false
$zoomInjected = $false
$inputPassed = $false
$fileManagerLaunchInjected = $false
$fileManagerNavigationInjected = $false
$fileManagerPassed = $false
$fileManagerKeys = @('down', 'down', 'ret', 'end', 'left', 'left', 'ret')
$fileManagerKeyIndex = 0
$nextFileManagerKey = Get-Date
$bootDeviceRemoved = $false
$keyboardProbeObserved = $false
$nextHmpConnect = Get-Date
$nextKeyboardProbe = Get-Date
try {
    while ((Get-Date) -lt $deadline) {
        $observedLog = ''
        $now = Get-Date
        if (($InputTest -or $FileManagerTest -or $Uas) -and
            $hmpWriter -eq $null -and
            $now -ge $nextHmpConnect) {
            try {
                $hmpClient = [Net.Sockets.TcpClient]::new()
                $hmpClient.Connect('127.0.0.1', $MonitorPort)
                $hmpWriter = [IO.StreamWriter]::new(
                    $hmpClient.GetStream(), [Text.Encoding]::ASCII, 1024, $true)
                $hmpWriter.NewLine = "`n"
                $hmpWriter.AutoFlush = $true
            }
            catch {
                if ($hmpClient -ne $null) { $hmpClient.Dispose() }
                $hmpClient = $null
                $nextHmpConnect = $now.AddMilliseconds(250)
            }
        }
        # Exercise the interrupt-IN path as soon as enumeration completes.
        # Repeated harmless key taps make this independent of boot timing;
        # QEMU drops taps sent before the USB keyboard is configured.
        if (($InputTest -or $FileManagerTest) -and
            $hmpWriter -ne $null -and
            -not $keyboardProbeObserved -and $now -ge $nextKeyboardProbe) {
            $hmpWriter.WriteLine('sendkey a 20')
            $nextKeyboardProbe = $now.AddMilliseconds(250)
        }
        if (Test-Path -LiteralPath $serialLog) {
            $serial = Get-Content -LiteralPath $serialLog -Raw -ErrorAction SilentlyContinue
            $observedLog = $serial
            if ($Uas -and -not $bootDeviceRemoved -and
                $hmpWriter -ne $null -and
                $serial -match 'OS_KERNEL_ENTRY') {
                $hmpWriter.WriteLine('device_del vm-usb-boot')
                $bootDeviceRemoved = $true
            }
            if (-not $InputTest -and -not $FileManagerTest -and
                $serial -match 'OS_REALTEST_PASS') {
                $marker = 'PASS'
                break
            }
            if ($serial -match 'OS_REALTEST_FAIL') {
                $marker = 'FAIL'
                break
            }
        }
        if (-not $marker -and (Get-Date) -ge $nextImageCheck -and
            (Test-Path -LiteralPath $imagePath)) {
            try {
                & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $extractScript `
                    -Image $imagePath -Path 'log/realtest.log' `
                    -Output $liveExtractedLog 2>$null | Out-Null
            }
            catch { }
            if (Test-Path -LiteralPath $liveExtractedLog) {
                $liveLog = Get-Content -LiteralPath $liveExtractedLog -Raw
                $observedLog = $liveLog
                if (-not $InputTest -and -not $FileManagerTest -and
                    $liveLog -match 'OS_REALTEST_PASS') {
                    $marker = 'PASS'
                } elseif ($liveLog -match 'OS_REALTEST_FAIL') {
                    $marker = 'FAIL'
                }
            }
            $nextImageCheck = (Get-Date).AddSeconds(2)
        }
        if (($InputTest -or $FileManagerTest) -and -not $marker) {
            if ($observedLog -match 'OS_USB_KEYBOARD_EVENT_OK') {
                $keyboardProbeObserved = $true
            }
        }
        if ($InputTest -and -not $marker) {
            if (-not $inputInjected -and
                $observedLog -match 'OS_DESKTOP_CACHE_OK') {
                # Super+3 opens the third dock application (Notepad).
                $hmpWriter.WriteLine('sendkey meta_l-3 20')
                $inputInjected = $true
            }
            if ($inputInjected -and -not $zoomInjected -and
                $observedLog -match
                    'OS_DESKTOP_CACHE_OK[\s\S]*OS_NOTEPAD_READY SIZE=22') {
                $hmpWriter.WriteLine('sendkey ctrl-equal 20')
                $zoomInjected = $true
            }
            if ($zoomInjected -and
                $observedLog -match 'OS_USB_KEYBOARD_EVENT_OK' -and
                $observedLog -match 'OS_DIAG_HID_REPORT .*PROTO=1' -and
                $observedLog -match
                    'OS_DIAG_INPUT_ROUTE code=46 value=1 focus=[1-9][0-9]* target=[1-9][0-9]* delivered=1' -and
                $observedLog -match
                    'OS_DESKTOP_CACHE_OK[\s\S]*OS_NOTEPAD_READY SIZE=22[\s\S]*OS_TEXT_ZOOM_SIZE=26') {
                $inputPassed = $true
                $marker = 'PASS'
            }
        }
        if ($FileManagerTest -and -not $marker) {
            if (-not $fileManagerLaunchInjected -and
                $hmpWriter -ne $null -and
                $keyboardProbeObserved -and
                $observedLog -match 'OS_DESKTOP_CACHE_OK') {
                # Super+1 opens the first dock application (File Manager).
                $hmpWriter.WriteLine('sendkey meta_l-1 20')
                $fileManagerLaunchInjected = $true
            }
            if ($fileManagerLaunchInjected -and
                $observedLog -match 'OS_FILEMAN_READY') {
                $fileManagerNavigationInjected = $true
            }
            if ($fileManagerNavigationInjected -and
                $fileManagerKeyIndex -lt $fileManagerKeys.Count -and
                $hmpWriter -ne $null -and $now -ge $nextFileManagerKey) {
                # Leave a gap between key holds so each navigation event is
                # delivered independently by the USB keyboard path.
                $key = $fileManagerKeys[$fileManagerKeyIndex]
                $hmpWriter.WriteLine("sendkey $key 20")
                ++$fileManagerKeyIndex
                $nextFileManagerKey = $now.AddMilliseconds(100)
            }
            if ($fileManagerNavigationInjected -and
                $observedLog -match
                    'OS_FILEMAN_READY[\s\S]*OS_NOTEPAD_READY SIZE=22') {
                $fileManagerPassed = $true
                $marker = 'PASS'
            }
        }
        if ($marker) { break }
        if ($qemuProcess.HasExited) { break }
        # Keep the check non-blocking; the caller owns any pacing policy.
        [void]$qemuProcess.WaitForExit(0)
    }
    if (-not $InputTest -and -not $FileManagerTest -and -not $marker -and
        (Test-Path -LiteralPath $serialLog)) {
        $serial = Get-Content -LiteralPath $serialLog -Raw -ErrorAction SilentlyContinue
        if ($serial -match 'OS_REALTEST_PASS') { $marker = 'PASS' }
        elseif ($serial -match 'OS_REALTEST_FAIL') { $marker = 'FAIL' }
    }
    if (-not $marker -and -not $qemuProcess.HasExited) {
        throw "QEMU did not reach a real-test result within $TimeoutSeconds seconds."
    }
}
finally {
    if ($hmpWriter -ne $null) { $hmpWriter.Dispose() }
    if ($hmpClient -ne $null) { $hmpClient.Dispose() }
    if (-not $qemuProcess.HasExited) {
        Stop-Process -Id $qemuProcess.Id -Force -ErrorAction SilentlyContinue
    }
    try { $qemuProcess.WaitForExit(5000) | Out-Null } catch { }
}

$logImagePath = $imagePath
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $extractScript `
    -Image $logImagePath -Path 'log/realtest.log' -Output $extractedLog
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $extractedLog)) {
    throw 'The QEMU FAT image did not contain log/realtest.log.'
}
$realtestLog = Get-Content -LiteralPath $extractedLog -Raw
if ($Uas) {
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $extractScript `
        -Image $legacyBootImagePath -Path 'log/realtest.log' `
        -Output $loaderExtractedLog
    if ($LASTEXITCODE -ne 0 -or
        -not (Test-Path -LiteralPath $loaderExtractedLog)) {
        throw 'The QEMU UAS boot image did not contain the loader log.'
    }
    $loaderLog = Get-Content -LiteralPath $loaderExtractedLog -Raw
    $loaderLogPath = $loaderExtractedLog
} else {
    $loaderLog = $realtestLog
    $loaderLogPath = $extractedLog
}
if (-not $marker) {
    if ($realtestLog -match 'OS_REALTEST_PASS') { $marker = 'PASS' }
    elseif ($realtestLog -match 'OS_REALTEST_FAIL') { $marker = 'FAIL' }
}
if (-not $marker) { throw 'The QEMU real-test log has no PASS/FAIL marker.' }
if ($marker -ne $expectedMarker) {
    throw "QEMU real-test expected $expectedMarker but observed $marker."
}
if ($loaderLog -notmatch 'OS_LOADER_BEGIN') {
    throw 'Real-test loader log is missing marker: OS_LOADER_BEGIN'
}
foreach ($requiredMarker in @(
    'OS_REALTEST_LOG_READY',
    "OS_REALTEST_$expectedMarker"
)) {
    if ($realtestLog -notmatch [regex]::Escape($requiredMarker)) {
        throw "Real-test log is missing marker: $requiredMarker"
    }
}
if ($InputTest -and -not $inputPassed) {
    throw 'QEMU input test did not observe Notepad receiving and applying the font zoom key.'
}
if ($FileManagerTest -and -not $fileManagerPassed) {
    throw 'QEMU file-manager test did not open Notepad from a selected file.'
}
if ($Uas) {
    $uasSerialLog = Get-Content -LiteralPath $serialLog -Raw -ErrorAction SilentlyContinue
    if ($uasSerialLog -notmatch 'OS_UAS_SCSI_OK') {
        throw 'QEMU UAS test did not complete a SCSI command on the UAS transport.'
    }
}

$resultPath = Join-Path $buildDirectory ("realtest-$mode.result")
Set-Content -LiteralPath $resultPath -Value @(
    "mode=$mode",
    "expected_marker=$expectedMarker",
    "marker=$marker",
    "serial=$serialLog",
    "qemu=$qemuLog",
    "log=$extractedLog",
    "loader_log=$loaderLogPath",
    "input_test=$inputPassed",
    "file_manager_test=$fileManagerPassed",
    "boot_device_removed=$bootDeviceRemoved",
    "uas_scsi=$($Uas -and $uasSerialLog -match 'OS_UAS_SCSI_OK')"
)
Write-Host "QEMU real-test $mode passed: $extractedLog"
