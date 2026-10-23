<#
.SYNOPSIS
    WS-7 station-PC qualification of the Aether Serial Bridge (master §8.2 HW 4, 8, 10; plus the
    firewall keyword pin and a real-COM-port smoke test). Non-interactive; run elevated (Windows PowerShell 5.1 or 7).

.DESCRIPTION
    Builds nothing. -Artifacts is a folder from the CI "windows" leg (artifact "ws7-station-artifacts"):
    aether-serial-bridge.exe, asb_win_qual.exe (the qualification harness: the real Applier over the real
    Windows Platform, test builds only), and runtime\ (the Qt and MSVC runtime DLLs of a dynamic-Qt test build).

    A service "AetherSerialBridge" this program did not install is never modified, started, stopped or uninstalled:
    the script refuses to run while one exists.
    Any existing %ProgramFiles%\AetherSerialBridge or %ProgramData%\AetherSerialBridge is renamed aside before the
    install and restored afterwards (so a config.json left there is never touched), and the Qt service is installed,
    exercised, and removed again with its rules and files.

    Phases (HW 8 needs reboots the operator performs):
      -Phase Run                 preconditions, install, firewall check, keyword pin, HW 10, serial smoke,
                                 HW 8 part 1 (auto-start configured). Then: reboot, run -Phase AfterReboot.
      -Phase AfterReboot         HW 8a: the service came back and listens; then a failed apply (forced firewall
                                 failure) leaves it disabled. Then: reboot, run -Phase AfterSecondReboot.
      -Phase AfterSecondReboot   HW 8b: the bridge stayed down; then the cleanup.
      -Phase Cleanup             removes everything this script installed and restores what it moved aside: the Qt
                                 service only as the product's detection identifies it, a folder only while it is
                                 still the one this run created (or moved aside). If the installation is now
                                 foreign or cannot be detected, it touches nothing, says so in a CLEANUP FAIL line
                                 and keeps state.json for a later -Phase Cleanup.
    The state between phases is kept in -Evidence (default C:\asb-ws7-station), with every command's output.

    Each item prints "PASS|FAIL|SKIPPED|BLOCKED|PENDING  <item>  <evidence>"; a summary ends every phase.
    Exit code: 0 = no FAIL in this phase, 1 = a FAIL, 2 = refused (preconditions).

.PARAMETER Artifacts   Folder with aether-serial-bridge.exe, asb_win_qual.exe and runtime\.
.PARAMETER Phase       Run (default), AfterReboot, AfterSecondReboot, Cleanup. (SelfTest: checks the script's own status
                       parsing against sample status lines and exits; no precondition, nothing touched. CI runs it.)
.PARAMETER ComPort     A real serial port for the smoke test (e.g. COM5). Without it a placeholder port is configured
                       and the serial smoke test is SKIPPED.
.PARAMETER TcpPort     The TCP port of the test mapping (default 47021).
.PARAMETER Procmon     Path to Sysinternals Procmon64.exe for HW 10 (no CreateFile on \\.\COMn while listing ports).
                       Without it HW 10 is SKIPPED.
.PARAMETER Evidence    Folder for the state file and the evidence (default C:\asb-ws7-station).
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Artifacts,
    [ValidateSet('Run', 'AfterReboot', 'AfterSecondReboot', 'Cleanup', 'SelfTest')][string]$Phase = 'Run',
    [string]$ComPort = '',
    [int]$TcpPort = 47021,
    [string]$Procmon = '',
    [string]$Evidence = "$env:SystemDrive\asb-ws7-station"
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$ServiceName = 'AetherSerialBridge'
$Group = 'Aether Serial Bridge'
$LockDir = Join-Path $env:ProgramFiles 'AetherSerialBridge'
$DataDir = Join-Path $env:ProgramData 'AetherSerialBridge'
$StateFile = Join-Path $Evidence 'state.json'
$script:Results = New-Object System.Collections.ArrayList
$script:Failed = $false

function Say([string]$text) { Write-Host $text }

function Record([string]$status, [string]$item, [string]$evidence) {
    [void]$script:Results.Add([pscustomobject]@{ Status = $status; Item = $item; Evidence = $evidence })
    if ($status -eq 'FAIL') { $script:Failed = $true }
    Say ("{0,-8} {1,-34} {2}" -f $status, $item, $evidence)
}

function Summary {
    Say ''
    Say "===== WS-7 station qualification: phase $Phase ====="
    $script:Results | Format-Table -AutoSize Status, Item, Evidence | Out-String -Width 400 | Write-Host
    $script:Results | ConvertTo-Json | Set-Content -Encoding UTF8 (Join-Path $Evidence "results-$Phase.json")
    if ($script:Failed) { exit 1 } else { exit 0 }
}

function Load-State {
    if (Test-Path $StateFile) { return (Get-Content -Raw $StateFile | ConvertFrom-Json) }
    return $null
}
function Save-State($state) { $state | ConvertTo-Json -Depth 6 | Set-Content -Encoding UTF8 $StateFile }

# Runs the harness; returns @{ Code; Lines; Result } and keeps the output as evidence.
function Qual([string[]]$arguments, [string]$tag) {
    $exe = Join-Path $Artifacts 'asb_win_qual.exe'
    $runtime = Join-Path $Artifacts 'runtime'
    $all = @('--exe', (Join-Path $Artifacts 'aether-serial-bridge.exe'), '--runtime', $runtime) + $arguments
    # Native stderr must not stop the script (Windows PowerShell 5.1 turns redirected stderr lines into errors).
    $saved = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $out = @(& $exe @all 2>&1 | ForEach-Object { "$_" })
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $saved
    }
    $out | Set-Content -Encoding UTF8 (Join-Path $Evidence "qual-$tag.txt")
    $result = ($out | Where-Object { $_ -like 'RESULT *' } | Select-Object -Last 1)
    if (-not $result) { $result = '(no RESULT line)' }
    return @{ Code = $code; Lines = $out; Result = $result }
}

function Service-Info {
    $svc = Get-CimInstance Win32_Service -Filter "Name='$ServiceName'" -ErrorAction SilentlyContinue
    if (-not $svc) { return $null }
    return [pscustomobject]@{ State = $svc.State; StartMode = $svc.StartMode; PathName = $svc.PathName; ProcessId = $svc.ProcessId }
}

# The non-empty items of a value as an array (a function returning nothing gives $null, and @($null) is one item).
function Items($x) { return ,@(@($x) | Where-Object { $null -ne $_ -and "$_" -ne '' }) }

# The product's own installation detection: NotInstalled, IncompleteInstall, Current or Foreign.
function Detect([string]$tag) {
    $q = Qual @('detect') $tag
    if ($q.Result -match 'state=(\w+)') { return $Matches[1] }
    return 'Unknown'
}

function Group-Rules {
    return ,@(Get-NetFirewallRule -Group $Group -ErrorAction SilentlyContinue | ForEach-Object { $_.Name } | Sort-Object)
}

function Listening([int]$port) {
    return [bool](Get-NetTCPConnection -LocalPort $port -State Listen -ErrorAction SilentlyContinue)
}

function Write-Config([string]$com) {
    $config = @"
{
  "version": 1,
  "ports": [
    {
      "enabled": true,
      "name": "WS-7 station qualification",
      "comPort": "$com",
      "baudRate": 38400,
      "tcpPort": $TcpPort,
      "bindAddress": "0.0.0.0",
      "allowedClients": "127.0.0.1"
    }
  ]
}
"@
    $path = Join-Path $Evidence 'config.json'
    [System.IO.File]::WriteAllText($path, $config, (New-Object System.Text.UTF8Encoding($false)))
    return $path
}

# DeleteService only marks the service: SCM removes it once every handle to it is closed. Wait for that (bounded)
# instead of reading a marked service as "still installed".
function Wait-ServiceDeleted([int]$Seconds = 60) {
    $t = [Diagnostics.Stopwatch]::StartNew()
    while (Get-Service -Name $ServiceName -ErrorAction SilentlyContinue) {
        if ($t.Elapsed.TotalSeconds -ge $Seconds) { return $false }
        Start-Sleep -Milliseconds 250
    }
    return $true
}

# The identity a folder had when this run created it (or moved it aside): its creation time, recorded in the state.
# A folder with another creation time was made by someone else since (another install, an operator) and is not ours.
function Folder-Stamp([string]$path) {
    $item = Get-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
    if (-not $item) { return $null }
    # Ticks with a prefix: a plain ISO date would come back from ConvertFrom-Json (PowerShell 7) as a DateTime.
    return 'ticks:' + $item.CreationTimeUtc.Ticks
}
# Whether this run moved <name> aside, from the <name>Moved flag the Run saves after each rename. A state file of the
# round-1 script has no flags: it was saved only after both renames, so a recorded aside path means it moved.
function Moved-Flag($state, [string]$name) {
    if ($state -and ($state.PSObject.Properties.Name -contains "${name}Moved")) { return [bool]$state."${name}Moved" }
    return [bool](State-Value $state "${name}Aside")
}
function State-Value($state, [string]$name) {
    if ($state -and ($state.PSObject.Properties.Name -contains $name)) { return $state.$name }
    return $null
}

# Removes what this run installed and restores what it moved aside. Returns $true when the state file can go;
# $false keeps it (nothing destructive was done that a rerun could not finish). Always ends with a CLEANUP line.
function Cleanup($state) {
    try {
        # A state file from an earlier version of this script has no folder identities: nothing here can tell that
        # the folders are still the run's, so nothing is touched (finish that run with the script that started it).
        if ($state -and -not ($state.PSObject.Properties.Name -contains 'LockDirMade')) {
            Record 'FAIL' 'CLEANUP: nothing left behind' "$StateFile was written by an earlier version of ws7-station.ps1 (no folder identities): nothing was removed or restored. Run -Phase Cleanup with the artifacts that started that run."
            return $false
        }
        # The product's own detection decides (never a path match). Anything but the Qt service or nothing means the
        # installation changed since the Run phase: touch nothing, keep the state for a later -Phase Cleanup.
        $kind = Detect 'detect-cleanup'
        if ($kind -ne 'Current' -and $kind -ne 'IncompleteInstall' -and $kind -ne 'NotInstalled') {
            Record 'FAIL' 'CLEANUP: nothing left behind' "the installation is now '$kind' (not the Qt service this run installed): nothing was removed or restored; $StateFile is kept. Resolve it, then run -Phase Cleanup again."
            return $false
        }
        if ($kind -eq 'Current' -or $kind -eq 'IncompleteInstall') {
            # The installation is uninstalled only if it is still the one this run made: its install folder must be
            # the folder this run created (the same creation stamp). A Qt installation someone made since, in a folder
            # this run did not create, is left alone, and the state is kept (WS-7 merge note).
            if ($state) {
                $made = State-Value $state 'LockDirMade'
                if (-not $made -or -not (Test-Path -LiteralPath $LockDir) -or (Folder-Stamp $LockDir) -ne $made) {
                    Record 'FAIL' 'CLEANUP: nothing left behind' "the installation in $LockDir is not the one this run created (its folder is not the run's): nothing was uninstalled, removed or restored; $StateFile is kept"
                    return $false
                }
            }
            $q = Qual @('uninstall') 'uninstall'
            Say "uninstall: $($q.Result)"
            if (-not (Wait-ServiceDeleted 60)) { Say 'uninstall: SCM has not deleted the service after 60 s' }
            $after = Detect 'detect-after-uninstall'
            if ($after -ne 'NotInstalled') {
                # Never a direct sc delete: that would remove the service without the product's order (the firewall
                # rules first, then the service). The product's own uninstall said why; fix that and run Cleanup again.
                Record 'FAIL' 'CLEANUP: nothing left behind' "after the uninstall the installation is '$after' ($($q.Result)): no folder was removed or restored; $StateFile is kept. Fix the cause, then run -Phase Cleanup again."
                return $false
            }
        }
        $keep = $false
        $left = @()
        if ($state) {
            # The folders this run created go, only if they are still the ones it created; the ones moved aside come
            # back, only if they are still the ones it moved and the original place is free.
            foreach ($f in @(@{ Dir = $LockDir; Aside = (State-Value $state 'LockDirAside'); Made = (State-Value $state 'LockDirMade');
                                AsideStamp = (State-Value $state 'LockDirAsideStamp'); Moved = (Moved-Flag $state 'LockDir') },
                             @{ Dir = $DataDir; Aside = (State-Value $state 'DataDirAside'); Made = (State-Value $state 'DataDirMade');
                                AsideStamp = (State-Value $state 'DataDirAsideStamp'); Moved = (Moved-Flag $state 'DataDir') })) {
                $dir = $f.Dir
                if ($f.Aside -and -not $f.Moved) {
                    # Planned to move aside, never moved (a Run that stopped before that rename): the folder in place is
                    # the original, untouched. Its creation stamp only confirms it is the same folder.
                    if ((Test-Path -LiteralPath $dir) -and $f.AsideStamp -and (Folder-Stamp $dir) -ne $f.AsideStamp) {
                        $left += "$dir (planned to move aside but never moved, and no longer the same folder: left alone)"
                        $keep = $true
                    }
                    continue
                }
                if (Test-Path -LiteralPath $dir) {
                    $now = Folder-Stamp $dir
                    if (-not $f.Made -or $now -ne $f.Made) {
                        $left += "$dir (created $now, not by this run: left alone)"
                        $keep = $true
                        continue
                    }
                    # A file the stopped service held a moment ago can still be closing: retry for up to 10 s.
                    for ($i = 0; $i -lt 20 -and (Test-Path -LiteralPath $dir); $i++) {
                        Remove-Item -LiteralPath $dir -Recurse -Force -ErrorAction SilentlyContinue
                        if (Test-Path -LiteralPath $dir) { Start-Sleep -Milliseconds 500 }
                    }
                    if (Test-Path -LiteralPath $dir) {
                        $left += "$dir (could not be removed)"
                        $keep = $true
                        continue   # never rename the aside folder onto it
                    }
                }
                if ($f.Aside -and (Test-Path -LiteralPath $f.Aside)) {
                    if ($f.AsideStamp -and (Folder-Stamp $f.Aside) -ne $f.AsideStamp) {
                        $left += "$($f.Aside) (not the folder this run moved aside: not restored)"
                        $keep = $true
                        continue
                    }
                    Rename-Item -LiteralPath $f.Aside -NewName (Split-Path -Leaf $dir)
                } elseif ($f.Aside) {
                    $left += "$($f.Aside) (moved aside by this run, now missing)"
                    $keep = $true
                }
            }
        }
        $kind = Detect 'detect-final'
        if ($kind -eq 'Current' -or $kind -eq 'IncompleteInstall') { $left += 'the Qt service'; $keep = $true }
        $before = @()
        if ($state) { $before = Items $state.RulesBefore }
        $rules = Items (Items (Group-Rules) | Where-Object { $before -notcontains $_ })
        if ($rules.Count -gt 0) { $left += "firewall rules: $($rules -join ', ')"; $keep = $true }
        if ($left.Count -eq 0) {
            Record 'PASS' 'CLEANUP: nothing left behind' 'no Qt service, no new rules in the group, folders restored'
            return $true
        }
        $note = ''
        if ($keep) { $note = "; $StateFile is kept for another -Phase Cleanup" }
        Record 'FAIL' 'CLEANUP: nothing left behind' ("left: " + ($left -join '; ') + $note)
        return $false
    } catch {
        Record 'FAIL' 'CLEANUP: nothing left behind' "the cleanup stopped: $_; $StateFile is kept"
        return $false
    }
}

# The SERIAL smoke verdict from the harness's "STATUS <json>" line (the status.json the service writes): the port
# `com` must be Connected with no error and no byte written to it (the client sends nothing). StatusStore always
# writes the "lastError" key (null when there is none), so it is parsed, never matched as text (station run
# 2026-10-15). Returns $null for a pass, else the reason.
function Serial-Verdict([string]$statusLine, [string]$com) {
    if (-not $statusLine -or -not $statusLine.StartsWith('STATUS ')) { return "no STATUS line: $statusLine" }
    try { $status = $statusLine.Substring(7) | ConvertFrom-Json } catch { return "the status is not JSON: $statusLine" }
    if (-not ($status.PSObject.Properties.Name -contains 'ports')) { return "the status has no ports: $statusLine" }
    $port = @($status.ports | Where-Object { ($_.PSObject.Properties.Name -contains 'comPort') -and $_.comPort -eq $com }) | Select-Object -First 1
    if (-not $port) { return "no port $com in the status: $statusLine" }
    $names = $port.PSObject.Properties.Name
    if (-not ($names -contains 'state') -or $port.state -ne 'Connected') { return "$com is not Connected: $statusLine" }
    if (($names -contains 'lastError') -and $null -ne $port.lastError -and "$($port.lastError)" -ne '') { return "$com reports lastError '$($port.lastError)'" }
    if (-not ($names -contains 'bytesToSerial') -or [long]$port.bytesToSerial -ne 0) { return "$com received bytes (bytesToSerial must be 0): $statusLine" }
    return $null
}

if ($Phase -eq 'SelfTest') {
    $sample = 'STATUS {"configError":null,"ports":[{"bindAddress":"0.0.0.0","bytesToClient":0,"bytesToSerial":0,"client":"127.0.0.1:65281","comPort":"COM3","lastError":null,"name":"x","state":"Connected","tcpPort":47021}]}'
    $cases = @(
        @{ Name = 'the station status of 2026-10-15 (lastError null)'; Line = $sample; Pass = $true },
        @{ Name = 'no lastError key'; Line = $sample.Replace('"lastError":null,', ''); Pass = $true },
        @{ Name = 'an error'; Line = $sample.Replace('"lastError":null', '"lastError":"Access denied"'); Pass = $false },
        @{ Name = 'Listening, not Connected'; Line = $sample.Replace('"Connected"', '"Listening"'); Pass = $false },
        @{ Name = 'bytes written to the port'; Line = $sample.Replace('"bytesToSerial":0', '"bytesToSerial":5'); Pass = $false },
        @{ Name = 'another port'; Line = $sample.Replace('"COM3"', '"COM4"'); Pass = $false },
        @{ Name = 'not JSON'; Line = 'STATUS {not json'; Pass = $false },
        @{ Name = 'no STATUS line'; Line = ''; Pass = $false }
    )
    $bad = 0
    foreach ($c in $cases) {
        $why = Serial-Verdict $c.Line 'COM3'
        $passed = ($null -eq $why)
        if ($passed -eq $c.Pass) { Say "ok    $($c.Name)" } else { Say "FAIL  $($c.Name): $why"; $bad++ }
    }
    if ($bad -gt 0) { exit 1 } else { exit 0 }
}

# ------------------------------------------------------------------------------------------------------------
# Preconditions (every phase)
# ------------------------------------------------------------------------------------------------------------
$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Say 'REFUSED: run this script from an elevated PowerShell (Run as administrator).'
    exit 2
}
foreach ($f in @('aether-serial-bridge.exe', 'asb_win_qual.exe')) {
    if (-not (Test-Path -LiteralPath (Join-Path $Artifacts $f))) { Say "REFUSED: $f is not in $Artifacts"; exit 2 }
}
New-Item -ItemType Directory -Force -Path $Evidence | Out-Null
Say "WS-7 station qualification, phase $Phase, $(Get-Date -Format o)"
Say ("Windows {0} build {1}" -f (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion').DisplayVersion,
     (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion').CurrentBuild)

$info = Service-Info
$kind = Detect "detect-$Phase"
Say "installation: $kind"
if ($kind -eq 'Unknown') { Say 'REFUSED: the installation could not be detected (see qual-detect-*.txt).'; exit 2 }
if ($info -and $kind -ne 'Current' -and $kind -ne 'IncompleteInstall' -and $info.State -ne 'Stopped') {
    Say "REFUSED: the service '$ServiceName' ($kind, $($info.PathName)) is $($info.State). Stop it first; this script never touches it."
    exit 2
}

Record 'SKIPPED' 'HW 4: over-the-shoulder UAC' 'GUI flow (owner WS-10); not part of this script'

# ------------------------------------------------------------------------------------------------------------
if ($Phase -eq 'Run') {
    if (Test-Path $StateFile) { Say "REFUSED: $StateFile exists from an earlier run; finish it (-Phase Cleanup) first."; exit 2 }
    $state = [ordered]@{ LockDirAside = $null; DataDirAside = $null; LockDirAsideStamp = $null;
                         DataDirAsideStamp = $null; LockDirMade = $null; DataDirMade = $null; LockDirMoved = $false;
                         DataDirMoved = $false; RulesBefore = (Group-Rules) }
    $configPath = Write-Config ($(if ($ComPort) { $ComPort } else { 'COM250' }))

    if ($kind -ne 'NotInstalled') { Say "REFUSED: the service '$ServiceName' exists ($kind, $($info.PathName)); for the Qt service run -Phase Cleanup first, anything else is left alone."; exit 2 }

    # Folders from before (e.g. a config left after an uninstall) are moved aside, untouched, and restored later.
    $stamp = Get-Date -Format 'yyyyMMddHHmmss'
    # The restoration state is on disk BEFORE anything moves (review round 2): what is about to move aside and its
    # identity (a rename keeps the creation time), then saved again after each move. A move that fails halfway
    # (Program Files moved, ProgramData held open by something) leaves a state.json that -Phase Cleanup acts on.
    $state.LockDirAside = $null; $state.DataDirAside = $null
    if (Test-Path -LiteralPath $LockDir) { $state.LockDirAside = "$LockDir.ws7-aside-$stamp"; $state.LockDirAsideStamp = Folder-Stamp $LockDir }
    if (Test-Path -LiteralPath $DataDir) { $state.DataDirAside = "$DataDir.ws7-aside-$stamp"; $state.DataDirAsideStamp = Folder-Stamp $DataDir }
    Save-State $state
    try {
        if ($state.LockDirAside) { Rename-Item -LiteralPath $LockDir -NewName (Split-Path -Leaf $state.LockDirAside); $state.LockDirMoved = $true; Save-State $state }
        if ($state.DataDirAside) { Rename-Item -LiteralPath $DataDir -NewName (Split-Path -Leaf $state.DataDirAside); $state.DataDirMoved = $true; Save-State $state }
    } catch {
        Record 'FAIL' 'INSTALL: move the existing folders aside' "$_ (nothing was installed). NEXT: run -Phase Cleanup: it puts back what was moved."
        Summary
    }

    # ---- install ----
    $q = Qual @('install', '--config', $configPath) 'install'
    # The folders this run created, by their identity: Cleanup removes them only while they are still these.
    $state.LockDirMade = Folder-Stamp $LockDir
    $state.DataDirMade = Folder-Stamp $DataDir
    Save-State $state
    $svc = Service-Info
    Start-Sleep -Seconds 2
    if ($q.Code -eq 0 -and $svc -and $svc.State -eq 'Running' -and (Listening $TcpPort)) { Record 'PASS' 'INSTALL: Save & apply -> Ok, listening' $q.Result }
    else { Record 'FAIL' 'INSTALL: Save & apply -> Ok, listening' "$($q.Result) state=$($svc.State) listening=$(Listening $TcpPort)" }

    $q = Qual @('fw-check') 'fw-check'
    if ($q.Code -eq 0) { Record 'PASS' 'FIREWALL comparator' $q.Result } else { Record 'FAIL' 'FIREWALL comparator' $q.Result }

    $q = Qual @('keyword-matrix') 'keyword-matrix'
    if ($q.Code -eq 0) { Record 'PASS' 'KEYWORDS: pinned set holds here' $q.Result } else { Record 'FAIL' 'KEYWORDS: pinned set holds here' (($q.Lines | Where-Object { $_ -like '*DIFFERS*' }) -join ' | ') }

    # ---- HW 10: listing ports opens no COM port ----
    if ($Procmon -and (Test-Path -LiteralPath $Procmon)) {
        $pml = Join-Path $Evidence 'hw10.pml'
        $csv = Join-Path $Evidence 'hw10.csv'
        Start-Process -FilePath $Procmon -ArgumentList '/AcceptEula', '/Quiet', '/Minimized', '/BackingFile', "`"$pml`"" | Out-Null
        Start-Sleep -Seconds 5
        $q = Qual @('list-ports') 'list-ports'
        Start-Sleep -Seconds 2
        Start-Process -FilePath $Procmon -ArgumentList '/Terminate' -Wait | Out-Null
        Start-Process -FilePath $Procmon -ArgumentList '/AcceptEula', '/Quiet', '/OpenLog', "`"$pml`"", '/SaveAs', "`"$csv`"" -Wait | Out-Null
        $rows = Items (Import-Csv $csv | Where-Object { $_.'Process Name' -like 'asb_win_qual*' -and $_.Operation -eq 'CreateFile' -and $_.Path -match '(?i)(\\\\\.\\COM\d+|\\Device\\Serial|\\Device\\VCP|COM\d+$)' })
        if ($rows.Count -eq 0) { Record 'PASS' 'HW 10: --list-ports opens no COM port' "$($q.Result); Process Monitor: 0 CreateFile on a COM device" }
        else { Record 'FAIL' 'HW 10: --list-ports opens no COM port' ("CreateFile on: " + (($rows | ForEach-Object { $_.Path }) -join ', ')) }
    } else {
        Record 'SKIPPED' 'HW 10: --list-ports opens no COM port' 'pass -Procmon <path to Procmon64.exe> (the listing is the harness''s QSerialPortInfo, the code --list-ports uses on Windows)'
    }

    # ---- a real COM port through the installed service ----
    if ($ComPort) {
        $client = New-Object System.Net.Sockets.TcpClient
        try {
            $client.Connect('127.0.0.1', $TcpPort)
            Start-Sleep -Seconds 3
            $q = Qual @('status') 'serial-status'
            $statusLine = ($q.Lines | Where-Object { $_ -like 'STATUS *' } | Select-Object -First 1)
            $why = Serial-Verdict $statusLine $ComPort
            if ($null -eq $why) { Record 'PASS' "SERIAL smoke ($ComPort)" 'client connected; the port opened; no lastError; nothing written to the port' }
            else { Record 'FAIL' "SERIAL smoke ($ComPort)" $why }
        } catch {
            Record 'FAIL' "SERIAL smoke ($ComPort)" "$_"
        } finally { $client.Close() }
    } else {
        Record 'SKIPPED' 'SERIAL smoke' 'pass -ComPort COMn to bridge a real port'
    }

    # ---- HW 8 part 1 ----
    $svc = Service-Info
    if ($svc -and $svc.StartMode -eq 'Auto') { Record 'PENDING' 'HW 8: reboot with auto-start' 'start type Automatic. NEXT: reboot this PC, then run: -Phase AfterReboot' }
    else { Record 'FAIL' 'HW 8: reboot with auto-start' "start type is $($svc.StartMode), not Automatic" }
    Summary
}

# ------------------------------------------------------------------------------------------------------------
$state = Load-State
if (-not $state -and $Phase -eq 'Cleanup') {
    # AfterSecondReboot ends with the cleanup and deletes the state file. With no service installed there is nothing
    # of this script's left; with one, this script cannot tell what it owns and touches nothing (review round 3).
    if ($kind -eq 'NotInstalled') {
        Say "Nothing to clean up: there is no state file in $Evidence and no service is installed (AfterSecondReboot already cleaned up)."
        exit 0
    }
    Say "REFUSED: there is no state file in $Evidence, but the installation is '$kind': this script cannot tell what it installed. Remove it with the product's own uninstall if it is the Qt service."
    exit 2
}
if (-not $state) { Say "REFUSED: no state file in $Evidence (run -Phase Run first)."; exit 2 }

if ($Phase -eq 'AfterReboot') {
    $ok = $false
    for ($i = 0; $i -lt 60 -and -not $ok; $i++) {
        $svc = Service-Info
        if ($svc -and $svc.State -eq 'Running' -and (Listening $TcpPort)) { $ok = $true } else { Start-Sleep -Seconds 2 }
    }
    if ($ok) { Record 'PASS' 'HW 8a: back after reboot, listening' "pid $($svc.ProcessId)" } else { Record 'FAIL' 'HW 8a: back after reboot, listening' "state=$($svc.State) listening=$(Listening $TcpPort)" }
    $q = Qual @('apply-fail', '--config', (Join-Path $Evidence 'config.json')) 'apply-fail'
    $svc = Service-Info
    if ($q.Code -eq 0 -and $svc.StartMode -eq 'Disabled' -and $svc.State -ne 'Running') {
        Record 'PENDING' 'HW 8b: failed apply then reboot stays down' "$($q.Result); disabled and stopped. NEXT: reboot, then run: -Phase AfterSecondReboot"
    } else { Record 'FAIL' 'HW 8b: failed apply then reboot stays down' "$($q.Result) start=$($svc.StartMode) state=$($svc.State)" }
    Summary
}

if ($Phase -eq 'AfterSecondReboot') {
    Start-Sleep -Seconds 30   # give SCM every chance to (wrongly) start it
    $svc = Service-Info
    if ($svc -and $svc.State -ne 'Running' -and -not (Listening $TcpPort)) { Record 'PASS' 'HW 8b: failed apply then reboot stays down' "state=$($svc.State) start=$($svc.StartMode); port $TcpPort closed" }
    else { Record 'FAIL' 'HW 8b: failed apply then reboot stays down' "state=$($svc.State) listening=$(Listening $TcpPort)" }
    if (Cleanup $state) { Remove-Item -LiteralPath $StateFile -Force }
    Summary
}

if ($Phase -eq 'Cleanup') {
    if (Cleanup $state) { Remove-Item -LiteralPath $StateFile -Force }
    Summary
}
