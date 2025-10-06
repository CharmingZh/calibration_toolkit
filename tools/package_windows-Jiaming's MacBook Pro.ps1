$ErrorActionPreference = 'Stop'
$ScriptDir = Split-Path -Path $MyInvocation.MyCommand.Definition -Parent
$ProjectRoot = Resolve-Path "$ScriptDir/.."
$BuildDir = $env:BUILD_DIR
if (-not $BuildDir) {
    $BuildDir = Join-Path $ProjectRoot 'build/package-win'
}
$Config = 'Release'

function Detect-QtPrefix {
    param([string]$Existing)
    if ($Existing) { return $Existing }

    if (Get-Command qtpaths6 -ErrorAction SilentlyContinue) {
        return (& qtpaths6 --install-prefix) | Select-Object -First 1
    }
    elseif (Get-Command qtpaths -ErrorAction SilentlyContinue) {
        return (& qtpaths --install-prefix) | Select-Object -First 1
    }
    else {
        throw "无法自动检测 Qt 安装目录，请通过 QT_PREFIX_PATH 环境变量指定"
    }
}

$qtPrefix = Detect-QtPrefix -Existing $env:QT_PREFIX_PATH
$windeploy = Join-Path $qtPrefix 'bin/windeployqt.exe'
if (-not (Test-Path $windeploy)) {
    throw "未找到 windeployqt.exe (${windeploy})"
}

$cmakeArgs = @('-S', $ProjectRoot, '-B', $BuildDir, "-DCMAKE_BUILD_TYPE=${Config}", "-DCMAKE_PREFIX_PATH=$qtPrefix")

cmake @cmakeArgs
cmake --build $BuildDir --config $Config --target my_calib_gui

$exePath = Join-Path (Join-Path $BuildDir $Config) 'my_calib_gui.exe'
if (-not (Test-Path $exePath)) {
    throw "未找到可执行文件 $exePath"
}

& $windeploy --release --verbose 1 --dir (Split-Path $exePath -Parent) $exePath

# 复制 OpenCV 运行库（如果可用）
$cacheFile = Join-Path $BuildDir 'CMakeCache.txt'
if (Test-Path $cacheFile) {
    $match = Select-String -Path $cacheFile -Pattern '^OpenCV_DIR:PATH=(.+)$' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($match) {
        $opencvDir = $match.Matches[0].Groups[1].Value
        if ($opencvDir) {
            $opencvBin = [System.IO.Path]::GetFullPath((Join-Path $opencvDir '..\..\bin'))
            if (Test-Path $opencvBin) {
                Write-Host "Bundling OpenCV runtime from $opencvBin"
                Copy-Item -Path (Join-Path $opencvBin 'opencv*.dll') -Destination (Split-Path $exePath -Parent) -Force -ErrorAction SilentlyContinue
            }
        }
    }
}

cmake --build $BuildDir --config $Config --target package

Write-Host "`nArtifacts in $BuildDir:" -ForegroundColor Cyan
Get-ChildItem -Path $BuildDir -Filter '*.exe' | ForEach-Object { Write-Host $_.FullName }
Get-ChildItem -Path $BuildDir -Filter '*.zip' | ForEach-Object { Write-Host $_.FullName }
