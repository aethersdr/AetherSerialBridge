<#
.SYNOPSIS
    The Windows leg's Test step: every test that is not a native row runs in parallel, then the native rows
    (label win-integration) run one at a time, and the two runs together must cover exactly the configured list.

.DESCRIPTION
    The native rows install the real service, firewall rule groups, the Apps & features key, COM ports and the
    FAT32 volume, so they never overlap anything (they also carry RUN_SERIAL). Every other Windows test works on
    fakes, its own temporary folders and OS-chosen loopback ports; the two that share Qt's per-user test-mode
    folder carry a RESOURCE_LOCK (tests/CMakeLists.txt).

    Fails (exit 1) when:
      - a listing cannot be read, is empty, repeats a name, or its names disagree with its "Total Tests" line;
      - the parallel and serial listings overlap, or together are not exactly the full listing;
      - a run's "out of N" is not its listing's count (a test that silently did not run);
      - either run fails. Both runs always happen: a unit failure does not skip the native rows.
    The parallel run's LastTest.log is kept as LastTest-unit.log (the serial run writes LastTest.log).
#>
param(
    [string]$Preset = 'windows-dev',
    [int]$Jobs = 12,
    [string]$Ctest = 'ctest',
    [string]$LogDir = 'build\windows-dev\Testing\Temporary'
)
Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
$Label = 'win-integration'
$script:bad = New-Object System.Collections.Generic.List[string]

# The test names in a listing, in order. Returned through ',' so a one-name listing stays an array.
function Listing([string[]]$filter) {
    $out = @(& $Ctest --preset $Preset -N @filter | ForEach-Object { "$_" })
    if ($LASTEXITCODE -ne 0) { throw "ctest -N $filter exited $LASTEXITCODE`n$($out -join "`n")" }
    $names = @($out | ForEach-Object { if ($_ -match '^\s*Test\s+#\d+:\s+(\S+)\s*$') { $Matches[1] } })
    $total = @($out | ForEach-Object { if ($_ -match '^\s*Total Tests:\s*(\d+)\s*$') { [int]$Matches[1] } })
    if ($total.Count -ne 1 -or $total[0] -ne $names.Count) {
        throw "ctest -N $filter listed $($names.Count) names but its Total Tests line says '$($total -join ',')'"
    }
    return , $names
}

# One ctest run, printed as it goes; returns its exit code and the N of its summary's "out of N".
function RunCtest([string]$what, [string[]]$ctestArgs) {
    Write-Host "=== $what`: ctest --preset $Preset $($ctestArgs -join ' ') ==="
    $out = @(& $Ctest --preset $Preset @ctestArgs | ForEach-Object { $s = "$_"; Write-Host $s; $s })
    $rc = $LASTEXITCODE
    # ctest's summary: "100% tests passed out of N", or "P% tests passed, F tests failed out of N".
    $ran = @($out | ForEach-Object { if ($_ -match '^\s*\d+% tests passed(, \d+ tests failed)? out of (\d+)\s*$') { [int]$Matches[2] } })
    return @{ rc = $rc; ran = $ran }
}

$all = Listing @()
$unit = Listing @('-LE', $Label)
$native = Listing @('-L', $Label)
Write-Host "listing: $($all.Count) tests = $($unit.Count) in parallel (-LE $Label) + $($native.Count) serial (-L $Label)"

if ($all.Count -eq 0) { $script:bad.Add('the full listing is empty') }
if ($unit.Count -eq 0) { $script:bad.Add('the parallel listing is empty') }
if ($native.Count -eq 0) { $script:bad.Add("the serial listing (-L $Label) is empty") }
foreach ($g in @($all | Group-Object | Where-Object Count -gt 1)) { $script:bad.Add("listed $($g.Count) times: $($g.Name)") }
foreach ($t in @($unit | Where-Object { $native -contains $_ })) { $script:bad.Add("in both runs: $t") }
foreach ($t in @($all | Where-Object { $unit -notcontains $_ -and $native -notcontains $_ })) { $script:bad.Add("in neither run: $t") }
foreach ($t in @(@($unit) + @($native) | Where-Object { $all -notcontains $_ })) { $script:bad.Add("not in the full listing: $t") }
if ($unit.Count + $native.Count -ne $all.Count) {
    $script:bad.Add("$($unit.Count) + $($native.Count) is not the full listing's $($all.Count)")
}
if ($script:bad.Count -gt 0) {
    foreach ($b in $script:bad) { Write-Host "SPLIT: $b" }
    exit 1
}

$runs = @(
    @{ what = "parallel (-j $Jobs)"; args = @('-j', "$Jobs", '-LE', $Label); expect = $unit.Count; keep = 'LastTest-unit.log' },
    @{ what = 'native rows, serial'; args = @('-L', $Label); expect = $native.Count; keep = $null }
)
foreach ($r in $runs) {
    $res = RunCtest $r.what $r.args
    if ($res.rc -ne 0) { $script:bad.Add("$($r.what): ctest exited $($res.rc)") }
    if ($res.ran.Count -ne 1 -or $res.ran[0] -ne $r.expect) {
        $script:bad.Add("$($r.what): ran '$($res.ran -join ',')' tests, the listing has $($r.expect)")
    }
    if ($r.keep) {
        $log = Join-Path $LogDir 'LastTest.log'
        if (Test-Path -LiteralPath $log) { Copy-Item -LiteralPath $log (Join-Path $LogDir $r.keep) -Force }
    }
}
if ($script:bad.Count -gt 0) {
    foreach ($b in $script:bad) { Write-Host "SPLIT: $b" }
    exit 1
}
Write-Host "SPLIT: OK, $($all.Count) tests ran ($($unit.Count) parallel + $($native.Count) serial)"
exit 0
