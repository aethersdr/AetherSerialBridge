# The Windows deployed-tree test (WS-13): install from the UNZIPPED release tree, then apply, status and the
# installed folder's exact contents.
#
#   pwsh packaging/windows/deployed-tree-test.ps1 -Zip <aether-serial-bridge-windows-x64.zip> -Work <empty folder>
#
# Why (2026-10-16, a production station): a Save & apply installed a development exe into Program Files beside DLLs
# from another build. The release exes are static, and the install copies exactly one file: this test proves that on
# the shipped bytes. After --install, --add-port, --apply and --status, %ProgramFiles%\AetherSerialBridge must hold
# exactly the installed program and the three lock files (no DLL, no plugins, no qt.conf, no temp), the installed
# program must be byte-identical to the zip's aether-serial-bridge.exe, and the service must run it.
# The Apps & features entry (WS-16) lives in the registry, not in the folder; --install writes it, and --uninstall
# removes it together with the whole program folder.
#
# Runs on an administrator runner with no installation present (a fresh hosted runner); it refuses to start
# otherwise, and it removes what it created (service, firewall rules through --uninstall, both folders).
param(
    [Parameter(Mandatory = $true)][string]$Zip,
    [Parameter(Mandatory = $true)][string]$Work
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$pf = "$env:ProgramFiles\AetherSerialBridge"
$pd = "$env:ProgramData\AetherSerialBridge"
$expected = @('aether-serial-bridge.exe', 'apply.lock', 'engine.lock', 'engine.lock.id')
$entry = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\AetherSerialBridge'   # 64-bit pwsh: the 64-bit view
$failures = [Collections.Generic.List[string]]::new()
function Fail([string]$m) { Write-Host "FAIL $m"; $failures.Add($m) }

if ((Test-Path $pf) -or (Test-Path $pd) -or (Get-Service AetherSerialBridge -ErrorAction SilentlyContinue)) {
    Write-Host "an installation is already present ($pf, $pd or the service): refusing to run"
    exit 2
}

if (Test-Path $Work) { Remove-Item -Recurse -Force $Work }
$tree = (New-Item -ItemType Directory -Force "$Work\tree").FullName
Expand-Archive -Path $Zip -DestinationPath $tree
$cli = "$tree\aether-serial-bridge.exe"
$shipped = (Get-FileHash -Algorithm SHA256 $cli).Hash

function Invoke-Asb([string[]]$a) {
    $out = & $cli @a 2>&1 | Out-String
    $rc = $LASTEXITCODE
    Write-Host "$ aether-serial-bridge $($a -join ' ') -> exit $rc"
    Write-Host $out.TrimEnd()
    return [pscustomobject]@{ rc = $rc; out = $out }
}

function Check-Folder([string]$when) {
    $items = @(Get-ChildItem -Force -Recurse $pf -ErrorAction SilentlyContinue)
    $names = @($items | ForEach-Object { $_.FullName.Substring($pf.Length + 1) } | Sort-Object)
    Write-Host "$pf ($when): $($names -join ', ')"
    $want = @($expected | Sort-Object)
    if (($names -join '|') -ne ($want -join '|')) { Fail "$when`: the installed folder holds [$($names -join ', ')], expected exactly [$($want -join ', ')]" }
    foreach ($i in $items) {
        if ($i.Name -match '\.(dll|conf)$' -or $i.PSIsContainer) { Fail "$when`: $($i.FullName) must not be in the installed folder" }
    }
    if (Test-Path "$pf\aether-serial-bridge.exe") {
        $h = (Get-FileHash -Algorithm SHA256 "$pf\aether-serial-bridge.exe").Hash
        if ($h -ne $shipped) { Fail "$when`: the installed program ($h) is not the zip's aether-serial-bridge.exe ($shipped)" }
    }
}

try {
    $r = Invoke-Asb @('--install')
    if ($r.rc -ne 0) { Fail "--install exited $($r.rc)" }
    Check-Folder 'after --install'
    if (-not (Test-Path $entry)) { Fail 'no Apps & features entry after --install' }
    $svc = Get-CimInstance Win32_Service -Filter "Name='AetherSerialBridge'"
    if (-not $svc) { Fail 'no AetherSerialBridge service after --install' }
    elseif ($svc.PathName -ne "`"$pf\aether-serial-bridge.exe`" --service") { Fail "service binPath is $($svc.PathName)" }

    $r = Invoke-Asb @('--add-port', 'Deployed', 'COM250', '45231', '--bind', '127.0.0.1')
    if ($r.rc -ne 0) { Fail "--add-port exited $($r.rc)" }
    $r = Invoke-Asb @('--apply')
    if ($r.rc -ne 0) { Fail "--apply exited $($r.rc)" }
    $r = Invoke-Asb @('--status')
    if ($r.rc -ne 0) { Fail "--status exited $($r.rc)" }
    if ($r.out -notmatch 'Service: running') { Fail '--status does not say the service is running' }
    if ($r.out -notmatch '127\.0\.0\.1:45231') { Fail '--status does not list the port' }
    Check-Folder 'after --apply and --status'

    # The process the service runs is the installed program.
    $svc = Get-CimInstance Win32_Service -Filter "Name='AetherSerialBridge'"
    if ($svc -and $svc.ProcessId) {
        $proc = Get-CimInstance Win32_Process -Filter "ProcessId=$($svc.ProcessId)"
        if ($proc.ExecutablePath -ne "$pf\aether-serial-bridge.exe") { Fail "the service runs $($proc.ExecutablePath)" }
        try {
            $mods = @((Get-Process -Id $svc.ProcessId).Modules | ForEach-Object { $_.FileName })
        } catch {
            $mods = @()
            Fail "could not read the service process's modules: $($_.Exception.Message)"
        }
        $foreign = @($mods | Where-Object { $_ -like "$pf\*" -and $_ -ne "$pf\aether-serial-bridge.exe" })
        if ($foreign.Count -gt 0) { Fail "the service loaded files from its folder: $($foreign -join ', ')" }
        Write-Host "service pid $($svc.ProcessId): $($mods.Count) modules, none from $pf but the program itself"
    } else {
        Fail 'the service has no process after --apply'
    }

    # v1.1 station fix (2026-10-18): a real station's config shape on the shipped bytes. Three more ports, names
    # with spaces, bound to one of this machine's own addresses (the station: its VPN address), anyone allowed (no
    # --allow). The service must still be running 12 s after the apply (the station's exited 7 about 3 s after every
    # start) and list every port Listening. The slow firewall check itself is the seam build's row
    # (tst_ServiceNative::station_shape_slow_check_listens); this one covers names, bind and allowed list as shipped.
    # The address of the interface that holds the default route (a real NIC, not a VMware/Hyper-V virtual adapter),
    # Else the first preferred non-loopback, non-link-local IPv4.
    $bind = @(Get-NetIPConfiguration -ErrorAction SilentlyContinue | Where-Object { $_.IPv4DefaultGateway -and $_.IPv4Address } |
        ForEach-Object { $_.IPv4Address.IPAddress })[0]
    if (-not $bind) {
        $bind = @(Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue |
            Where-Object { $_.IPAddress -notlike '127.*' -and $_.IPAddress -notlike '169.254.*' -and $_.AddressState -eq 'Preferred' } |
            Select-Object -ExpandProperty IPAddress)[0]
    }
    Write-Host "station shape: binding to $bind"
    if (-not $bind) { Fail 'station shape: this machine has no IPv4 address besides loopback and link-local' }
    else {
        $station = @(@('Elecraft KXPA100', 'COM251', 45232), @('Elecraft KAT500', 'COM252', 45233), @('Elecraft KPA100', 'COM253', 45234))
        foreach ($s in $station) {
            $r = Invoke-Asb @('--add-port', $s[0], $s[1], "$($s[2])", '--bind', $bind)
            if ($r.rc -ne 0) { Fail "station shape: --add-port $($s[0]) exited $($r.rc)" }
        }
        $r = Invoke-Asb @('--apply')
        if ($r.rc -ne 0) { Fail "station shape: --apply exited $($r.rc)" }
        $before = (Get-CimInstance Win32_Service -Filter "Name='AetherSerialBridge'").ProcessId
        Start-Sleep -Seconds 12
        $svc = Get-CimInstance Win32_Service -Filter "Name='AetherSerialBridge'"
        if ($svc.State -ne 'Running' -or $svc.ProcessId -ne $before) {
            Fail "station shape: the service did not stay up (state $($svc.State), pid $before then $($svc.ProcessId), exit $($svc.ExitCode))"
        }
        $r = Invoke-Asb @('--status')
        foreach ($s in $station) {
            if ($r.out -notmatch ([regex]::Escape("$($s[0])") + '\s+' + $s[1] + '\s+' + [regex]::Escape("${bind}:$($s[2])") + '\s+Listening')) {
                Fail "station shape: --status does not list $($s[0]) on ${bind}:$($s[2]) as Listening"
            }
        }
        if ($failures.Count -gt 0) { Write-Host $r.out; Get-Content "$pd\bridge.log" -Tail 20 -ErrorAction SilentlyContinue }
        Check-Folder 'after the station-shape apply'
    }
} finally {
    $r = Invoke-Asb @('--uninstall')
    if ($r.rc -ne 0) { Fail "--uninstall exited $($r.rc)" }
    $t = [Diagnostics.Stopwatch]::StartNew()
    while ((Get-Service AetherSerialBridge -ErrorAction SilentlyContinue) -and $t.Elapsed.TotalSeconds -lt 60) { Start-Sleep -Milliseconds 250 }
    if (Get-Service AetherSerialBridge -ErrorAction SilentlyContinue) { Fail 'the service was not removed' }
    # WS-16: the uninstall removes the program folder itself (run from the unzipped tree, nothing in it is in use).
    if (Test-Path $pf) { Fail "$pf is still there after --uninstall: $(@(Get-ChildItem -Force $pf | ForEach-Object Name) -join ', ')" }
    if (Test-Path $entry) { Fail 'the Apps & features entry is still there after --uninstall' }
    Remove-Item -Recurse -Force $pf, $pd -ErrorAction SilentlyContinue
    Remove-Item -Recurse -Force $entry -ErrorAction SilentlyContinue
}

if ($failures.Count -gt 0) {
    Write-Host "deployed-tree test: $($failures.Count) failure(s)"
    exit 1
}
Write-Host 'deployed-tree test: OK (the unzipped tree installs exactly one file, which the service runs)'
exit 0
