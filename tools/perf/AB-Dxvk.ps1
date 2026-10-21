<#
.SYNOPSIS
  DXVK 2.7.1 (the client's own) and another d3d9.dll in alternating rounds (2026-10-21), stream off. Puts the
  client's own back at the end.
#>
param(
    [Parameter(Mandatory)][string]$Other,
    [string]$Test = 'perf-quick',
    [int]$Rounds = 1,
    [int]$Width = 1152,
    [int]$Height = 864,
    [string]$Out = (Join-Path $env:TEMP 'ab-dxvk.txt')
)
$ErrorActionPreference = 'Continue'
"AB-Dxvk $Test, $Rounds rounds, $(Get-Date)" | Set-Content $Out
for ($r = 1; $r -le $Rounds; $r++) {
    foreach ($v in 'orig', $Other) {
        $lines = & (Join-Path $PSScriptRoot 'Run-Perf.ps1') -Test $Test -Stream 0 -NoInstall -Width $Width -Height $Height -D3d9 $v *>&1 | ForEach-Object { "$_" }
        "round $r d3d9 $v" | Add-Content $Out
        $lines | Where-Object { $_ -match 'pass .*(fps|CPU)' -or $_ -match '(FAIL|fail)' } | ForEach-Object { "  $_" } | Add-Content $Out
    }
}
& (Join-Path $PSScriptRoot 'Fresh-Client.ps1') -NoLogin -D3d9 orig | Out-Null
"done $(Get-Date)" | Add-Content $Out
