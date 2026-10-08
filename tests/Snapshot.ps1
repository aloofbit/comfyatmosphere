<#
.SYNOPSIS
  Reads the owner's whole setup from the running test client, as a test's config (2026-10-04).

.DESCRIPTION
  When the owner says "snapshot", this is run while they stand where a test is to start. It reads, without
  moving anything:
    - the place and the map (ComfyTest pos, the log's map name, Map.dbc for its id);
    - the camera: heading, pitch and distance (ComfyTest look, from comfyatmos.dll's comfyStats);
    - flight: on when the character is more than 3 yards over the ground under it, and not swimming (at or
      under the water's level, from the probe's water line);
    - the sun the shadows use (the probe's sunshadows line);
    - the hour the sky shows (the probe's night line, which [time] sets);
    - every Atmosphere page control, as last set (the log's "--- control:" lines), Debug View left out.
  It prints the config and saves it to results\<time>-snapshot.json. A test starts from that block.

.EXAMPLE
  .\Snapshot.ps1
#>
param(
    [string]$Client = (Join-Path $env:USERPROFILE 'Desktop\wow-clients\octow - Copy')
)

$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
# wow-test-tool: comfy-wow's tools folder, the first one up from here (2026-10-08), as Run-Tests.ps1.
$tool = $null
for ($up = Split-Path $here; $up -and -not $tool; $up = Split-Path $up) {
    if (Test-Path (Join-Path $up 'tools\wow-test-tool\Run-Test.ps1')) { $tool = Join-Path $up 'tools\wow-test-tool' }
}
if (-not $tool) { throw "No tools\wow-test-tool in any folder over $here." }
if ($Client -match '\\octow$') { throw 'That is the live client. Point -Client at the test client.' }
# comfyatmos.log: in the client's Logs folder since 2026-10-06, in the client folder before. The newest is this run's.
$log = @('Logs\comfyatmos.log', 'comfyatmos.log') | ForEach-Object { Join-Path $Client $_ } | Where-Object { Test-Path $_ } |
       Sort-Object { (Get-Item $_).LastWriteTime } -Descending | Select-Object -First 1
if (-not $log) { $log = Join-Path $Client 'Logs\comfyatmos.log' }

$script = Join-Path $env:TEMP 'comfy-snapshot.txt'
[IO.File]::WriteAllText($script, "pos snapshot`r`nlook`r`natmos probe`r`nwait 3`r`n")
$out = & (Join-Path $tool 'Run-Test.ps1') $script -Client $Client 6>&1 | Out-String

$pos = [regex]::Match($out, 'pos\s+(-?[\d.]+)\s+(-?[\d.]+)\s+(-?[\d.]+)\s+(-?[\d.]+)\s+snapshot')
$look = [regex]::Match($out, 'look\s+(-?[\d.]+)\s+(-?[\d.]+)(\s+([\d.]+))?')
if (-not $pos.Success -or -not $look.Success) { throw "No place or camera from the game:`n$out" }
$x, $y, $z, $gz = 1..4 | ForEach-Object { [double]$pos.Groups[$_].Value }
$yaw = [double]$look.Groups[1].Value
$pitch = [double]$look.Groups[2].Value

$lines = Get-Content $log
# The camera's distance: comfyStats' camdist, or else the probe's depth line.
$dist = if ($look.Groups[4].Success) { [double]$look.Groups[4].Value } else { $null }
if ($null -eq $dist) {
    $d = $lines | Where-Object { $_ -match '^depth: last frame' } | Select-Object -Last 1
    if ($d -match 'the camera ([\d.]+) yd from you') { $dist = [double]$Matches[1] }
}
# The sun the shadows used.
$sun = $lines | Where-Object { $_ -match '^sunshadows: drawn' } | Select-Object -Last 1
$az = 45.0; $el = 45.0
if ($sun -match 'sun \((-?[\d.]+) (-?[\d.]+) (-?[\d.]+)\)') {
    $sx = [double]$Matches[1]; $sy = [double]$Matches[2]; $sz = [double]$Matches[3]
    $az = [math]::Round([math]::Atan2($sy, $sx) * 180 / [math]::PI, 1)
    $el = [math]::Round([math]::Atan2($sz, [math]::Sqrt($sx * $sx + $sy * $sy)) * 180 / [math]::PI, 1)
}
# The hour the sky showed (2026-10-07): the probe's game time, which [time] sets. A test sets it again with /atmos
# time.hour (config hour); without it a lighthouse test taken at night ran by day.
$hour = $null
$night = $lines | Where-Object { $_ -match '^night: game time (\d+):(\d+)' } | Select-Object -Last 1
if ($night -match 'game time (\d+):(\d+)') { $hour = [math]::Round([double]$Matches[1] + [double]$Matches[2] / 60, 2) }
# The map: its name in the log, its id from Map.dbc.
$mapName = ($lines | Where-Object { $_ -match 'map terrain: map "([^"]+)"' } | Select-Object -Last 1) -replace '.*map terrain: map "([^"]+)".*', '$1'
$mapId = @{ 'Azeroth' = 0; 'Kalimdor' = 1; 'development' = 451 }[$mapName]
if ($null -eq $mapId) {
    $js = "const { Dbc } = require('./lib/dbc.js'); const m = Dbc.open('Map.dbc'); for (let i = 0; i < m.recordCount; i++) if (m.str(i, 1) === '$mapName') console.log(m.uint(i, 0));"
    Push-Location (Join-Path $tool '..\model-browser'); $mapId = & node -e $js; Pop-Location
}
# The controls, each as last set.
$cvars = [ordered]@{}
foreach ($l in $lines) {
    if ($l -match '^--- control: (comfy\w+) = (-?[\d.]+) ---' -and $Matches[1] -ne 'comfyDebugView') { $cvars[$Matches[1]] = [double]$Matches[2] }
}

# The camera's own distance (2026-10-05): the field the mouse wheel moves, from comfytest.dll. A test sets it
# exactly (cameraZoom); camdist measures to the feet and changes with the pitch, which close in was a notch off.
Import-Module (Join-Path $tool 'Robot.psm1') -Force -DisableNameChecking
$zoom = $null
if ((Send-ComfyTest 'camera') -match 'EC=([\d.]+)') { $zoom = [math]::Round([double]$Matches[1], 3) }

# Swimming (2026-10-07): the probe's water line gives the water's level where the character is. A swimmer at the
# surface sits about 1.2 yards under it, and over deep water it is many yards over the ground: taken by its height
# alone, a swimmer off the Darkshore coast, 23 yards over the sea floor, came out as flying, and the runner then
# waited for ground before it turned flight on.
$swim = $false
$wl = $lines | Where-Object { $_ -match '^water: you stand at' } | Select-Object -Last 1
if ($wl -match 'water at (-?[\d.]+)') { $swim = $z -le [double]$Matches[1] + 1.0 }

$camera = if ($null -ne $dist -and $dist -lt 1.5) { 0 } else { $null }
$config = [ordered]@{
    character = 1
    flight    = -not $swim -and ($z - $gz) -gt 3.0
}
if ($null -ne $zoom) { $config.cameraZoom = $zoom }
elseif ($null -ne $camera) { $config.camera = 0 } elseif ($null -ne $dist) { $config.cameraDistance = [math]::Round($dist, 1) }
$config.start = [ordered]@{
    map = [int]$mapId; x = $x; y = $y; z = $z; facing = [math]::Round($yaw, 1)
    about = ('the owner''s snapshot: {6}{0:0.0} yards over the ground ({1:0.0}); the camera faced {2:0.0} degrees, {3:0.0} {4}, {5} yards back; the sun from their probe' -f ($z - $gz), $gz, $yaw, [math]::Abs($pitch), $(if ($pitch -lt 0) { 'down' } else { 'up' }), $(if ($null -ne $dist) { '{0:0.0}' -f $dist } else { '?' }), $(if ($swim) { 'swimming, ' } else { '' }))
}
$config.sun = [ordered]@{ azimuth = $az; elevation = $el }
if ($null -ne $hour) { $config.hour = $hour }
$config.cvars = $cvars
$snap = [ordered]@{ config = $config; face = [ordered]@{ heading = [math]::Round($yaw, 1); pitch = [math]::Round($pitch, 1) } }

$json = $snap | ConvertTo-Json -Depth 5
$file = Join-Path $here ("results\{0}-snapshot.json" -f (Get-Date).ToString('yyyyMMdd-HHmmss'))
[IO.File]::WriteAllText($file, $json)
$json
"saved: $file"
# A swimmer brought to the start sinks about 0.6 yards: a jump step brings it up (darkshore-underwater-npcs).
if ($swim) { 'swimming: start the test''s steps with { "jump": 1 }' }
