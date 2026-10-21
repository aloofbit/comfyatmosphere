<#
.SYNOPSIS
  The perf branch's install (2026-10-20): close the test client, copy this worktree's builds into it, set the
  measuring settings in Config.wtf, start it and log in.

.DESCRIPTION
  - Closes only the test client (wow-test-tool's Get-WowProcess: the comfytest pipe or comfytest-pid.txt).
  - Copies comfyatmos.dll and tools\prof\comfyprof.dll (with -Install), retrying while the old file is held.
  - -D3d9 <path>: that d3d9.dll goes into the client (the old one is kept as d3d9.dll.perf-orig once).
    -D3d9 orig puts the original back.
  - Config.wtf: gxVSync 0 (the client drops the line, and the display's 120 Hz then caps every measurement),
    gxMultisample 2.
  - Logs in with wow-test-tool's Login.ps1 at 1152x864.
#>
param(
    [switch]$Install,
    [string]$D3d9,
    [int]$Character = 1,
    [string]$Client = (Join-Path $env:USERPROFILE 'Desktop\wow-clients\octow - Copy'),
    [switch]$NoLogin
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path (Split-Path $PSScriptRoot)
$tool = Join-Path $repo '..\..\tools\wow-test-tool'
Import-Module (Join-Path $tool 'Robot.psm1') -Force -DisableNameChecking

$wow = Get-WowProcess -Client $Client
if ($wow) {
    Write-Host "Closing the test client ($($wow.Id))."
    $null = $wow.CloseMainWindow()
    if (-not $wow.WaitForExit(15000)) { $null = $wow.CloseMainWindow(); if (-not $wow.WaitForExit(15000)) { Stop-Process -Id $wow.Id -Force } }
    Start-Sleep -Milliseconds 500
}

function Copy-Retry([string]$from, [string]$to) {
    for ($i = 0; $i -lt 20; $i++) {
        try { Copy-Item $from $to -Force; return } catch { Start-Sleep -Seconds 1 }
    }
    throw "Could not copy $from to $to"
}
if ($Install) {
    Copy-Retry (Join-Path $repo 'comfyatmos.dll') (Join-Path $Client 'comfyatmos.dll')
    Copy-Retry (Join-Path $repo 'tools\prof\comfyprof.dll') (Join-Path $Client 'comfyprof.dll')
    Write-Host 'Installed comfyatmos.dll and comfyprof.dll.'
}
if ($D3d9) {
    $orig = Join-Path $Client 'd3d9.dll.perf-orig'
    if (-not (Test-Path $orig)) { Copy-Item (Join-Path $Client 'd3d9.dll') $orig }
    $src = if ($D3d9 -eq 'orig') { $orig } else { $D3d9 }
    Copy-Retry $src (Join-Path $Client 'd3d9.dll')
    Write-Host "d3d9.dll from $src."
}

$config = Join-Path $Client 'WTF\Config.wtf'
$text = [IO.File]::ReadAllText($config)
foreach ($kv in @(@('gxVSync', '0'), @('gxMultisample', '2'))) {
    $want = "SET $($kv[0]) `"$($kv[1])`""
    if ($text -match "(?m)^SET $($kv[0]) `"[^`"]*`"") { $text = $text -replace "(?m)^SET $($kv[0]) `"[^`"]*`"", $want }
    else { $text = $text.TrimEnd() + "`r`n$want`r`n" }
}
[IO.File]::WriteAllText($config, $text)

if (-not $NoLogin) {
    & (Join-Path $tool 'Login.ps1') -Character $Character -Client $Client
}
