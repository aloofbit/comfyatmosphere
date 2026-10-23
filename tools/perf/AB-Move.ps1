<#
.SYNOPSIS
  Variants in alternating rounds, measured in motion (2026-10-23). Each variant is a DLL and ini values; each run
  restarts the test client at 1920 x 1080, writes the values over the client's own ini, and runs one test with
  -NoLogin. The client's ini is put back at the end.

.PARAMETER Variants
  The variants, separated by ';': 'label|dll|section.key=value,section.key=value'. An empty dll is this
  worktree's build, copied once at the start (a rebuild during the run does not change it). One string, as
  powershell -File passes it.

.EXAMPLE
  .\AB-Move.ps1 -Variants 'old|C:\x\comfyatmos-0.17.0.dll|;new||' -Rounds 2
#>
param(
    [Parameter(Mandatory)][string]$Variants,
    [string]$Test = 'perf-move-only',
    [int]$Rounds = 2,
    [int]$Width = 1920,
    [int]$Height = 1080,
    [string]$Out = (Join-Path $env:TEMP 'ab-move.txt'),
    [string]$Client = (Join-Path $env:USERPROFILE 'Desktop\wow-clients\octow - Copy')
)
$ErrorActionPreference = 'Continue'
$repo = Split-Path (Split-Path $PSScriptRoot)
$tool = Join-Path $repo '..\..\tools\wow-test-tool'
$keep = Join-Path $env:TEMP 'comfy-perf-logs'
New-Item -ItemType Directory -Force $keep | Out-Null

$ini = Join-Path $Client 'comfyatmos.ini'
$iniOrig = [IO.File]::ReadAllText($ini)
$work = Join-Path $env:TEMP ("ab-move-" + (Get-Date -Format 'HHmmss'))
New-Item -ItemType Directory -Force $work | Out-Null

$parsed = @()
foreach ($v in ($Variants -split ';' | Where-Object { $_.Trim() })) {
    $p = $v -split '\|', 3
    $dll = $p[1]
    if (-not $dll) { $dll = Join-Path $repo 'comfyatmos.dll' }
    $copy = Join-Path $work ($p[0] + '.dll')
    Copy-Item $dll $copy -Force
    $sets = @()
    if ($p.Count -gt 2 -and $p[2]) { $sets = $p[2] -split ',' }
    $parsed += [pscustomobject]@{ Label = $p[0]; Dll = $copy; Sets = $sets }
}

function Set-IniValue([string]$text, [string]$section, [string]$key, [string]$value) {
    $nl = "`r`n"
    $secPat = '(?ms)^\[' + [regex]::Escape($section) + '\]\r?\n(.*?)(?=^\[|\z)'
    $m = [regex]::Match($text, $secPat)
    if (-not $m.Success) { return $text.TrimEnd() + "$nl$nl[$section]$nl$key = $value$nl" }
    $body = $m.Groups[1].Value
    $keyPat = '(?m)^' + [regex]::Escape($key) + '\s*=.*$'
    if ([regex]::IsMatch($body, $keyPat)) { $body = [regex]::Replace($body, $keyPat, "$key = $value") }
    else { $body = "$key = $value$nl" + $body }
    return $text.Substring(0, $m.Groups[1].Index) + $body + $text.Substring($m.Groups[1].Index + $m.Groups[1].Length)
}

"AB-Move $Test, $Rounds rounds, $($Width)x$Height, $(Get-Date)" | Set-Content $Out
foreach ($v in $parsed) { "  $($v.Label): $($v.Dll) $($v.Sets -join ', ')" | Add-Content $Out }
try {
    for ($r = 1; $r -le $Rounds; $r++) {
        foreach ($v in $parsed) {
            & (Join-Path $PSScriptRoot 'Fresh-Client.ps1') -NoLogin -Client $Client *>&1 | Out-Null
            for ($i = 0; $i -lt 20; $i++) { try { Copy-Item $v.Dll (Join-Path $Client 'comfyatmos.dll') -Force -ErrorAction Stop; break } catch { Start-Sleep 1 } }
            $text = $iniOrig
            foreach ($s in $v.Sets) {
                if ($s -match '^\s*([^.]+)\.([^=]+)=(.*)$') { $text = Set-IniValue $text $Matches[1].Trim() $Matches[2].Trim() $Matches[3].Trim() }
            }
            [IO.File]::WriteAllText($ini, $text)
            $login = & (Join-Path $tool 'Login.ps1') -Client $Client -Width $Width -Height $Height *>&1 | ForEach-Object { "$_" }
            if (-not ($login -match 'In the world')) { "round $r $($v.Label): login failed: $($login | Select-Object -Last 3)" | Add-Content $Out; continue }
            Push-Location (Join-Path $repo 'tests')
            try { $lines = & .\Run-Tests.ps1 $Test -NoLogin -NoOpen *>&1 | ForEach-Object { "$_" } } finally { Pop-Location }
            $log = Join-Path $Client 'Logs\comfyatmos.log'
            $name = "{0}-ab-r{1}-{2}.log" -f (Get-Date -Format 'HHmmss'), $r, $v.Label
            Copy-Item $log (Join-Path $keep $name)
            "round $r $($v.Label) ($name)" | Add-Content $Out
            $lines | Where-Object { $_ -match '^pass .*fps' -or $_ -match '\bFAIL\b' } | ForEach-Object { "  $_" } | Add-Content $Out
            # The frame logs' CPU and GPU lines, one of each a place.
            Get-Content $log | Where-Object { $_ -match '^framelog: (\d+ frames over|our CPU in all|our GPU a frame)' } |
                ForEach-Object { "    $_" } | Add-Content $Out
        }
    }
}
finally {
    [IO.File]::WriteAllText($ini, $iniOrig)
    "done $(Get-Date); the client's ini put back" | Add-Content $Out
}
