# Build the Windows release zip (WS-13; master D3, D9, section 4.11): two STATIC executables and nothing else that
# runs. No DLL, no plugins folder, no qt.conf: the window links Qt (with the qwindows platform plugin) and the MSVC
# runtime statically, and so does the console program.
#
#   pwsh packaging/windows/build-release.ps1 -QtGui <static GUI-profile prefix> -QtHeadless <static headless prefix>
#        -BuildRoot <dir> -OutDir <dir> [-Seams]
#
# Run from an MSVC developer environment. Two build trees, one per static Qt profile (packaging/qt-static/profiles):
#   <BuildRoot>\headless  aether-serial-bridge.exe against the headless profile (no QtGui at all)
#   <BuildRoot>\gui       AetherSerialBridge.exe against the GUI profile, carrying the SHA-256 of the headless
#                         executable built above (ASB_HEADLESS_DIGEST_EXE): Save & apply installs exactly that file
# Output: <OutDir>\aether-serial-bridge-windows-x64.zip and the same files unzipped in <OutDir>\tree.
#
# -Seams builds the headless executable of the headless tree once more with the test seams (ASB_BUILD_TESTS=ON,
# the native suites' --test-op world) into <BuildRoot>\headless-seams. It is never shipped.
param(
    [Parameter(Mandatory = $true)][string]$QtGui,
    [Parameter(Mandatory = $true)][string]$QtHeadless,
    [Parameter(Mandatory = $true)][string]$BuildRoot,
    [Parameter(Mandatory = $true)][string]$OutDir,
    [switch]$Seams
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$src = (Resolve-Path "$PSScriptRoot\..\..").Path

function Invoke-Checked([string]$what, [scriptblock]$block) {
    & $block
    if ($LASTEXITCODE -ne 0) { throw "$what failed ($LASTEXITCODE)" }
}
foreach ($qt in $QtGui, $QtHeadless) {
    if (-not (Test-Path "$qt\lib\Qt6Core.lib" -PathType Leaf) -or (Test-Path "$qt\bin\Qt6Core.dll")) {
        throw "$qt is not a static Qt (lib\Qt6Core.lib missing or bin\Qt6Core.dll present)"
    }
}
if (Test-Path "$QtHeadless\lib\Qt6Gui.lib") { throw "$QtHeadless has QtGui: it is not the headless profile" }

# The static MSVC runtime everywhere (Qt's -static-runtime): a /MD object would not link against it.
$common = @('-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', '-DASB_BUILD_TESTS=OFF', '-DASB_WERROR=ON',
            '-DASB_REQUIRE_PINNED_QT=ON', '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded')

$hb = "$BuildRoot\headless"
Invoke-Checked 'headless configure' { cmake -S $src -B $hb @common -DASB_BUILD_GUI=OFF "-DCMAKE_PREFIX_PATH=$QtHeadless" }
Invoke-Checked 'headless build' { cmake --build $hb --target aether-serial-bridge }
$headlessExe = "$hb\bin\aether-serial-bridge.exe"

$gb = "$BuildRoot\gui"
Invoke-Checked 'GUI configure' {
    cmake -S $src -B $gb @common -DASB_BUILD_GUI=ON "-DCMAKE_PREFIX_PATH=$QtGui" "-DASB_HEADLESS_DIGEST_EXE=$headlessExe"
}
Invoke-Checked 'GUI build' { cmake --build $gb --target AetherSerialBridge }
$guiExe = "$gb\bin\AetherSerialBridge.exe"

$tree = "$OutDir\tree"
Remove-Item -Recurse -Force $tree -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $tree | Out-Null
Copy-Item $guiExe, $headlessExe $tree
Copy-Item "$src\packaging\windows\README.txt" "$tree\README.txt"
Copy-Item "$src\LICENSE" "$tree\LICENSE.txt"
Copy-Item "$src\packaging\THIRD-PARTY-NOTICES.txt" "$tree\THIRD-PARTY-NOTICES.txt"

$zip = "$OutDir\aether-serial-bridge-windows-x64.zip"
Remove-Item -Force $zip -ErrorAction SilentlyContinue
Compress-Archive -Path "$tree\*" -DestinationPath $zip
Get-ChildItem $tree | Format-Table -AutoSize Length, Name
Write-Host "built $zip"

if ($Seams) {
    $sb = "$BuildRoot\headless-seams"
    $seamCommon = $common | ForEach-Object { if ($_ -eq '-DASB_BUILD_TESTS=OFF') { '-DASB_BUILD_TESTS=ON' } else { $_ } }
    Invoke-Checked 'seam build configure' { cmake -S $src -B $sb @seamCommon -DASB_BUILD_GUI=OFF "-DCMAKE_PREFIX_PATH=$QtHeadless" }
    Invoke-Checked 'seam build' { cmake --build $sb --target aether-serial-bridge }
    New-Item -ItemType Directory -Force "$OutDir\seams" | Out-Null
    Copy-Item "$sb\bin\aether-serial-bridge.exe" "$OutDir\seams\aether-serial-bridge.exe"
    Write-Host "built the seam build $OutDir\seams\aether-serial-bridge.exe (never shipped)"
}
