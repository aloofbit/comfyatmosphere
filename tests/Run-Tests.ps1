<#
.SYNOPSIS
  Runs comfyatmosphere's tests in the WoW test client and says which pass.

.DESCRIPTION
  A test is a .json file in this folder: a config (where to start, flight, camera), a list of steps, and what
  to expect from the probe. The runner logs in with wow-test-tool's Login.ps1, turns the config and the
  steps into a script for its Run-Test.ps1, runs it, and reads comfyfog.log. Each result, the script and the
  screenshot taken go to results\.

  Config:
    character   the slot on character select, from the top (Luf is 1)
    flight      true: type "/cast Toggle GM Flight Mode" unless an earlier test of the run turned it on (it is off
                after a login); an addon's CastSpellByName does not reach it
    camera      0 first person, 1 to 9 that many steps back out, 10 all the way out ("zoomed" is 0, "far" 10)
    flySpeed    yards a second while flying with W, to turn "fly" yards into seconds (7 is the run speed)
    start       { map, x, y, z }: where the test begins, by .go xyz
    sun         { azimuth, elevation } in degrees: a fixed sun ([sun] fixed), the same light every run
    ini         { "section.key": value, ... }: comfyfog.ini values for this test, by /atmos. /atmos reset runs
                before and after, so nothing stays set
    debugView   the Debug View to show (the comfyDebugView control)
    cvars       { "name": value, ... }: the Atmosphere page's controls, which /atmos refuses. Every CVar a test
                sets, by cvars, debugView or a cvar step, goes back to its value before at the end (ComfyTest's
                cvarsback): the game saves them in Config.wtf

  Steps, one key each:
    wait <s>, hop { to, heading, step, pause }, fly <yards>, flyFor <seconds>, back <yards>, turn <degrees> (180: the client's Ctrl+Shift+F flip; other angles hold Q, 180 a second), probe, screenshot,
    face { heading, pitch } (the camera, in degrees: heading counter-clockwise from +x as the sun's azimuth, pitch up positive; ComfyTest checks and corrects it),
    pos, atmos "<words>", cvar "<name> <value>", chat "<text>". Where the character is (from comfyfog.dll's
    comfyStats CVar) goes into the result after the start and after each fly, back and turn.

  Expect, each checked against the last probe:
    probe   a pattern (a regular expression) for lines of the probe
    max     at most this many lines may match (0: none)
    min     at least this many must match
  or against a screenshot of the run:
    shot    which one, from 1 (in the order taken)
    row     a row of pixels, from the top, and from, to: the columns along it (every 4th is read)
    min     no pixel's brightness (0..255, the mean of red, green and blue) under this
    mean    the row's average brightness at least this
  or against a box of a screenshot:
    shot    which one, from 1
    box     [ left, top, right, bottom ] in pixels (every 2nd row is read)
    jumpMax the share of pixels allowed, in percent, whose brightness differs from the next one along the row
            by more than 60: speckle in the shade-alone view (Debug View 5) jumps, even shade or light does not

.EXAMPLE
  .\Run-Tests.ps1                         # every test here
  .\Run-Tests.ps1 far-terrain-cache       # one test
  .\Run-Tests.ps1 far-terrain-cache -NoLogin   # the client is already in the world
#>
param(
    [Parameter(Position = 0)][string[]]$Name,
    [switch]$NoLogin,
    [string]$Client = (Join-Path $env:USERPROFILE 'Desktop\wow-clients\octow - Copy')
)

$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$tool = (Resolve-Path (Join-Path $here '..\..\..\tools\wow-test-tool')).Path
$resultsDir = Join-Path $here 'results'
New-Item -ItemType Directory -Force $resultsDir | Out-Null
if ($Client -match '\\octow$') { throw 'That is the live client. Point -Client at the test client.' }

$files = if ($Name) { $Name | ForEach-Object { Join-Path $here "$_.json" } } else { Get-ChildItem $here -Filter *.json | ForEach-Object { $_.FullName } }
$log = Join-Path $Client 'comfyfog.log'
$summary = @()
# One login for the whole run (2026-10-04): a login a test cost about a minute each. Another login only for a
# test that wants another character. Flight is a toggle, so the runner keeps track of it: on after a test
# turned it on, off after a login. With -NoLogin it is taken as off.
$loggedAs = $null
$flying = $false
# Flight off: the same cast. In the air the character falls, which a GM survives; between tests the next
# start's .go xyz follows at once.
$flightOff = @('type /cast Toggle GM Flight Mode', 'wait 0.5')

foreach ($file in $files) {
    if (-not (Test-Path $file)) { throw "No test $file" }
    $t = Get-Content $file -Raw | ConvertFrom-Json
    $cfg = $t.config
    $stamp = (Get-Date).ToString('yyyyMMdd-HHmmss')
    Write-Host "=== $($t.name) ==="

    $slot = if ($cfg.character) { [int]$cfg.character } else { 1 }
    if (-not $NoLogin -and $loggedAs -ne $slot) {
        & (Join-Path $tool 'Login.ps1') -Restart -Character $slot -Client $Client
        $loggedAs = $slot
        $flying = $false
    }

    # The config and the steps into a wow-test-tool script.
    $speed = if ($cfg.flySpeed) { [double]$cfg.flySpeed } else { 7.0 }
    $lines = @("# $($t.name), made by Run-Tests.ps1 from $([IO.Path]::GetFileName($file))")
    # Flight first, then the start: a start in the air holds only with flight on. The character must be on the
    # ground when flight goes on, or it can stick in the air (the owner, 2026-10-03): wow-test-tool's ground waits
    # until its height has stayed on the map files' ground for half a second.
    # Each test in the flight state it asks for.
    if ($cfg.flight -and -not $flying) {
        $lines += 'ground 60'; $lines += 'type /cast Toggle GM Flight Mode'; $lines += 'wait 0.5'
        $flying = $true
    }
    elseif (-not $cfg.flight -and $flying) {
        $lines += $flightOff
        $flying = $false
    }
    if ($cfg.start) {
        $s = $cfg.start
        $lines += "chat .go xyz $($s.x) $($s.y) $($s.z) $($s.map)"
        $lines += 'wait 5'
    }
    $lines += 'wait 1.2'; $lines += 'pos start'   # the place comes from comfyStats, up to a second old
    # comfyfog's values: none left from before, then this test's.
    $lines += 'atmos reset'
    if ($cfg.sun) {
        $lines += 'atmos sun.fixed 1'
        $lines += "atmos sun.azimuth $($cfg.sun.azimuth)"
        $lines += "atmos sun.elevation $($cfg.sun.elevation)"
    }
    if ($cfg.ini) {
        foreach ($p in $cfg.ini.PSObject.Properties) { $lines += "atmos $($p.Name) $($p.Value)" }
    }
    if ($cfg.cvars) {
        foreach ($p in $cfg.cvars.PSObject.Properties) { $lines += "cvar $($p.Name) $($p.Value)" }
    }
    if ($null -ne $cfg.debugView) { $lines += "cvar comfyDebugView $($cfg.debugView)" }
    # The camera by a number (the owner, 2026-10-03): 0 first person, 1 to 9 that many steps back out from it,
    # 10 all the way out. "zoomed" is 0 and "far" is 10.
    if ($null -ne $cfg.camera) {
        $cam = if ($cfg.camera -eq 'zoomed') { 0 } elseif ($cfg.camera -eq 'far') { 10 } else { [int]$cfg.camera }
        $lines += 'zoom -30'
        if ($cam -ge 10) { $lines += 'zoom 30' } elseif ($cam -gt 0) { $lines += "zoom $cam" }
    }
    # hop: .go xyz along a line from the start at the start's height, in steps (2026-10-03). Exact where turning
    # and flight speed were not. $along is how far along the line the last hop left the character.
    $along = 0.0
    foreach ($st in $t.steps) {
        $p = $st.PSObject.Properties | Select-Object -First 1
        $v = $p.Value
        switch ($p.Name) {
            'wait'       { $lines += "wait $v" }
            'fly'        { $lines += ('hold W {0:0.##}' -f ([double]$v / $speed)); $lines += 'wait 1.2'; $lines += "pos after fly $v" }
            'hop'        {
                if (-not $cfg.start) { throw "$($t.name): hop needs a start" }
                $h = [double]$v.heading * [math]::PI / 180.0
                $stepLen = if ($v.step) { [double]$v.step } else { 10.0 }
                $pause = if ($null -ne $v.pause) { [double]$v.pause } else { 0.3 }
                $to = [double]$v.to
                $dir = if ($to -ge $along) { 1.0 } else { -1.0 }
                while ([math]::Abs($to - $along) -gt 0.01) {
                    $along += $dir * [math]::Min($stepLen, [math]::Abs($to - $along))
                    $x = [double]$cfg.start.x + [math]::Cos($h) * $along
                    $y = [double]$cfg.start.y + [math]::Sin($h) * $along
                    $lines += ('chat .go xyz {0:0.0} {1:0.0} {2:0.0} {3}' -f $x, $y, [double]$cfg.start.z, $cfg.start.map)
                    $lines += "wait $pause"
                }
                $lines += 'wait 1.2'; $lines += "pos after hop to $to"
            }
            'flyFor'     { $lines += "hold W $v"; $lines += 'wait 1.2'; $lines += "pos after flying $v s" }   # seconds, not yards: two legs of the same time cover the same ground
            'back'       { $lines += ('hold S {0:0.##}' -f ([double]$v / ($speed * 0.64))); $lines += 'wait 1.2'; $lines += "pos after back $v" }   # backing up is 64% of the speed
            'turn'       { if ([double]$v -eq 180) { $lines += 'keys ctrl+shift+f' } else { $lines += ('hold Q {0:0.##}' -f ([double]$v / 180.0)) }; $lines += 'wait 1.2'; $lines += "pos after turn $v" }               # the keys turn 180 degrees a second
            'pos'        { $lines += 'wait 1.2'; $lines += 'pos' }
            'face'       { $lines += "face $($v.heading)"; if ($null -ne $v.pitch) { $lines += "pitch $($v.pitch)" } }   # the camera, checked against comfyStats: ComfyTest turns, wow-test-tool tilts
            'probe'      { $lines += 'atmos probe' }
            'screenshot' { $lines += 'keys alt+z'; $lines += 'wait 0.2'; $lines += 'screenshot'; $lines += 'keys alt+z' }   # without the UI (Alt+Z), then the UI back
            'atmos'      { $lines += "atmos $v" }
            'cvar'       { $lines += "cvar $v" }
            'chat'       { $lines += "chat $v" }
            default      { throw "$($t.name): unknown step '$($p.Name)'" }
        }
    }
    # Put back what the test set.
    $lines += 'cvarsback'
    $lines += 'atmos reset'
    $script = Join-Path $resultsDir "$stamp-$($t.name).script.txt"
    [IO.File]::WriteAllText($script, ($lines -join "`r`n") + "`r`n")

    $logStart = if (Test-Path $log) { @(Get-Content $log).Count } else { 0 }
    $started = Get-Date
    $runOut = & (Join-Path $tool 'Run-Test.ps1') $script -Client $Client 6>&1 | Out-String
    $positions = @($runOut -split "`r?`n" | Where-Object { $_ -match '^(pos|face|pitch) ' })
    $warnings = @($runOut -split "`r?`n" | Where-Object { $_ -match '^error ' })

    # The last probe of this run: from its header to the next report or the end.
    $new = if (Test-Path $log) { @(Get-Content $log | Select-Object -Skip $logStart) } else { @() }
    $heads = @(for ($i = 0; $i -lt $new.Count; $i++) { if ($new[$i] -match '^=== client report \((F12|/atmos probe)\) ===') { $i } })
    $probe = @()
    if ($heads.Count) {
        $from = $heads[-1]
        $to = $new.Count
        for ($i = $from + 1; $i -lt $new.Count; $i++) { if ($new[$i] -match '^=== client report') { $to = $i; break } }
        $probe = $new[$from..($to - 1)]
    }

    # Every screenshot of the run, in order: a test can take one in each debug view.
    $shots = @(Get-ChildItem (Join-Path $Client 'Screenshots') -File -ErrorAction SilentlyContinue |
               Where-Object { $_.LastWriteTime -ge $started } | Sort-Object LastWriteTime)

    $results = @()
    $pass = $true
    foreach ($e in $t.expect) {
        if ($null -ne $e.shot -and $null -ne $e.box) {
            # A box of a screenshot (2026-10-04): how much of it jumps from one pixel to the next. Counting
            # half-grey pixels missed speckle that came as hard black and white dots.
            $n = [int]$e.shot
            if ($n -lt 1 -or $n -gt $shots.Count) { $results += "FAIL  no screenshot $n ($($shots.Count) taken): $($e.about)"; $pass = $false; continue }
            Add-Type -AssemblyName System.Drawing
            $bmp = [Drawing.Bitmap]::FromFile($shots[$n - 1].FullName)
            $jumps = 0; $count = 0
            try {
                for ($y = [int]$e.box[1]; $y -le [int]$e.box[3] -and $y -lt $bmp.Height; $y += 2) {
                    for ($x = [int]$e.box[0]; $x -lt [int]$e.box[2] -and $x + 1 -lt $bmp.Width; $x++) {
                        $p = $bmp.GetPixel($x, $y); $q = $bmp.GetPixel($x + 1, $y)
                        if ([Math]::Abs(($p.R + $p.G + $p.B) - ($q.R + $q.G + $q.B)) / 3 -gt 60) { $jumps++ }
                        $count++
                    }
                }
            }
            finally { $bmp.Dispose() }
            $share = if ($count) { [int](100 * $jumps / $count) } else { 0 }
            $ok = $share -le [int]$e.jumpMax
            if (-not $ok) { $pass = $false }
            $results += ('{0}  {1}: {2}% of the box jumps from one pixel to the next (at most {3}%)' -f $(if ($ok) { 'pass' } else { 'FAIL' }), $e.about, $share, $e.jumpMax)
            continue
        }
        if ($null -ne $e.shot) {
            # A row of a screenshot (2026-10-04): the Debug View 5 shot is one grey where the shade is even, and
            # a strip of shade that should not be there shows as a dip.
            $n = [int]$e.shot
            if ($n -lt 1 -or $n -gt $shots.Count) { $results += "FAIL  no screenshot $n ($($shots.Count) taken): $($e.about)"; $pass = $false; continue }
            Add-Type -AssemblyName System.Drawing
            $bmp = [Drawing.Bitmap]::FromFile($shots[$n - 1].FullName)
            $low = 255; $at = -1; $sum = 0; $count = 0
            try {
                for ($x = [int]$e.from; $x -le [int]$e.to -and $x -lt $bmp.Width; $x += 4) {
                    $c = $bmp.GetPixel($x, [int]$e.row)
                    $l = [int](($c.R + $c.G + $c.B) / 3)
                    if ($l -lt $low) { $low = $l; $at = $x }
                    $sum += $l; $count++
                }
            }
            finally { $bmp.Dispose() }
            $avg = if ($count) { [int]($sum / $count) } else { 0 }
            $ok = ($null -eq $e.min -or $low -ge [int]$e.min) -and ($null -eq $e.mean -or $avg -ge [int]$e.mean)
            if (-not $ok) { $pass = $false }
            $results += ('{0}  {1}: row {2} averages {3}, its darkest pixel {4} at column {5} (wanted: average {6}, darkest {7})' -f $(if ($ok) { 'pass' } else { 'FAIL' }), $e.about, $e.row, $avg, $low, $at, $(if ($null -ne $e.mean) { "at least $($e.mean)" } else { 'any' }), $(if ($null -ne $e.min) { "at least $($e.min)" } else { 'any' }))
            continue
        }
        if (-not $heads.Count) { $results += "FAIL  no probe in the log: $($e.about)"; $pass = $false; continue }
        $hits = @($probe | Where-Object { $_ -match $e.probe })
        $ok = $true
        if ($null -ne $e.max -and $hits.Count -gt [int]$e.max) { $ok = $false }
        if ($null -ne $e.min -and $hits.Count -lt [int]$e.min) { $ok = $false }
        if (-not $ok) { $pass = $false }
        $results += ('{0}  {1}: {2} line(s) match' -f $(if ($ok) { 'pass' } else { 'FAIL' }), $e.about, $hits.Count)
        $results += @($hits | Select-Object -First 10 | ForEach-Object { "        $_" })
    }

    $shotCopies = @()
    for ($i = 0; $i -lt $shots.Count; $i++) {
        $suffix = if ($shots.Count -gt 1) { "-$($i + 1)" } else { '' }
        $copy = Join-Path $resultsDir "$stamp-$($t.name)$suffix$($shots[$i].Extension)"
        Copy-Item $shots[$i].FullName $copy
        $shotCopies += $copy
    }

    $verdict = if ($pass) { 'PASS' } else { 'FAIL' }
    $report = @("$verdict  $($t.name)  ($stamp)") + $results
    if ($positions.Count) { $report += 'where the character was:'; $report += @($positions | ForEach-Object { "        $_" }) }
    if ($warnings.Count) { $report += 'errors from the game:'; $report += @($warnings | ForEach-Object { "        $_" }) }
    if ($shotCopies.Count) { $report += @($shotCopies | ForEach-Object { "screenshot: $_" }) } else { $report += 'screenshot: none taken' }
    $text = $report -join "`r`n"
    Write-Host $text
    [IO.File]::WriteAllText((Join-Path $resultsDir "$stamp-$($t.name).txt"), $text + "`r`n`r`n" + ($probe -join "`r`n") + "`r`n")
    $summary += "$verdict  $($t.name)"
}

# The character is left as a login leaves it: flight off.
if ($flying) {
    $script = Join-Path $resultsDir "$((Get-Date).ToString('yyyyMMdd-HHmmss'))-flight-off.script.txt"
    [IO.File]::WriteAllText($script, ($flightOff -join "`r`n") + "`r`n")
    & (Join-Path $tool 'Run-Test.ps1') $script -Client $Client 6>&1 | Out-Null
}

Write-Host ''
Write-Host ($summary -join "`r`n")
