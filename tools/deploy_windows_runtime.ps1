param(
    [string]$BinaryDir = "build\\win-release\\Release",
    [string]$QtPrefix = $env:QT_PREFIX_PATH,
    [switch]$SkipCompilerRuntime
)

$ErrorActionPreference = 'Stop'
$scriptDir = Split-Path -Path $MyInvocation.MyCommand.Definition -Parent
$projectRoot = Resolve-Path "$scriptDir/.."

function Resolve-ProjectPath {
    param([string]$Relative)
    if ([System.IO.Path]::IsPathRooted($Relative)) {
        return (Resolve-Path $Relative)
    }
    return (Resolve-Path (Join-Path $projectRoot $Relative))
}

function Get-QtPrefix {
    param([string]$Existing)
    if ($Existing) {
        if (Test-Path $Existing) { return (Resolve-Path $Existing) }
        Write-Warning "提供的 Qt 前缀路径不存在：$Existing，尝试自动检测"
    }

    function Resolve-QtPrefixFromExe {
        param([string]$ExePath)
        if (-not (Test-Path $ExePath)) { return $null }
        try {
            $raw = & $ExePath --install-prefix 2>$null
            $lines = $raw | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
            $candidate = $lines | Where-Object { $_ -notmatch '^(Usage:|--install-prefix)' } | Select-Object -First 1
            if ($candidate) { return (Resolve-Path $candidate.Trim()) }
        }
        catch {}

        $current = Split-Path $ExePath -Parent
        if ((Split-Path $current -Leaf) -ieq 'bin') {
            $current = Split-Path $current -Parent
        }
        if ($current -and (Split-Path $current -Leaf) -match '^Qt(5|6)$') {
            $current = Split-Path $current -Parent
            $current = Split-Path $current -Parent
        }
        if ($current -and (Test-Path $current)) { return (Resolve-Path $current) }
        return $null
    }

    $qtPath = $null

    if ($env:VCPKG_ROOT) {
        $potentialTriplets = @()
        if ($env:VCPKG_TARGET_TRIPLET) { $potentialTriplets += $env:VCPKG_TARGET_TRIPLET }
        if ($env:VCPKG_DEFAULT_TRIPLET) { $potentialTriplets += $env:VCPKG_DEFAULT_TRIPLET }
        $potentialTriplets += 'x64-windows', 'x86-windows'
        $potentialTriplets = $potentialTriplets | Where-Object { $_ } | Select-Object -Unique
        foreach ($triplet in $potentialTriplets) {
            $candidate = Join-Path $env:VCPKG_ROOT "installed/$triplet/tools/Qt6/bin/qtpaths6.exe"
            $qtPath = Resolve-QtPrefixFromExe -ExePath $candidate
            if ($qtPath) { break }
            $candidateLegacy = Join-Path $env:VCPKG_ROOT "installed/$triplet/tools/Qt5/bin/qtpaths.exe"
            $qtPath = Resolve-QtPrefixFromExe -ExePath $candidateLegacy
            if ($qtPath) { break }
        }
    }

    if (-not $qtPath) {
        $cmd = Get-Command qtpaths6 -ErrorAction SilentlyContinue
        if ($cmd) { $qtPath = Resolve-QtPrefixFromExe -ExePath $cmd.Source }
    }

    if (-not $qtPath) {
        $cmdLegacy = Get-Command qtpaths -ErrorAction SilentlyContinue
        if ($cmdLegacy) { $qtPath = Resolve-QtPrefixFromExe -ExePath $cmdLegacy.Source }
    }

    if ($qtPath) { return $qtPath }
    throw "无法自动检测 Qt 安装目录，请通过 QT_PREFIX_PATH 环境变量指定"
}

$resolvedBinaryDir = Resolve-ProjectPath -Relative $BinaryDir
$exePath = Join-Path $resolvedBinaryDir 'my_calib_gui.exe'
if (-not (Test-Path $exePath)) {
    throw "未找到可执行文件：$exePath"
}

$qtPrefix = Get-QtPrefix -Existing $QtPrefix
Write-Host "Using Qt prefix: $qtPrefix" -ForegroundColor DarkGray

$windeployCandidates = @(
    (Join-Path $qtPrefix 'bin/windeployqt.exe'),
    (Join-Path $qtPrefix 'tools/Qt6/bin/windeployqt.exe'),
    (Join-Path $qtPrefix 'bin/windeployqt6.exe')
)
$windeploy = $windeployCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $windeploy) {
    throw "未找到 windeployqt.exe，请确认 Qt 已安装完整"
}

$windeployArgs = @('--release', '--no-translations', '--dir', $resolvedBinaryDir, $exePath)
if (-not $SkipCompilerRuntime) {
    $windeployArgs = @('--compiler-runtime') + $windeployArgs
}

Write-Host "Running windeployqt:" -ForegroundColor DarkGray
Write-Host "  $windeploy $($windeployArgs -join ' ')"
& $windeploy @windeployArgs

$platformDir = Join-Path $resolvedBinaryDir 'platforms'
if (-not (Test-Path $platformDir)) {
    New-Item -ItemType Directory -Path $platformDir | Out-Null
}

if (-not (Test-Path (Join-Path $platformDir 'qwindows.dll'))) {
    $candidatePlatformDirs = @(
        (Join-Path $qtPrefix 'plugins/platforms'),
        (Join-Path $qtPrefix 'Qt6/plugins/platforms'),
        (Join-Path $qtPrefix 'lib/qt6/plugins/platforms')
    )
    foreach ($candidate in $candidatePlatformDirs) {
        if (Test-Path $candidate) {
            Write-Host "Copying platform plugins from $candidate" -ForegroundColor DarkCyan
            Copy-Item -Path (Join-Path $candidate '*') -Destination $platformDir -Recurse -Force
            break
        }
    }
}

if (-not (Test-Path (Join-Path $platformDir 'qwindows.dll'))) {
    Write-Warning '未找到 qwindows.dll，Qt 安装可能不完整或版本与构建二进制不匹配'
}

$qtConfPath = Join-Path $resolvedBinaryDir 'qt.conf'
$qtConfContent = "[Paths]`nPrefix=.\`nPlugins=.\`nTranslations=./translations`n"
Set-Content -Path $qtConfPath -Value $qtConfContent -Encoding UTF8

Write-Host "Runtime deployment completed. 可从 $resolvedBinaryDir 直接启动 my_calib_gui.exe" -ForegroundColor Green
