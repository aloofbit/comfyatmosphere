<#
.SYNOPSIS
  One page of every test's latest result, whichever run it came from (2026-10-06, the owner).

.DESCRIPTION
  Run-Tests.ps1 saves each test's result as results\<time>-<test>.record.json. This script takes the newest one of
  each test in tests\ and writes results\latest-report.html, and the same page as results\last-results.html, so a tab
  kept open there shows it when refreshed. Each test's section gives the time of the run it came from.

  A run before 2026-10-06 16:00 saved no record. For such a test the result is read back from its text report,
  results\<time>-<test>.txt: the checks, where the character was, the errors, the shots and the recordings. Which
  Debug View each shot shows comes from the test's steps as they are now.

  -Card also writes the results cards from the same records (Write-Card.ps1): media\test-summary.svg for the README,
  and media\test-results.svg, the large one.

.EXAMPLE
  .\Build-Latest.ps1                        # every test
  .\Build-Latest.ps1 -Name water-standing   # these tests only
  .\Build-Latest.ps1 -Card                  # and the results cards
#>
param(
    [string[]]$Name,
    [switch]$NoOpen,
    [switch]$Card,
    [string]$Client = (Join-Path $env:USERPROFILE 'Desktop\wow-clients\octow - Copy')
)

$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$resultsDir = Join-Path $here 'results'
$expectedDir = Join-Path $here 'expected'

# The Debug Views' names, for the page, as Run-Tests.ps1 reads them (kDebugViews in cvars.cpp).
$viewNames = @()
$cvarsSrc = Join-Path $here '..\src\cvars.cpp'
if (Test-Path $cvarsSrc) {
    $src = Get-Content $cvarsSrc -Raw
    $at = $src.IndexOf('kDebugViews[] = {')
    if ($at -ge 0) {
        $block = $src.Substring($at, $src.IndexOf('};', $at) - $at)
        $viewNames = @([regex]::Matches($block, '\{ "([^"]*)"') | ForEach-Object { $_.Groups[1].Value })
    }
}

# A result read back from a text report, for a run that saved no record.
function FromText([string]$file, $t) {
    $lines = @(Get-Content $file)
    $stamp = ([IO.Path]::GetFileName($file)).Substring(0, 15)
    $expects = @($t.expect | Where-Object { $_ })
    $checks = @(); $positions = @(); $warnings = @(); $shots = @(); $recs = @()
    $section = ''
    foreach ($line in $lines) {
        if ($line -eq '') { break }   # the probe follows the report after an empty line
        if ($line -match '^(PASS|FAIL)  \S+  \(') { continue }
        if ($line -eq 'where the character was:') { $section = 'pos'; continue }
        if ($line -eq 'errors from the game:') { $section = 'err'; continue }
        if ($line -match '^screenshot: (.+)$') { if ($Matches[1] -ne 'none taken') { $shots += $Matches[1] }; continue }
        if ($line -match '^recording: (.+)$') { $recs += $Matches[1]; continue }
        if ($line -match '^        (.*)$') {
            if ($section -eq 'pos') { $positions += $Matches[1] }
            elseif ($section -eq 'err') { $warnings += $Matches[1] }
            continue   # a check's matching lines
        }
        if ($line -match '^(pass|FAIL)  (.*)$') {
            $section = ''
            $ok = $Matches[1] -eq 'pass'
            $text = $Matches[2]
            # The check whose about the line holds: the about can itself hold a colon.
            $e = $expects | Where-Object { $_.about -and $text.Contains($_.about) } | Select-Object -First 1
            if ($e) {
                $got = $text.Replace($e.about, '').Trim(' ', ':')
                $shot = if ($null -ne $e.row -or $null -ne $e.box) { [int]$e.shot } else { 0 }
                $checks += [pscustomobject]@{ ok = $ok; about = $e.about; got = $got; shot = $shot }
            }
            else { $checks += [pscustomobject]@{ ok = $ok; about = $text; got = ''; shot = 0 } }
        }
    }
    # Which Debug View each shot and recording shows, from the steps, as Run-Tests.ps1 tracks it.
    $view = if ($null -ne $t.config.debugView) { [int]$t.config.debugView } else { 0 }
    $views = @(); $recViews = @(); $recLabels = @()
    foreach ($st in $t.steps) {
        $p = $st.PSObject.Properties | Select-Object -First 1
        switch ($p.Name) {
            'cvar' { if ("$($p.Value)" -match '^comfyDebugView\s+(\d+)') { $view = [int]$Matches[1] } }
            'screenshot' { $views += $view }
            'record' { $recViews += $view; $recLabels += $(if ($p.Value.label) { $p.Value.label } else { '' }) }
        }
    }
    $first = $lines | Select-Object -First 1
    [pscustomobject]@{
        name = $t.name; about = $t.about; pass = $first -match '^PASS'; stamp = $stamp; checks = $checks
        positions = $positions; warnings = $warnings; shots = $shots; views = $views
        recordings = $recs; recViews = $recViews; recLabels = $recLabels
    }
}

$files = if ($Name) { $Name | ForEach-Object { Join-Path $here "$_.json" } } else { Get-ChildItem $here -Filter *.json | ForEach-Object { $_.FullName } }
$records = @()
$missing = @()
foreach ($file in $files) {
    $t = Get-Content $file -Raw | ConvertFrom-Json
    $n = [regex]::Escape($t.name)
    $found = @(Get-ChildItem $resultsDir -File | Where-Object { $_.Name -match "^\d{8}-\d{6}-$n\.(record\.json|txt)$" })
    if (-not $found.Count) { $missing += $t.name; continue }
    # The newest run; its record before its text report.
    $newest = $found | Sort-Object @{ Expression = { $_.Name.Substring(0, 15) } }, @{ Expression = { $_.Name -like '*.record.json' } } |
              Select-Object -Last 1
    if ($newest.Name -like '*.record.json') {
        $r = Get-Content $newest.FullName -Raw | ConvertFrom-Json
        # Unrolled: Windows PowerShell's ConvertFrom-Json gives a JSON array as one object.
        foreach ($k in 'checks', 'positions', 'warnings', 'shots', 'views', 'recordings', 'recViews', 'recLabels') {
            $r.$k = @($r.$k | ForEach-Object { $_ })
        }
        $records += $r
    }
    else { $records += FromText $newest.FullName $t }
}

$records = @($records | Sort-Object name)
$oldest = ($records | ForEach-Object { $_.stamp } | Sort-Object | Select-Object -First 1)
$newestRun = ($records | ForEach-Object { $_.stamp } | Sort-Object | Select-Object -Last 1)
$fmt = { param($s) [datetime]::ParseExact($s, 'yyyyMMdd-HHmmss', [Globalization.CultureInfo]::InvariantCulture).ToString('yyyy-MM-dd HH:mm') }
$summary = "each test's latest run, $(& $fmt $oldest) to $(& $fmt $newestRun)"
$page = Join-Path $resultsDir 'latest-report.html'
& (Join-Path $here 'Write-Report.ps1') -Records $records -Page $page -ExpectedDir $expectedDir -ViewNames $viewNames `
    -History (Join-Path $resultsDir 'perf-history.json') -Client $Client -Summary $summary
Copy-Item $page (Join-Path $resultsDir 'last-results.html') -Force
if ($Card) {
    # Every test, so the card never shows a part of them as the whole.
    if ($Name) { Write-Host 'card: not written, -Name takes only some tests' }
    else {
        & (Join-Path $here 'Write-Card.ps1') -Records $records -History (Join-Path $resultsDir 'perf-history.json') `
            -Path (Join-Path $here '..\media\test-results.svg') -SummaryPath (Join-Path $here '..\media\test-summary.svg') `
            -Client $Client
    }
}

foreach ($r in $records) { Write-Host ('{0}  {1}  ({2})' -f $(if ($r.pass) { 'PASS' } else { 'FAIL' }), $r.name, $r.stamp) }
if ($missing.Count) { Write-Host "never run: $($missing -join ', ')" }
Write-Host "report: $page"
if (-not $NoOpen) { Start-Process $page }
