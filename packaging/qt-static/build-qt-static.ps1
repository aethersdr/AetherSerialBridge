# Build the pinned Qt (cmake/qt-pin.env) from source as a STATIC Qt for one Windows profile (WS-13; master D3,
# section 9). The Windows twin of build-qt-static.sh; read that header for the cache and stamp rules.
#
#   pwsh packaging/qt-static/build-qt-static.ps1 -QtProfile windows-gui|windows-headless -CacheRoot <dir> [-KeyOnly]
#
# -KeyOnly prints QT_STATIC_KEY=<key> and QT_STATIC_PREFIX=<prefix> and exits, building nothing (CI keys its
# actions/cache entry of the prefix with it; run it in the same developer environment as the build).
#
# Run from an MSVC developer environment (cl, link and ninja on PATH; VCToolsVersion set): the key includes the
# toolset, so a newer MSVC builds a new tree instead of mixing objects. The last line printed is
# QT_STATIC_PREFIX=<prefix>. Parallelism: ASB_QT_JOBS (default: the processor count).
param(
    [Parameter(Mandatory = $true)][string]$QtProfile,
    [Parameter(Mandatory = $true)][string]$CacheRoot,
    [switch]$KeyOnly
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$here = $PSScriptRoot
$root = (Resolve-Path "$here\..\..").Path

function Read-Env([string]$path) {
    $map = @{}
    foreach ($line in Get-Content $path) {
        if ($line -match '^([A-Z0-9_]+)=(.*)$') { $map[$Matches[1]] = $Matches[2].Trim().Trim('"') }
    }
    return $map
}
$pin = Read-Env "$root\cmake\qt-pin.env"
$src = Read-Env "$here\qt-static.env"
$version = $pin['QT_VERSION']

$profileFile = "$here\profiles\$QtProfile.txt"
if (-not (Test-Path $profileFile -PathType Leaf)) { throw "no profile $profileFile" }
New-Item -ItemType Directory -Force $CacheRoot | Out-Null
$CacheRoot = (Resolve-Path $CacheRoot).Path
$jobs = if ($env:ASB_QT_JOBS) { $env:ASB_QT_JOBS } else { [Environment]::ProcessorCount }

$cfgArgs = @()
foreach ($line in Get-Content $profileFile) {
    $l = ($line -split '#', 2)[0].Trim()
    if ($l) { $cfgArgs += ($l -split '\s+') }
}

if (-not $env:VCToolsVersion) { throw 'not an MSVC developer environment (VCToolsVersion is not set)' }
$scriptSha = (Get-FileHash -Algorithm SHA256 $PSCommandPath).Hash.ToLower()
$keyInput = @(
    "qt $version"
    "qtbase $($src['QTBASE_ZIP_SHA256'])"
    "qtserialport $($src['QTSERIALPORT_ZIP_SHA256'])"
    "profile $QtProfile"
    "args $($cfgArgs -join ' ')"
    "script $scriptSha"
    "compiler msvc $($env:VCToolsVersion)"
    "arch $env:PROCESSOR_ARCHITECTURE"
) -join "`n"
$sha = [Security.Cryptography.SHA256]::Create()
$key = (($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($keyInput + "`n")) | ForEach-Object { $_.ToString('x2') }) -join '').Substring(0, 16)
$prefix = "$CacheRoot\$version-$QtProfile-$key"
$stamp = "$prefix\.asb-qt-static"
if ($KeyOnly) {
    Write-Host "QT_STATIC_KEY=$key"
    Write-Host "QT_STATIC_PREFIX=$prefix"
    exit 0
}

function Test-Tree {
    if (-not (Test-Path "$prefix\lib\Qt6Core.lib" -PathType Leaf)) { return $false }
    if (Test-Path "$prefix\bin\Qt6Core.dll") { return $false }                          # static, not shared
    foreach ($f in 'lib\cmake\Qt6\Qt6Config.cmake', 'lib\cmake\Qt6SerialPort\Qt6SerialPortConfig.cmake', 'bin\qmake.exe') {
        if (-not (Test-Path "$prefix\$f" -PathType Leaf)) { return $false }
    }
    return ((& "$prefix\bin\qmake.exe" -query QT_VERSION) -eq $version)
}

if ((Test-Path $stamp -PathType Leaf) -and ((Get-Content -Raw $stamp) -eq $keyInput) -and (Test-Tree)) {
    Write-Host "static Qt $version ($QtProfile, key $key) already built"
    Write-Host "QT_STATIC_PREFIX=$prefix"
    exit 0
}
Write-Host "building static Qt $version, profile $QtProfile, key $key, $jobs jobs"
Write-Host $keyInput

function Get-Archive([string]$name, [string]$expected) {
    $dir = "$CacheRoot\src"
    New-Item -ItemType Directory -Force $dir | Out-Null
    $file = "$dir\$name"
    if ((Test-Path $file) -and ((Get-FileHash -Algorithm SHA256 $file).Hash.ToLower() -eq $expected)) { return $file }
    Remove-Item -Force "$file", "$file.part" -ErrorAction SilentlyContinue
    & curl.exe -fsSL --retry 3 -o "$file.part" "$($src['QT_STATIC_BASE_URL'])/$name"
    if ($LASTEXITCODE -ne 0) { throw "download of $name failed ($LASTEXITCODE)" }
    $got = (Get-FileHash -Algorithm SHA256 "$file.part").Hash.ToLower()
    if ($got -ne $expected) {
        Remove-Item -Force "$file.part"
        throw "$name does not match its pinned SHA-256 (expected $expected, got $got)"
    }
    Move-Item "$file.part" $file
    return $file
}

$baseZip = Get-Archive $src['QTBASE_ZIP'] $src['QTBASE_ZIP_SHA256']
$spZip = Get-Archive $src['QTSERIALPORT_ZIP'] $src['QTSERIALPORT_ZIP_SHA256']

# A short work path: Qt's object paths are deep, and MAX_PATH still applies to parts of the toolchain.
$work = "$CacheRoot\.w$($key.Substring(0, 8))"
Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$work\s", "$work\b", "$work\p" | Out-Null
try {
    & tar.exe -xf $baseZip -C "$work\s"
    if ($LASTEXITCODE -ne 0) { throw "extracting $baseZip failed" }
    & tar.exe -xf $spZip -C "$work\s"
    if ($LASTEXITCODE -ne 0) { throw "extracting $spZip failed" }
    $baseSrc = "$work\s\" + [IO.Path]::GetFileNameWithoutExtension($src['QTBASE_ZIP'])
    $spSrc = "$work\s\" + [IO.Path]::GetFileNameWithoutExtension($src['QTSERIALPORT_ZIP'])

    # Only now drop a previous (possibly half-built) tree and the stamp that vouched for it.
    Remove-Item -Recurse -Force $prefix -ErrorAction SilentlyContinue

    Push-Location "$work\b"
    try {
        & "$baseSrc\configure.bat" -prefix $prefix @cfgArgs
        if ($LASTEXITCODE -ne 0) { throw "qtbase configure failed ($LASTEXITCODE)" }
        & cmake --build . --parallel $jobs
        if ($LASTEXITCODE -ne 0) { throw "qtbase build failed ($LASTEXITCODE)" }
        & cmake --install .
        if ($LASTEXITCODE -ne 0) { throw "qtbase install failed ($LASTEXITCODE)" }
    } finally { Pop-Location }

    Push-Location "$work\p"
    try {
        & "$prefix\bin\qt-configure-module.bat" $spSrc
        if ($LASTEXITCODE -ne 0) { throw "qtserialport configure failed ($LASTEXITCODE)" }
        & cmake --build . --parallel $jobs
        if ($LASTEXITCODE -ne 0) { throw "qtserialport build failed ($LASTEXITCODE)" }
        & cmake --install .
        if ($LASTEXITCODE -ne 0) { throw "qtserialport install failed ($LASTEXITCODE)" }
    } finally { Pop-Location }

    if (-not (Test-Tree)) { throw "the static Qt at $prefix is incomplete" }
    Set-Content -NoNewline -Path "$stamp.tmp" -Value $keyInput
    Move-Item -Force "$stamp.tmp" $stamp
} finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
Write-Host "static Qt $(& "$prefix\bin\qmake.exe" -query QT_VERSION) built at $prefix"
Write-Host "QT_STATIC_PREFIX=$prefix"
