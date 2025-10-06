$ErrorActionPreference = 'Stop'
$ScriptDir = Split-Path -Path $MyInvocation.MyCommand.Definition -Parent
$ProjectRoot = Resolve-Path "$ScriptDir/.."
$BuildDir = $env:BUILD_DIR
if (-not $BuildDir) {
    $BuildDir = Join-Path $ProjectRoot 'build/package-win'
}
$Config = 'Release'

if (Test-Path $BuildDir) {
    Write-Host "Cleaning existing package directory: $BuildDir" -ForegroundColor DarkGray
    Remove-Item -Recurse -Force $BuildDir
}

function Detect-QtPrefix {
    param([string]$Existing)
    if ($Existing) { return $Existing }

    function Resolve-QtPrefixFromExe {
        param([string]$ExePath)
        if (-not (Test-Path $ExePath)) { return $null }
        try {
            $raw = & $ExePath --install-prefix 2>$null
            $lines = $raw | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
            $candidate = $lines | Where-Object { $_ -notmatch '^(Usage:|--install-prefix)' } | Select-Object -First 1
            if ($candidate) {
                return $candidate.Trim()
            }
        }
        catch {
            # ignore and fall back to heuristics
        }

        $current = Split-Path $ExePath -Parent
        if ((Split-Path $current -Leaf) -ieq 'bin') {
            $current = Split-Path $current -Parent
        }
        if ($current -and (Split-Path $current -Leaf) -match '^Qt(5|6)$') {
            $current = Split-Path $current -Parent # tools
            $current = Split-Path $current -Parent # installed/<triplet>
        }
        return $current
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
        if ($cmd) {
            $qtPath = Resolve-QtPrefixFromExe -ExePath $cmd.Source
        }
    }

    if (-not $qtPath) {
        $cmdLegacy = Get-Command qtpaths -ErrorAction SilentlyContinue
        if ($cmdLegacy) {
            $qtPath = Resolve-QtPrefixFromExe -ExePath $cmdLegacy.Source
        }
    }

    if ($qtPath) { return $qtPath }

    throw "无法自动检测 Qt 安装目录，请通过 QT_PREFIX_PATH 环境变量指定"
}

function Convert-ToCMakePath {
    param([string]$Path)
    if ([string]::IsNullOrWhiteSpace($Path)) { return $null }
    return ($Path -replace '\\', '/')
}

$qtPrefix = Detect-QtPrefix -Existing $env:QT_PREFIX_PATH
$windeploy = Join-Path $qtPrefix 'bin/windeployqt.exe'
if (-not (Test-Path $windeploy)) {
    $altWindeploy = Join-Path $qtPrefix 'tools/Qt6/bin/windeployqt.exe'
    if (Test-Path $altWindeploy) {
        $windeploy = $altWindeploy
    }
}
if (-not (Test-Path $windeploy)) {
    throw "未找到 windeployqt.exe (${windeploy})"
}

if (-not $env:VCINSTALLDIR) {
    $programFilesX86 = ${env:ProgramFiles(x86)}
    if (-not $programFilesX86) { $programFilesX86 = ${env:ProgramFiles} }
    if ($programFilesX86) {
        $vswhere = Join-Path $programFilesX86 'Microsoft Visual Studio\Installer\vswhere.exe'
        if (Test-Path $vswhere) {
            $vsInstallPath = (& $vswhere -latest -products * -requires Microsoft.Component.MSVC.Tools.x86.x64 -property installationPath | Select-Object -First 1)
            if (-not $vsInstallPath) {
                $vsInstallPath = (& $vswhere -latest -products * -requires Microsoft.Component.MSVC.v143.x86.x64 -property installationPath | Select-Object -First 1)
            }
            if (-not $vsInstallPath) {
                $vsInstallPath = (& $vswhere -latest -products * -property installationPath | Select-Object -First 1)
            }
            if (-not $vsInstallPath) {
                $vsInstallPath = (& $vswhere -latest -property installationPath | Select-Object -First 1)
            }
            if ($vsInstallPath) {
                $vsInstallPath = $vsInstallPath.Trim()
                $vcDir = Join-Path $vsInstallPath 'VC'
                if (Test-Path $vcDir) {
                    $env:VCINSTALLDIR = "$vcDir\"
                    Write-Host "Detected VCINSTALLDIR at $($env:VCINSTALLDIR)" -ForegroundColor DarkCyan
                }
            }
        }
    }
}

$cmakeArgs = @('-S', $ProjectRoot, '-B', $BuildDir, "-DCMAKE_BUILD_TYPE=${Config}")

$prefixList = @()
if ($qtPrefix) {
    Write-Host "Using Qt prefix: $qtPrefix" -ForegroundColor DarkGray
    $prefixList += $qtPrefix
}
if ($env:CMAKE_PREFIX_PATH) {
    $prefixList += $env:CMAKE_PREFIX_PATH.Split(';') | Where-Object { $_ }
}

$vcpkgRoot = $env:VCPKG_ROOT
$toolchainFile = $env:CMAKE_TOOLCHAIN_FILE
if (-not $toolchainFile -and $vcpkgRoot) {
    $candidateToolchain = Join-Path $vcpkgRoot 'scripts/buildsystems/vcpkg.cmake'
    if (Test-Path $candidateToolchain) {
        $toolchainFile = $candidateToolchain
    }
}

$vcpkgTriplet = $env:VCPKG_TARGET_TRIPLET
if (-not $vcpkgTriplet -and $vcpkgRoot) {
    $installedRoot = Join-Path $vcpkgRoot 'installed'
    if (Test-Path $installedRoot) {
        $candidate = Get-ChildItem -Path $installedRoot -Directory -ErrorAction SilentlyContinue | Where-Object {
            (Test-Path (Join-Path $_.FullName 'share/opencv')) -or (Test-Path (Join-Path $_.FullName 'share/opencv4'))
        } | Select-Object -First 1
        if ($candidate) { $vcpkgTriplet = $candidate.Name }
    }
    if (-not $vcpkgTriplet) { $vcpkgTriplet = 'x64-windows' }
}

$opencvDir = $env:OpenCV_DIR
if (-not $opencvDir -and $vcpkgRoot) {
    $potentialTriplets = @()
    if ($vcpkgTriplet) { $potentialTriplets += $vcpkgTriplet }
    $potentialTriplets += 'x64-windows', 'x64-windows-static'
    $potentialTriplets = $potentialTriplets | Select-Object -Unique
    foreach ($triplet in $potentialTriplets) {
        $candidateOpencv = Join-Path $vcpkgRoot "installed/$triplet/share/opencv"
        if (Test-Path $candidateOpencv) { $opencvDir = $candidateOpencv; break }
        $candidateOpencv4 = Join-Path $vcpkgRoot "installed/$triplet/share/opencv4"
        if (Test-Path $candidateOpencv4) { $opencvDir = $candidateOpencv4; break }
    }
}

if ($opencvDir) {
    $prefixList += $opencvDir
    $cmakeArgs += "-DOpenCV_DIR=$(Convert-ToCMakePath $opencvDir)"
}

$prefixList = $prefixList | Where-Object { $_ -and (Test-Path $_) } | Select-Object -Unique
if ($prefixList.Count -gt 0) {
    $prefixJoined = [string]::Join(';', ($prefixList | ForEach-Object { Convert-ToCMakePath $_ }))
    $cmakeArgs += "-DCMAKE_PREFIX_PATH=$prefixJoined"
}

if ($toolchainFile) {
    Write-Host "Using vcpkg toolchain: $toolchainFile" -ForegroundColor DarkCyan
    $cmakeArgs += "-DCMAKE_TOOLCHAIN_FILE=$(Convert-ToCMakePath $toolchainFile)"
    if ($vcpkgTriplet) {
        $cmakeArgs += "-DVCPKG_TARGET_TRIPLET=$vcpkgTriplet"
    }
}

Write-Host "Configuring CMake with arguments:" -ForegroundColor DarkGray
$cmakeArgs | ForEach-Object { Write-Host "  $_" }

cmake @cmakeArgs
cmake --build $BuildDir --config $Config --target my_calib_gui

$exePath = Join-Path (Join-Path $BuildDir $Config) 'my_calib_gui.exe'
if (-not (Test-Path $exePath)) {
    throw "未找到可执行文件 $exePath"
}

$releaseDir = Split-Path $exePath -Parent

$windeployArgs = @('--release', '--verbose', '1', '--dir', (Split-Path $exePath -Parent), $exePath)
if ($env:VCINSTALLDIR) {
    $windeployArgs = @('--compiler-runtime') + $windeployArgs
}
& $windeploy @windeployArgs

$platformDir = Join-Path $releaseDir 'platforms'
if (-not (Test-Path $platformDir)) {
    New-Item -ItemType Directory -Path $platformDir | Out-Null
}
if (-not (Test-Path (Join-Path $platformDir 'qwindows.dll'))) {
    $candidatePlatformDirs = @(
        (Join-Path $qtPrefix 'Qt6/plugins/platforms')
        (Join-Path $qtPrefix 'plugins/platforms')
        (Join-Path $qtPrefix 'lib/qt6/plugins/platforms')
    )
    foreach ($candidate in $candidatePlatformDirs) {
        if (Test-Path $candidate) {
            Write-Host "Copying Qt platform plugins from $candidate" -ForegroundColor DarkCyan
            Copy-Item -Path (Join-Path $candidate '*') -Destination $platformDir -Recurse -Force
            break
        }
    }
    if (-not (Test-Path (Join-Path $platformDir 'qwindows.dll'))) {
        Write-Warning "未能在 Qt 安装中找到 qwindows.dll，请检查 Qt 组件安装是否完整"
    }
}

$qtConfPath = Join-Path $releaseDir 'qt.conf'
$qtConfContent = "[Paths]`nPrefix=.\`nPlugins=.\`nTranslations=./translations`n"
Set-Content -Path $qtConfPath -Value $qtConfContent -Encoding UTF8

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

# 复制 MSVC 运行库（如果可用）
if ($env:VCINSTALLDIR) {
    $crtRoot = Join-Path $env:VCINSTALLDIR 'Redist\MSVC'
    if (Test-Path $crtRoot) {
        $versionDirs = Get-ChildItem -Path $crtRoot -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Name -match '^[0-9]' }
        $latestVersion = $versionDirs | Sort-Object Name -Descending | Select-Object -First 1
        if ($latestVersion) {
            $archRoot = Join-Path $latestVersion.FullName 'x64'
            if (Test-Path $archRoot) {
                $crtDirs = Get-ChildItem -Path $archRoot -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Name -match 'Microsoft\.VC\d+\.CRT' }
                foreach ($dir in $crtDirs) {
                    $copied = @()
                    Get-ChildItem -Path $dir.FullName -Filter '*.dll' -ErrorAction SilentlyContinue | ForEach-Object {
                        Copy-Item -Path $_.FullName -Destination (Split-Path $exePath -Parent) -Force
                        $copied += $_.Name
                    }
                    if ($copied.Count -gt 0) {
                        Write-Host "Bundled MSVC runtime: $($copied -join ', ')" -ForegroundColor DarkCyan
                    }
                }
            }
        }
    }
    else {
        Write-Warning "未找到 VC 运行库目录：$crtRoot"
    }
}
else {
    Write-Warning '未能设置 VCINSTALLDIR，无法拷贝 MSVC 运行库'
}


$releaseDir = Split-Path $exePath -Parent
$versionMatch = Select-String -Path $cacheFile -Pattern '^MyCalibGui_VERSION:STATIC=(.+)$' -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $versionMatch) {
    $versionMatch = Select-String -Path $cacheFile -Pattern '^CMAKE_PROJECT_VERSION:STATIC=(.+)$' -ErrorAction SilentlyContinue | Select-Object -First 1
}
$packageVersion = if ($versionMatch) { $versionMatch.Matches[0].Groups[1].Value } else { '0.0.0' }
$packageName = "Calib Evaluator-$packageVersion-win64"
$zipPath = Join-Path $BuildDir "$packageName.zip"
if (Test-Path $zipPath) {
    Remove-Item $zipPath -Force
}
Write-Host "Creating portable ZIP: $zipPath" -ForegroundColor DarkCyan
Compress-Archive -Path (Join-Path $releaseDir '*') -DestinationPath $zipPath -Force

$nsisCmd = Get-Command makensis.exe -ErrorAction SilentlyContinue
if (-not $nsisCmd) {
    Write-Warning "未检测到 NSIS (makensis.exe)，当前仅生成 ZIP 包。"
}

Write-Host "`nArtifacts in ${BuildDir}:" -ForegroundColor Cyan
Get-ChildItem -Path $BuildDir -Filter '*.exe' | ForEach-Object { Write-Host $_.FullName }
Get-ChildItem -Path $BuildDir -Filter '*.zip' | ForEach-Object { Write-Host $_.FullName }
