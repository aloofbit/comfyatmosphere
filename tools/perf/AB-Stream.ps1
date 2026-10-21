<#
.SYNOPSIS
  Stream off and on in alternating rounds (2026-10-21): each round restarts the client with [stream] enabled set,
  runs a test, and keeps its fps lines. The laptop's GPU drifts as it warms, so the variants alternate.
#>
param(
    [string]$Test = 'perf-quick',
    [int]$Rounds = 2,
    [int]$Width = 1152,
    [int]$Height = 864,
    [string]$D3d9 = '',
    [string]$Out = (Join-Path $env:TEMP 'ab-stream.txt')
)
$ErrorActionPreference = 'Continue'
"AB-Stream $Test, $Rounds rounds, $(Get-Date)" | Set-Content $Out
$install = $true
for ($r = 1; $r -le $Rounds; $r++) {
    foreach ($s in 0, 1) {
        $lines = & (Join-Path $PSScriptRoot 'Run-Perf.ps1') -Test $Test -Stream $s -NoInstall:(-not $install) -Width $Width -Height $Height -D3d9 $D3d9 *>&1 | ForEach-Object { "$_" }
        $install = $false
        $fps = $lines | Where-Object { $_ -match 'pass .*(fps|CPU)' -or $_ -match '(FAIL|fail)' }
        "round $r stream $s" | Add-Content $Out
        $fps | ForEach-Object { "  $_" } | Add-Content $Out
    }
}
"done $(Get-Date)" | Add-Content $Out
