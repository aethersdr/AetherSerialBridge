<#
.SYNOPSIS
    Self-test of ci/scripts/win-ctest-split.ps1: it must fail closed.

.DESCRIPTION
    Runs the split against a fake ctest (written to a temporary folder) whose listings and runs are bent one way
    at a time: a test in neither run, a test in both, a listing whose names disagree with its Total Tests line, an
    empty serial listing, a run that ran fewer tests than its listing, a failed parallel run, a failed serial run.
    Each must exit 1 and say why; a failed parallel run must still run the native rows. The straight case must
    exit 0 with both runs made, the parallel one with -j and -LE win-integration, the serial one with -L and no -j.
    Needs no build and changes nothing outside its temporary folder. Exit 0 = every case behaved; 1 = one did not.
#>
Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
$split = Join-Path $PSScriptRoot 'win-ctest-split.ps1'
$work = Join-Path ([System.IO.Path]::GetTempPath()) ("asb-ctest-split-selftest-" + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force $work | Out-Null
$fake = Join-Path $work 'fake-ctest.ps1'
$calls = Join-Path $work 'calls.txt'
$script:bad = 0

# The fake: tests u1, u2 (unit), b1 (build), n1, n2 (win-integration). FAKE_* variables bend it.
@'
$a = @($args | ForEach-Object { "$_" })
Add-Content -LiteralPath $env:FAKE_CALLS -Value ($a -join ' ')
$tests = [ordered]@{ u1 = 'unit'; u2 = 'unit'; b1 = 'build'; n1 = 'win-integration'; n2 = 'win-integration' }
$i = [array]::IndexOf($a, '-L'); $L = if ($i -ge 0) { $a[$i + 1] } else { $null }
$i = [array]::IndexOf($a, '-LE'); $LE = if ($i -ge 0) { $a[$i + 1] } else { $null }
$pick = @($tests.Keys | Where-Object {
    (-not $L -or $tests[$_] -eq $L) -and (-not $LE -or $tests[$_] -ne $LE) })
if ($LE -and $env:FAKE_LEAK) { $pick = @($pick | Where-Object { $_ -ne 'b1' }) }    # b1 in neither run
if ($L -and $env:FAKE_OVERLAP) { $pick += 'u1' }                                    # u1 in both runs
if ($L -and $env:FAKE_EMPTY_NATIVE) { $pick = @() }
if ($a -contains '-N') {
    'Test project C:/fake/build/windows-dev'
    $n = 0; foreach ($t in $pick) { $n++; "  Test  #$($n): $t" }
    ''
    $total = $pick.Count; if ($env:FAKE_TOTAL_LIE -and -not $L -and -not $LE) { $total++ }
    "Total Tests: $total"
    if ($pick.Count -eq 0) { 'No tests were found!!!'; exit 8 }
    exit 0
}
$ran = $pick.Count
if ($LE -and $env:FAKE_SHORT) { $ran-- }
$rc = 0
if ($LE -and $env:FAKE_UNIT_RC) { $rc = [int]$env:FAKE_UNIT_RC }
if ($L -and $env:FAKE_NATIVE_RC) { $rc = [int]$env:FAKE_NATIVE_RC }
$failed = if ($rc -ne 0) { 1 } else { 0 }
# ctest's two summary shapes: no failure, and failures.
if ($failed -eq 0) { "100% tests passed out of $ran" }
else { "$([int](100 * ($ran - $failed) / [math]::Max($ran, 1)))% tests passed, $failed tests failed out of $ran" }
exit $rc
'@ | Set-Content -LiteralPath $fake -Encoding utf8

function Case([string]$name, [hashtable]$envs, [int]$wantRc, [string]$wantText) {
    foreach ($k in 'FAKE_LEAK', 'FAKE_OVERLAP', 'FAKE_EMPTY_NATIVE', 'FAKE_TOTAL_LIE', 'FAKE_SHORT', 'FAKE_UNIT_RC', 'FAKE_NATIVE_RC') {
        Remove-Item "Env:\$k" -ErrorAction SilentlyContinue
    }
    foreach ($k in $envs.Keys) { Set-Item "Env:\$k" $envs[$k] }
    $env:FAKE_CALLS = $calls
    Remove-Item -LiteralPath $calls -ErrorAction SilentlyContinue
    try { $out = & pwsh -NoProfile -File $split -Ctest $fake -LogDir $work 2>&1 | Out-String; $rc = $LASTEXITCODE }
    finally { foreach ($k in $envs.Keys) { Remove-Item "Env:\$k" -ErrorAction SilentlyContinue } }
    $runs = @(Get-Content -LiteralPath $calls -ErrorAction SilentlyContinue | Where-Object { $_ -notmatch '(^| )-N( |$)' })
    $ok = ($rc -eq $wantRc) -and ($out -match [regex]::Escape($wantText))
    return @{ ok = $ok; rc = $rc; out = $out; runs = $runs; name = $name }
}
function Report($r, [bool]$extra = $true, [string]$why = '') {
    if ($r.ok -and $extra) { Write-Host "ok    $($r.name)" }
    else { $script:bad++; Write-Host "FAIL  $($r.name) (exit $($r.rc)) $why`n$($r.out)" }
}

try {
    $r = Case 'straight: both runs, every test' @{} 0 'SPLIT: OK, 5 tests ran (3 parallel + 2 serial)'
    $shape = $r.runs.Count -eq 2 -and $r.runs[0] -match '(^| )-j 12 ' -and $r.runs[0] -match '-LE win-integration$' -and
             $r.runs[1] -match '-L win-integration$' -and $r.runs[1] -notmatch '(^| )-j '
    Report $r $shape "runs: $($r.runs -join ' | ')"

    Report (Case 'a test in neither run' @{ FAKE_LEAK = '1' } 1 'SPLIT: in neither run: b1')
    Report (Case 'a test in both runs' @{ FAKE_OVERLAP = '1' } 1 'SPLIT: in both runs: u1')
    Report (Case 'names disagree with Total Tests' @{ FAKE_TOTAL_LIE = '1' } 1 'listed 5 names but its Total Tests line says')
    Report (Case 'empty serial listing' @{ FAKE_EMPTY_NATIVE = '1' } 1 'ctest -N -L win-integration exited 8')
    Report (Case 'a run ran fewer tests than listed' @{ FAKE_SHORT = '1' } 1 "ran '2' tests, the listing has 3")

    $r = Case 'parallel run fails, native rows still run' @{ FAKE_UNIT_RC = '8' } 1 'parallel (-j 12): ctest exited 8'
    Report $r ($r.runs.Count -eq 2 -and $r.runs[1] -match '-L win-integration$') "runs: $($r.runs -join ' | ')"
    Report (Case 'serial run fails' @{ FAKE_NATIVE_RC = '8' } 1 'native rows, serial: ctest exited 8')
}
finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}
if ($script:bad -gt 0) { Write-Host "$($script:bad) case(s) did not fail closed"; exit 1 }
Write-Host 'the Test step split fails closed'
exit 0
