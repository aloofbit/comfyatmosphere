<#
.SYNOPSIS
  Puts the character at a test's start, and does nothing else.

.DESCRIPTION
  One .go xyz to the test's config.start, sent through wow-test-tool to the running test client. Flight, the
  camera, the settings and the weather stay as they are (2026-10-09, the owner: "take me to the test" means the
  place alone). A start in the air with flight off falls to the ground under it.

.EXAMPLE
  .\Go-Test.ps1 development-windmill
#>
param(
    [Parameter(Mandatory, Position = 0)][string]$Name,
    [string]$Client = (Join-Path $env:USERPROFILE 'Desktop\wow-clients\octow - Copy')
)

$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$tool = $null
for ($up = Split-Path $here; $up -and -not $tool; $up = Split-Path $up) {
    if (Test-Path (Join-Path $up 'tools\wow-test-tool\Run-Test.ps1')) { $tool = Join-Path $up 'tools\wow-test-tool' }
}
if (-not $tool) { throw "No tools\wow-test-tool in any folder over $here." }
if ($Client -match '\\octow$') { throw 'That is the live client. Point -Client at the test client.' }

$file = Join-Path $here "$Name.json"
if (-not (Test-Path $file)) { throw "No test $file" }
$s = (Get-Content $file -Raw | ConvertFrom-Json).config.start
if (-not $s) { throw "$Name has no config.start" }

$script = Join-Path $env:TEMP 'wow-test-go.txt'
[IO.File]::WriteAllText($script, "chat .go xyz $($s.x) $($s.y) $($s.z) $($s.map)`r`n")
& (Join-Path $tool 'Run-Test.ps1') $script -Client $Client 6>&1 | Out-Null
Write-Host "At $Name's start: $($s.x) $($s.y) $($s.z), map $($s.map)."
