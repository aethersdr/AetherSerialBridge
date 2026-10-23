# activation_matrix, Windows half (INV-99; cases owned by WS-5, run on the release executables by WS-13).
#
#   pwsh packaging/windows/activation-matrix.ps1 -Zip <release zip> -Qual <asb_win_qual.exe> -Work <empty folder>
#
# Config A: port 4001 -> COM3, allow 10.0.0.5. Config B: port 4001 -> COM4, allow 0.0.0.0/0. For each point the
# release CLI (unzipped) installs and applies A, then the point is produced, then every attempt runs the RELEASE
# executables:
#   (1) SCM start (sc.exe start), then managed recovery (--start): if the service runs, its status ACK digest equals
#       config.json's and the firewall rules match the committed config (the INV-117 comparator, asb_win_qual fw-check);
#   (2) --run --config <copy of B> from a non-installed copy: the listener is on 127.0.0.1 only and a connection to
#       a non-loopback address of this machine fails;
#   (3) --run --config <copy of B> from the installed program: refused (exit 2, "installed service program");
#   untick/remove points: config.json is A's bytes and config.pending.json B's.
# Points:
#   rename_fail      the release CLI's --apply of B while config.json is held open without delete sharing: the sync
#                    succeeds and the rename over config.json fails (an OS-level fault on the shipped bytes);
#   remove_fails     SaveAndRemove of B whose firewall removal fails (asb_win_qual remove-fail, the test harness);
#   remove           SaveAndRemove of B, the Windows untick (asb_win_qual remove; Linux's untick is Deactivate);
#   after_r9         the release CLI's --apply of B completed (the state a crash after R9 leaves: committed, rules
#                    synced, started).
# The harness only produces points the shipped CLI cannot reach; no attempt runs it except fw-check (a read).
# Exit 0 = every assertion held. Removes everything it created.
param(
    [Parameter(Mandatory = $true)][string]$Zip,
    [Parameter(Mandatory = $true)][string]$Qual,
    [Parameter(Mandatory = $true)][string]$Work
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$pf = "$env:ProgramFiles\AetherSerialBridge"
$pd = "$env:ProgramData\AetherSerialBridge"
$installed = "$pf\aether-serial-bridge.exe"
$failures = [Collections.Generic.List[string]]::new()
$point = ''
function Fail([string]$m) { Write-Host "FAIL [$point] $m"; $failures.Add("[$point] $m") }

if ((Test-Path $pf) -or (Test-Path $pd) -or (Get-Service AetherSerialBridge -ErrorAction SilentlyContinue)) {
    Write-Host "an installation is already present: refusing to run"
    exit 2
}
if (Test-Path $Work) { Remove-Item -Recurse -Force $Work }
$tree = (New-Item -ItemType Directory -Force "$Work\tree").FullName
Expand-Archive -Path $Zip -DestinationPath $tree
$cli = "$tree\aether-serial-bridge.exe"
New-Item -ItemType Directory -Force "$Work\copy" | Out-Null
$copy = "$Work\copy\aether-serial-bridge.exe"
Copy-Item $cli $copy

$bJson = '{"version":1,"ports":[{"enabled":true,"name":"mx","comPort":"COM4","baudRate":38400,"tcpPort":4001,"bindAddress":"0.0.0.0","allowedClients":"0.0.0.0/0"}]}'
$bFile = "$Work\b.json"
[IO.File]::WriteAllText($bFile, $bJson)
# The scratch config of attempt (2)/(3) sits in a folder only this account, SYSTEM and Administrators can change
# (scratch trust: the runner's temp folder grants more), owned by Administrators.
$scratch = (New-Item -ItemType Directory -Force "$Work\scratch").FullName
& icacls.exe $scratch /inheritance:r /grant:r "*S-1-5-32-544:(OI)(CI)F" "*S-1-5-18:(OI)(CI)F" "$([Security.Principal.WindowsIdentity]::GetCurrent().Name):(OI)(CI)F" | Out-Null
& icacls.exe $scratch /setowner "*S-1-5-32-544" | Out-Null
$bCopy = "$scratch\b-copy.json"
Copy-Item $bFile $bCopy

function Run([string]$exe, [string[]]$a) {
    $out = & $exe @a 2>&1 | Out-String
    $rc = $LASTEXITCODE
    Write-Host "$ $([IO.Path]::GetFileName($exe)) $($a -join ' ') -> exit $rc"
    if ($out.Trim()) { Write-Host $out.TrimEnd() }
    return [pscustomobject]@{ rc = $rc; out = $out }
}
function Invoke-Asb([string[]]$a) { Run $cli $a }
function Bytes([string]$p) { if (Test-Path $p) { [IO.File]::ReadAllBytes($p) } else { $null } }
function Same($a, $b) { ($null -ne $a) -and ($null -ne $b) -and ([Convert]::ToBase64String($a) -eq [Convert]::ToBase64String($b)) }
function Sha([byte[]]$b) { -join ([Security.Cryptography.SHA256]::Create().ComputeHash($b) | ForEach-Object { $_.ToString('x2') }) }

function Show-Diagnostics([string]$why) {
    Write-Host "  --- diagnostics ($why) ---"
    $svc = Get-CimInstance Win32_Service -Filter "Name='AetherSerialBridge'"
    if ($svc) { Write-Host "  service: $($svc.State) exit=$($svc.ExitCode) specific=$($svc.ServiceSpecificExitCode) pid=$($svc.ProcessId)" }
    if (Test-Path "$pd\status.json") { Write-Host "  status.json: $(Get-Content -Raw "$pd\status.json")" }
    if (Test-Path "$pd\bridge.log") { Get-Content "$pd\bridge.log" -Tail 20 | ForEach-Object { Write-Host "  log: $_" } }
    Write-Host "  serial ports on this machine: $([IO.Ports.SerialPort]::GetPortNames() -join ', ')"
    if (Test-Path $installed) { $fw = Run $Qual @('--exe', $installed, 'fw-check') }
    Get-NetFirewallRule -Group 'Aether Serial Bridge' -ErrorAction SilentlyContinue | ForEach-Object {
        $a = $_ | Get-NetFirewallAddressFilter
        Write-Host "  rule $($_.Name): RemoteAddress=[$($a.RemoteAddress -join ', ')] (ActiveStore)"
    }
    Get-NetFirewallRule -Group 'Aether Serial Bridge' -PolicyStore PersistentStore -ErrorAction SilentlyContinue | ForEach-Object {
        $a = $_ | Get-NetFirewallAddressFilter
        Write-Host "  rule $($_.Name): RemoteAddress=[$($a.RemoteAddress -join ', ')] (PersistentStore)"
    }
}

function Wait-Gone {
    $t = [Diagnostics.Stopwatch]::StartNew()
    while ((Get-Service AetherSerialBridge -ErrorAction SilentlyContinue) -and $t.Elapsed.TotalSeconds -lt 60) { Start-Sleep -Milliseconds 250 }
}
function Reset {
    if (Get-Service AetherSerialBridge -ErrorAction SilentlyContinue) { Invoke-Asb @('--uninstall') | Out-Null; Wait-Gone }
    if (Get-Service AetherSerialBridge -ErrorAction SilentlyContinue) { sc.exe delete AetherSerialBridge | Out-Null; Wait-Gone }
    Remove-Item -Recurse -Force $pf, $pd -ErrorAction SilentlyContinue
}
function Setup-A {
    if ((Invoke-Asb @('--install')).rc -ne 0) { Fail '--install' }
    if ((Invoke-Asb @('--add-port', 'mx', 'COM3', '4001', '--allow', '10.0.0.5')).rc -ne 0) { Fail '--add-port A' }
    if ((Invoke-Asb @('--apply')).rc -ne 0) { Fail '--apply A' }
    $a = Bytes "$pd\config.json"
    if (-not $a) { Fail 'no config.json after applying A' }
    return ,$a
}
function Pending-B {
    if ((Invoke-Asb @('--remove-port', 'mx')).rc -ne 0) { Fail '--remove-port' }
    if ((Invoke-Asb @('--add-port', 'mx', 'COM4', '4001', '--allow', '0.0.0.0/0')).rc -ne 0) { Fail '--add-port B' }
    $b = Bytes "$pd\config.pending.json"
    if (-not $b) { Fail 'no config.pending.json after the edits' }
    return ,$b
}

function Check-Running([string]$how, [bool]$mustRun = $false) {
    $svc = Get-CimInstance Win32_Service -Filter "Name='AetherSerialBridge'"
    if (-not $svc -or $svc.State -ne 'Running') {
        if ($mustRun) { Fail "($how) the committed, synced config does not run"; Show-Diagnostics $how; return }
        Write-Host "  ($how) not running: state $(if ($svc) { "$($svc.State), exit $($svc.ExitCode)/$($svc.ServiceSpecificExitCode)" } else { 'no service' }) - refused, as allowed"
        return
    }
    $want = Sha (Bytes "$pd\config.json")
    $ack = $null
    $t = [Diagnostics.Stopwatch]::StartNew()
    while ($t.Elapsed.TotalSeconds -lt 20) {
        try { $ack = (Get-Content -Raw "$pd\status.json" | ConvertFrom-Json).configSha256 } catch { $ack = $null }
        if ($ack) { break }
        $svc = Get-CimInstance Win32_Service -Filter "Name='AetherSerialBridge'"
        if ($svc.State -ne 'Running') { Write-Host "  ($how) stopped before its ACK (exit $($svc.ExitCode)/$($svc.ServiceSpecificExitCode)) - refused"; return }
        Start-Sleep -Milliseconds 250
    }
    if ($ack -ne $want) { Fail "($how) the running service's ACK digest $ack is not config.json's $want" }
    $fw = Run $Qual @('--exe', $installed, 'fw-check')
    if ($fw.out -notmatch 'RESULT ok fw-check verdict=Match') { Fail "($how) the service runs but the firewall rules do not match the committed config" }
    else { Write-Host "  ($how) running config.json ($want) under rules synced from it" }
}

function Attempts([bool]$untick, $aBytes, $bBytes, [bool]$mayRun = $false) {
    # (3) --run from the installed program: refused before anything else.
    if (Test-Path $installed) {
        $r = Run $installed @('--run', '--config', $bCopy)
        if ($r.rc -ne 2 -or $r.out -notmatch 'installed service program') { Fail "(3) the installed program's --run was not refused (exit $($r.rc))" }
    }
    # (2) --run from a non-installed copy: loopback only. The service must not hold 4001 meanwhile.
    $s = Get-Service AetherSerialBridge -ErrorAction SilentlyContinue
    if ($s -and $s.Status -eq 'Running') { sc.exe stop AetherSerialBridge | Out-Null; Start-Sleep 3 }
    $p = Start-Process -FilePath $copy -ArgumentList @('--run', '--config', $bCopy) -PassThru -NoNewWindow `
         -RedirectStandardOutput "$Work\run.out" -RedirectStandardError "$Work\run.err"
    try {
        $listen = @()
        $t = [Diagnostics.Stopwatch]::StartNew()
        while ($t.Elapsed.TotalSeconds -lt 20 -and $listen.Count -eq 0) {
            Start-Sleep -Milliseconds 300
            $listen = @(Get-NetTCPConnection -State Listen -LocalPort 4001 -ErrorAction SilentlyContinue | Where-Object { $_.OwningProcess -eq $p.Id })
        }
        if ($listen.Count -eq 0) {
            Fail '(2) the scratch run never listened on 4001'
            $p.Refresh()
            Write-Host "  scratch run: exited=$($p.HasExited) $(if ($p.HasExited) { "code $($p.ExitCode)" })"
            Get-Content "$Work\run.out", "$Work\run.err" -ErrorAction SilentlyContinue | ForEach-Object { Write-Host "  run: $_" }
            Get-NetTCPConnection -State Listen -LocalPort 4001 -ErrorAction SilentlyContinue | ForEach-Object { Write-Host "  4001 held by pid $($_.OwningProcess) on $($_.LocalAddress)" }
        }
        $addrs = @($listen | ForEach-Object { $_.LocalAddress } | Sort-Object -Unique)
        if ($listen.Count -gt 0 -and ($addrs.Count -ne 1 -or $addrs[0] -ne '127.0.0.1')) { Fail "(2) the scratch run listens on $($addrs -join ', '), not 127.0.0.1 only" }
        $lan = @(Get-NetIPAddress -AddressFamily IPv4 | Where-Object { $_.IPAddress -notlike '127.*' -and $_.IPAddress -notlike '169.254.*' } | Select-Object -First 1)
        if ($lan.Count -eq 1) {
            $c = [Net.Sockets.TcpClient]::new()
            $ok = $false
            try { $ok = $c.ConnectAsync($lan[0].IPAddress, 4001).Wait(3000) -and $c.Connected } catch { $ok = $false } finally { $c.Dispose() }
            if ($ok) { Fail "(2) a connection to $($lan[0].IPAddress):4001 succeeded" }
            else { Write-Host "  (2) listener 127.0.0.1:4001 only; $($lan[0].IPAddress):4001 refused" }
        }
    } finally {
        Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
        $p.WaitForExit(10000) | Out-Null
    }
    # Untick/remove points: committed = A, pending = B (before any recovery could touch them).
    if ($untick) {
        if (-not (Same (Bytes "$pd\config.json") $aBytes)) { Fail 'committed config.json is not A' }
        if (-not (Same (Bytes "$pd\config.pending.json") $bBytes)) { Fail 'config.pending.json is not B' }
    }
    # (1) SCM start, then managed recovery.
    if (Get-Service AetherSerialBridge -ErrorAction SilentlyContinue) {
        sc.exe start AetherSerialBridge | Out-Null
        Start-Sleep 5
        Check-Running 'sc start'
        Invoke-Asb @('--start') | Out-Null
        Check-Running '--start' $mayRun
    } else {
        Write-Host '  (1) no service to start - refused, as allowed'
    }
}

try {
    # --- rename_fail ---
    $point = 'rename_fail'
    Reset; $a = Setup-A; $b = Pending-B
    $hold = [IO.File]::Open("$pd\config.json", 'Open', 'Read', 'ReadWrite')   # no FILE_SHARE_DELETE: rename over it fails
    try { $r = Invoke-Asb @('--apply') } finally { $hold.Dispose() }
    if ($r.rc -eq 0) { Fail '--apply succeeded although config.json could not be replaced' }
    if (-not (Same (Bytes "$pd\config.json") $a)) { Fail 'config.json changed although the rename failed' }
    Attempts $false $a $b

    # --- remove_fails ---
    $point = 'remove_fails'
    Reset; $a = Setup-A
    $r = Run $Qual @('--exe', $cli, 'remove-fail', '--config', $bFile)
    if ($r.rc -ne 0) { Fail "asb_win_qual remove-fail: $($r.out)" }
    Attempts $true $a ([IO.File]::ReadAllBytes($bFile))

    # --- remove (the Windows untick) ---
    $point = 'remove'
    Reset; $a = Setup-A
    $r = Run $Qual @('--exe', $cli, 'remove', '--config', $bFile)
    if ($r.rc -ne 0) { Fail "asb_win_qual remove: $($r.out)" }
    Attempts $true $a ([IO.File]::ReadAllBytes($bFile))

    # --- after_r9 ---
    $point = 'after_r9'
    Reset; $a = Setup-A; $b = Pending-B
    if ((Invoke-Asb @('--apply')).rc -ne 0) { Fail '--apply B'; Show-Diagnostics 'apply B' }
    if (-not (Same (Bytes "$pd\config.json") $b)) { Fail 'config.json is not B after a successful apply' }
    Attempts $false $a $b $true
} finally {
    $point = 'cleanup'
    Reset
}

if ($failures.Count -gt 0) {
    Write-Host "activation_matrix (Windows): $($failures.Count) failure(s)"
    $failures | ForEach-Object { Write-Host "  $_" }
    exit 1
}
Write-Host 'activation_matrix (Windows): OK'
exit 0
