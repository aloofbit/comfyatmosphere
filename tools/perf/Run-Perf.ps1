<#
.SYNOPSIS
  The perf branch's A/B run (2026-10-21): set [stream] in the test client's comfyatmos.ini, install this worktree's
  build, restart the client and run one test with -NoLogin. Prints the results and the stream's log lines.

.EXAMPLE
  .\Run-Perf.ps1 -Test perf-quick -Stream 1
#>
param(
    [string]$Test = 'perf-quick',
    [int]$Stream = 0,
    [int]$Stats = 1,
    [int]$Ahead = 1,
    [switch]$NoInstall,
    [int]$Width = 1152,
    [int]$Height = 864,
    [string]$D3d9 = '',
    [string]$Client = (Join-Path $env:USERPROFILE 'Desktop\wow-clients\octow - Copy')
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path (Split-Path $PSScriptRoot)

# Fresh-Client closes the client first; the ini is written while it is closed.
if ($D3d9) { & (Join-Path $PSScriptRoot 'Fresh-Client.ps1') -Install:(-not $NoInstall) -NoLogin -Client $Client -D3d9 $D3d9 }
else { & (Join-Path $PSScriptRoot 'Fresh-Client.ps1') -Install:(-not $NoInstall) -NoLogin -Client $Client }
$ini = Join-Path $Client 'comfyatmos.ini'
$text = [IO.File]::ReadAllText($ini)
$block = "[stream]`r`nenabled = $Stream`r`nstats = $Stats`r`nframesAhead = $Ahead`r`n"
if ($text -match '(?ms)^\[stream\]\r?\n.*?(?=^\[|\z)') { $text = [regex]::Replace($text, '(?ms)^\[stream\]\r?\n.*?(?=^\[|\z)', $block) }
else { $text = $text.TrimEnd() + "`r`n`r`n" + $block }
[IO.File]::WriteAllText($ini, $text)

& (Join-Path $repo '..\..\tools\wow-test-tool\Login.ps1') -Client $Client -Width $Width -Height $Height
Push-Location (Join-Path $repo 'tests')
try { & .\Run-Tests.ps1 $Test -NoLogin -NoOpen } finally { Pop-Location }
$log = Join-Path $Client 'Logs\comfyatmos.log'
Get-Content $log | Select-String -Pattern '^stream:|^framelog: \d+ frames|our GPU a frame|the water on the GPU' | Select-Object -Last 40 | ForEach-Object { $_.Line }
# The whole log kept, for the frame logs' detail lines (a restart rewrites it).
$keep = Join-Path $env:TEMP ("comfy-perf-logs")
New-Item -ItemType Directory -Force $keep | Out-Null
Copy-Item $log (Join-Path $keep ("{0}-{1}-stream{2}-{3}x{4}.log" -f (Get-Date -Format 'HHmmss'), $Test, $Stream, $Width, $Height))
