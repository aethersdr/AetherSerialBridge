# release_gui_self_test (WS-13, INV-88): the packaged GUI, after real initialisation, has loaded only System32/WinSxS
# modules and itself.
#
#   pwsh packaging/windows/release-gui-self-test.ps1 (-Zip <release zip> | -Tree <folder>) -Plant <asb_marker_plant.dll>
#        -Work <empty scratch folder> [-Dumpbin <dumpbin.exe>] [-PluginPlantsOnly]
#
# The tree (the release zip unzipped into <Work>\tree, or a copy of -Tree) is planted with:
#   - a marker DLL under every name in AetherSerialBridge.exe's import and delay-load tables (dumpbin /dependents);
#   - plugins\platforms\qwindows.dll (the marker) and a qt.conf beside the exe that points Qt's plugin path there;
#   - QT_PLUGIN_PATH / QT_QPA_PLATFORM_PLUGIN_PATH aimed at the planted folder.
# The marker DLL leaves <dll>.loaded beside itself when any process maps it. Then `AetherSerialBridge.exe
# --self-test-init` builds the window (real qwindows platform, real paint), waits for expose and the first paint and
# prints a JSON report with every loaded module (EnumProcessModulesEx). Accepted only when: exit 0, a report,
# exposed AND painted, every module is the exe itself or under %SystemRoot%\System32 or %SystemRoot%\WinSxS, and no
# marker exists. Each offence is printed as "NON-SYSTEM <path>" or "LOADED-PLANT <path>".
# Exit 0 = accepted, 1 = rejected, 2 = could not run.
#
# -PluginPlantsOnly skips the import-table plants: ci.yml's INV-88 control runs this script on the DYNAMIC GUI folder
# (a GUI whose qwindows is a dynamic plugin, the owned mutant), which has no dependent-load flag, so an import plant
# would stop it from starting at all and the control would prove nothing about the plugin.
param(
    [string]$Zip,
    [string]$Tree,
    [Parameter(Mandatory = $true)][string]$Plant,
    [Parameter(Mandatory = $true)][string]$Work,
    [string]$Dumpbin = 'dumpbin.exe',
    [switch]$PluginPlantsOnly
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if (Test-Path $Work) { Remove-Item -Recurse -Force $Work }
$d = (New-Item -ItemType Directory -Force "$Work\tree").FullName
if ($Zip) { Expand-Archive -Path $Zip -DestinationPath $d }
elseif ($Tree) { Copy-Item -Recurse "$Tree\*" $d }
else { Write-Host 'give -Zip or -Tree'; exit 2 }
$exe = "$d\AetherSerialBridge.exe"
if (-not (Test-Path $exe)) { Write-Host "no AetherSerialBridge.exe in the tree"; exit 2 }

# Every DLL the import and delay-load tables name.
$deps = & $Dumpbin /nologo /dependents $exe
if ($LASTEXITCODE -ne 0) { Write-Host "dumpbin failed"; exit 2 }
$names = @($deps | Select-String -Pattern '^\s+([A-Za-z0-9_.+-]+\.dll)\s*$' | ForEach-Object { $_.Matches[0].Groups[1].Value } |
           Sort-Object -Unique)
if ($names.Count -eq 0) { Write-Host 'dumpbin listed no DLL: the plant set would be empty'; exit 2 }
$planted = @()
foreach ($n in $names) {
    if ($PluginPlantsOnly) { break }
    $target = "$d\$n"
    if (Test-Path $target) { continue }   # never overwrite what the tree ships (a dynamic tree ships its Qt DLLs)
    Copy-Item $Plant $target
    $planted += $target
}
New-Item -ItemType Directory -Force "$d\plugins\platforms" | Out-Null
Copy-Item $Plant "$d\plugins\platforms\qwindows.dll"
$planted += "$d\plugins\platforms\qwindows.dll"
Set-Content -Encoding ascii "$d\qt.conf" "[Paths]`r`nPrefix = .`r`nPlugins = plugins`r`n"
Write-Host "planted $($planted.Count) DLLs: $(($planted | ForEach-Object { $_.Substring($d.Length + 1) }) -join ', ') and qt.conf"

$out = "$Work\self-test.out"
$err = "$Work\self-test.err"
$psi = [Diagnostics.ProcessStartInfo]::new($exe, '--self-test-init')
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.WorkingDirectory = $d
$psi.Environment['QT_PLUGIN_PATH'] = "$d\plugins"
$psi.Environment['QT_QPA_PLATFORM_PLUGIN_PATH'] = "$d\plugins\platforms"
$psi.Environment.Remove('QT_QPA_PLATFORM') | Out-Null   # the real platform plugin, never offscreen
$p = [Diagnostics.Process]::Start($psi)
$stdout = $p.StandardOutput.ReadToEndAsync()
$stderr = $p.StandardError.ReadToEndAsync()
if (-not $p.WaitForExit(60000)) {
    $p.Kill()
    Write-Host 'the self-test did not finish in 60 s; killed'
    exit 1
}
$rc = $p.ExitCode
$stdout.Result | Set-Content $out
$stderr.Result | Set-Content $err
if ($stderr.Result) { Write-Host "stderr: $($stderr.Result)" }

$bad = @()
$json = $stdout.Result.Trim()
$r = $null
if ($json) { try { $r = $json | ConvertFrom-Json } catch { $r = $null } }
if (-not $r) {
    $bad += "NO-REPORT exit $rc"
} else {
    if ($rc -ne 0) { $bad += "EXIT $rc" }
    if (-not $r.exposed) { $bad += 'NOT-EXPOSED' }
    if (-not $r.painted) { $bad += 'NOT-PAINTED' }
    $sys = [IO.Path]::GetFullPath("$env:SystemRoot\System32").TrimEnd('\') + '\'
    $sxs = [IO.Path]::GetFullPath("$env:SystemRoot\WinSxS").TrimEnd('\') + '\'
    $self = [IO.Path]::GetFullPath($exe)
    Write-Host "self-test exit $rc; exposed=$($r.exposed) painted=$($r.painted); $(@($r.modules).Count) modules"
    foreach ($m in @($r.modules)) {
        $full = [IO.Path]::GetFullPath($m)
        $ok = $full.Equals($self, [StringComparison]::OrdinalIgnoreCase) -or
              $full.StartsWith($sys, [StringComparison]::OrdinalIgnoreCase) -or
              $full.StartsWith($sxs, [StringComparison]::OrdinalIgnoreCase)
        if (-not $ok) { $bad += "NON-SYSTEM $full" }
    }
    if (@($r.modules).Count -lt 5) { $bad += "TOO-FEW-MODULES $(@($r.modules).Count)" }   # an empty list proves nothing
}
foreach ($m in Get-ChildItem -Recurse -Filter '*.loaded' $d -ErrorAction SilentlyContinue) {
    $bad += "LOADED-PLANT $($m.FullName.Substring(0, $m.FullName.Length - '.loaded'.Length))"
}
if ($bad.Count -gt 0) {
    Write-Host 'release_gui_self_test: REJECTED'
    $bad | ForEach-Object { Write-Host "  $_" }
    exit 1
}
Write-Host 'release_gui_self_test: OK (System32/WinSxS modules and the exe only; no plant loaded; exposed and painted)'
exit 0
