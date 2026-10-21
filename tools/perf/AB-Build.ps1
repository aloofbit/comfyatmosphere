<#
.SYNOPSIS
  An older comfyatmos.dll against this worktree's in alternating rounds (2026-10-21), stream off. Each variant runs
  its own test: the old one -OldTest, the new one -NewTest (which may set the new defaults the client's ini lacks).
#>
param(
    [Parameter(Mandatory)][string]$OldDll,
    [string]$OldTest = 'perf-owner',
    [string]$NewTest = 'perf-owner-final',
    [int]$Rounds = 2,
    [int]$Width = 1920,
    [int]$Height = 1080,
    [string]$Out = (Join-Path $env:TEMP 'ab-build.txt'),
    [string]$Client = (Join-Path $env:USERPROFILE 'Desktop\wow-clients\octow - Copy')
)
$ErrorActionPreference = 'Continue'
$repo = Split-Path (Split-Path $PSScriptRoot)
"AB-Build $OldTest / $NewTest, $Rounds rounds, $(Get-Date)" | Set-Content $Out
for ($r = 1; $r -le $Rounds; $r++) {
    foreach ($v in 'old', 'new') {
        & (Join-Path $PSScriptRoot 'Fresh-Client.ps1') -NoLogin -Client $Client | Out-Null
        $src = if ($v -eq 'old') { $OldDll } else { Join-Path $repo 'comfyatmos.dll' }
        for ($i = 0; $i -lt 20; $i++) { try { Copy-Item $src (Join-Path $Client 'comfyatmos.dll') -Force; break } catch { Start-Sleep 1 } }
        $test = if ($v -eq 'old') { $OldTest } else { $NewTest }
        $lines = & (Join-Path $PSScriptRoot 'Run-Perf.ps1') -Test $test -Stream 0 -NoInstall -Width $Width -Height $Height *>&1 | ForEach-Object { "$_" }
        "round $r $v" | Add-Content $Out
        $lines | Where-Object { $_ -match 'pass .*fps' -or $_ -match '(FAIL|fail)' } | ForEach-Object { "  $_" } | Add-Content $Out
    }
}
"done $(Get-Date)" | Add-Content $Out
