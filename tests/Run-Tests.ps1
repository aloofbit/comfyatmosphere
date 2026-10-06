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
    flightFrom  { x, y, z }: with flight, where it is turned on, on land, before the start (2026-10-06): a start in
                the air over deep water dropped the character into the water, where the ground wait never ends
    sun         { azimuth, elevation } in degrees: a fixed sun ([sun] fixed), the same light every run
    ini         { "section.key": value, ... }: comfyfog.ini values for this test, by /atmos. /atmos reset runs
                before and after, so nothing stays set
    debugView   the Debug View to show (the comfyDebugView control)
    cvars       { "name": value, ... }: the Atmosphere page's controls, which /atmos refuses. Every CVar a test
                sets, by cvars, debugView or a cvar step, goes back to its value before at the end (ComfyTest's
                cvarsback): the game saves them in Config.wtf

  Steps, one key each:
    wait <s>, hop { to, heading, step, pause }, fly <yards>, flyFor <seconds>, back <yards>, turn <degrees> (to the left, by face), probe, screenshot, target <name>, clearTarget,
    face { heading, pitch } (the camera, in degrees: heading counter-clockwise from +x as the sun's azimuth, pitch up positive; ComfyTest checks and corrects it),
    record { seconds, label } (a video without the UI, by ffmpeg, shown on the page and played at once),
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
  .\Run-Tests.ps1 -Accept                 # and take this run's screenshots as the expected ones

  Each run writes a page, results\<time>-report.html, and opens it (-NoOpen does not): every test, its checks,
  and each screenshot beside the expected one in expected\<test>-<n>.jpg. results\last-results.html is always the
  newest run's page: keep a tab open on it and refresh.
#>
param(
    [Parameter(Position = 0)][string[]]$Name,
    [switch]$NoLogin,
    [switch]$Accept,
    [switch]$NoOpen,
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
# test that wants another character. Flight is read before each test (2026-10-05, wow-test-tool's flight on and
# flight off, from comfytest.dll): a toggle the runner kept track of lost step, and a test started with it off.
$loggedAs = $null
$runStamp = (Get-Date).ToString('yyyyMMdd-HHmmss')
$runStarted = Get-Date
$expectedDir = Join-Path $here 'expected'
$records = @()
# The Debug Views' names, for the page, from the list the DLL keeps (kDebugViews in cvars.cpp).
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
# Each part of a test is named in the game's chat as it starts (the owner, 2026-10-05): "wow-test: <what>",
# shown in that client alone (ComfyTest's say).
function Say([string]$what) { "say $what" }

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
    }

    # The config and the steps into a wow-test-tool script.
    $speed = if ($cfg.flySpeed) { [double]$cfg.flySpeed } else { 7.0 }
    $lines = @("# $($t.name), made by Run-Tests.ps1 from $([IO.Path]::GetFileName($file))")
    $lines += Say "$($t.name): starting"
    # GM mode on first (the owner, 2026-10-05): a hostile creature, summoned or near the start, leaves the
    # character alone.
    $lines += 'chat .gm on'
    $lines += 'wait 0.5'
    # Flight first, then the start: a start in the air holds only with flight on. The character must be on the
    # ground when flight goes on, or it can stick in the air (the owner, 2026-10-03): wow-test-tool's ground waits
    # until its height has stayed on the map files' ground for half a second. flight on and flight off read the
    # state and cast only when it is the other way, and ground unlessflying does not wait when flight is on.
    if ($cfg.flight) {
        $lines += Say 'flight on'
        # To the start first (2026-10-04): the test before may leave the character swimming, where the ground
        # wait never ends. From a start in the air it falls to the ground under it; a GM takes no harm.
        # Or to flightFrom, on land, when the start is over deep water (2026-10-06).
        $f = if ($cfg.flightFrom) { $cfg.flightFrom } else { $cfg.start }
        if ($f) { $lines += "chat .go xyz $($f.x) $($f.y) $($f.z) $($cfg.start.map)"; $lines += "arrive $($f.x) $($f.y) $($f.z)" }
        $lines += 'hold W 0.5'   # a character left in the air with flight off falls only once it moves
        $lines += 'ground 60 unlessflying'; $lines += 'flight on'
    }
    else {
        $lines += Say 'flight off'
        $lines += 'flight off'
    }
    # Creatures the test needs (2026-10-04): summoned where each stands (.npc summon puts it at the player), on the
    # start's map, before the start. Each is deleted after the steps (below). Only a creature with no world
    # spawn: the delete goes by its name.
    if ($cfg.summon) {
        foreach ($u in $cfg.summon) {
            $lines += "chat .go xyz $($u.x) $($u.y) $($u.z) $($cfg.start.map)"
            $lines += "arrive $($u.x) $($u.y) $($u.z)"
            $lines += "chat .npc summon $($u.entry)"
            $lines += 'wait 1'
        }
    }
    if ($cfg.start) {
        $s = $cfg.start
        $lines += Say 'going to the start'
        $lines += "chat .go xyz $($s.x) $($s.y) $($s.z) $($s.map)"
        # Until the character is there, then 1 s (2026-10-05): it was 5 s every time, and the tool's arrive line
        # notes the place, so the pos line after it went too. Without comfytest.dll, arrive waits 5 s.
        $lines += "arrive $($s.x) $($s.y) $($s.z)"
        # The character's heading (2026-10-04): .go xyz keeps the old one. Turned by right-drags, the camera
        # with it (2026-10-05): the camera turned alone and a right-click did not bring the character after it.
        if ($null -ne $s.facing) { $lines += "heading $($s.facing)" }
    }
    if (-not $cfg.start) { $lines += 'wait 1.2'; $lines += 'pos start' }   # the place comes from comfyStats, up to a second old
    # No weather (the owner, 2026-10-05): rain or snow changes the light and the fog from run to run. The
    # weather is the zone's, so it is cleared here, at the start.
    $lines += Say 'clearing the weather'
    $lines += 'chat .wchange 0 0'
    # comfyfog's values: none left from before, then this test's.
    $lines += Say 'setting the snapshot: the sun and the controls'
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
    # The camera by a number (the owner, 2026-10-03): 0 first person, 1 to 9 that many notches of the mouse wheel
    # back out from it, 10 all the way out. "zoomed" is 0 and "far" is 10. By the wheel since 2026-10-04: zoom out
    # (CameraZoomOut) went all the way out whatever its number, so every test before then ran at 10. A notch: 1
    # put the camera 2.5 yards back, 2 at 3.4, 4 at 5.3.
    # Or by yards (cameraDistance, 2026-10-04): as far back as the owner had it, from a snapshot.
    # Or the camera's own distance (cameraZoom, 2026-10-05), the field the wheel moves, set exactly by
    # comfytest.dll: close in, camdist was a notch off.
    if ($null -ne $cfg.cameraZoom -or $null -ne $cfg.cameraDistance -or $null -ne $cfg.camera) { $lines += Say 'moving the camera' }
    if ($null -ne $cfg.cameraZoom) { $lines += "camzoom $($cfg.cameraZoom)" }
    elseif ($null -ne $cfg.cameraDistance) { $lines += "camdist $($cfg.cameraDistance)" }
    elseif ($null -ne $cfg.camera) {
        $cam = if ($cfg.camera -eq 'zoomed') { 0 } elseif ($cfg.camera -eq 'far') { 10 } else { [int]$cfg.camera }
        $lines += 'zoom -30'
        if ($cam -ge 10) { $lines += 'zoom 30' } elseif ($cam -gt 0) { $lines += 'wait 1'; $lines += "wheel $cam" }
    }
    # hop: .go xyz along a line from the start at the start's height, in steps (2026-10-03). Exact where turning
    # and flight speed were not. $along is how far along the line the last hop left the character.
    $along = 0.0
    # What each screenshot shows, for the page: the Debug View at the time.
    $view = if ($null -ne $cfg.debugView) { [int]$cfg.debugView } else { 0 }
    $shotViews = @()
    $recViews = @(); $recLabels = @()
    $nShot = 0; $nRec = 0; $nProbe = 0
    foreach ($st in $t.steps) {
        $p = $st.PSObject.Properties | Select-Object -First 1
        $v = $p.Value
        switch ($p.Name) {
            'face'       { $lines += Say "facing $($v.heading)$(if ($null -ne $v.pitch) { ", pitch $($v.pitch)" })" }
            'hop'        { $lines += Say "hopping to $($v.to) yards" }
            'fly'        { $lines += Say "flying $v yards" }
            'flyFor'     { $lines += Say "flying for $v s" }
            'back'       { $lines += Say "backing $v yards" }
            'turn'       { $lines += Say "turning $v degrees" }
            'probe'      { $nProbe++; $lines += Say "probe $nProbe" }
            'screenshot' { $nShot++; $lines += Say "screenshot $nShot" }
            'record'     {
                $nRec++
                $secs = if ($null -ne $v.seconds) { $v.seconds } else { $v }
                $lines += Say "recording $nRec, $secs s$(if ($v.label) { ": $($v.label)" })"
            }
            'cvar'       { $lines += Say "control: $v" }
            'atmos'      { $lines += Say "/atmos $v" }
            'chat'       { $lines += Say "chat: $v" }
            'target'     { $lines += Say "targeting $v" }
            'camera'     { $lines += Say 'moving the camera' }
            'jump'       { $lines += Say 'jumping' }
        }
        switch ($p.Name) {
            'wait'       { $lines += "wait $v" }
            'jump'       { for ($j = 0; $j -lt [int]$v; $j++) { $lines += 'jump'; $lines += 'wait 1.2' } }   # in the water, it brings a swimmer up to the surface
            'down'       { $lines += "down $v" }   # hold a key until its up step: the steps between run while it is held
            'up'         { $lines += "up $v" }
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
            'turn'       { $lines += "turnby $v"; $lines += 'wait 1.2'; $lines += "pos after turn $v" }   # to the left; by face, not Ctrl+Shift+F, which broke the camera (2026-10-05)
            'pos'        { $lines += 'wait 1.2'; $lines += 'pos' }
            'face'       { $lines += "heading $($v.heading)"; if ($null -ne $v.pitch) { $lines += "pitch $($v.pitch)$(if ($v.leftDrag) { ' left' })" } }   # the character and the camera, checked against comfyStats: right-drags turn, wow-test-tool tilts
            'probe'      { $lines += 'atmos probe' }
            'screenshot' { $shotViews += $view; $lines += 'ui hide'; $lines += 'wait 0.2'; $lines += 'screenshot'; $lines += 'ui show' }   # without the UI, then the UI back (Alt+Z without comfytest.dll)
            'record'     {   # a video, without the UI as a screenshot: { seconds, label } or the seconds alone
                $secs = if ($null -ne $v.seconds) { [double]$v.seconds } else { [double]$v }
                $recLabels += $(if ($v.label) { $v.label } else { '' })
                $recViews += $view
                # With start (2026-10-05), the steps after it run while it records, until recordWait: to record a
                # change those steps make, such as a stealth aura going on.
                if ($v.start) { $lines += 'ui hide'; $lines += 'wait 0.2'; $lines += "record $secs start" }
                else { $lines += 'ui hide'; $lines += 'wait 0.2'; $lines += "record $secs"; $lines += 'ui show' }
            }
            'recordWait' { $lines += 'recordwait'; $lines += 'ui show' }
            'atmos'      { $lines += "atmos $v" }
            'cvar'       { $lines += "cvar $v"; if ("$v" -match '^comfyDebugView\s+(\d+)') { $view = [int]$Matches[1] } }
            'chat'       { $lines += "chat $v" }
            'type'       { $lines += "type $v" }
            'target'      { $lines += "target $v" }   # the unit with that exact name, as /target (ComfyTest)
            'clearTarget' { $lines += 'cleartarget' }
            'camera'     {   # the camera's distance partway through, as config.camera: 0 first person, 1 to 9 notches out, 10 all the way
                $lines += 'zoom -30'
                if ([int]$v -ge 10) { $lines += 'zoom 30' } elseif ([int]$v -gt 0) { $lines += 'wait 1'; $lines += "wheel $([int]$v)" }
            }   # typed into the chat box as a player types: a slash command (/target Pinto)
            default      { throw "$($t.name): unknown step '$($p.Name)'" }
        }
    }
    # The summoned creatures go again: each selected by its name and deleted. .npc delete takes a world spawn out
    # of the database for good, so it is sent only when the selection has exactly that name.
    if ($cfg.summon) {
        foreach ($u in $cfg.summon) {
            $lines += "target $($u.name)"
            $lines += 'wait 0.5'
            $lines += "npcdelete $($u.name)"
            $lines += 'wait 0.5'
        }
        $lines += 'cleartarget'
    }
    # Put back what the test set.
    $lines += Say "$($t.name): putting the controls back"
    $lines += 'cvarsback'
    $lines += 'atmos reset'
    $script = Join-Path $resultsDir "$stamp-$($t.name).script.txt"
    [IO.File]::WriteAllText($script, ($lines -join "`r`n") + "`r`n")

    $logStart = if (Test-Path $log) { @(Get-Content $log).Count } else { 0 }
    $started = Get-Date
    $runOut = & (Join-Path $tool 'Run-Test.ps1') $script -Client $Client 6>&1 | Out-String
    $positions = @($runOut -split "`r?`n" | Where-Object { $_ -match '^(pos|arrive|face|heading|pitch) ' })
    $warnings = @($runOut -split "`r?`n" | Where-Object { $_ -match '^error ' })

    # The last probe of this run: from its header to the next report or the end. The frame it logs comes
    # before the header (2026-10-04: the water's lines), and before that the probe's own start: from its
    # "--- probe:" line (the see-through counts), or else its "begin frame" line.
    $new = if (Test-Path $log) { @(Get-Content $log | Select-Object -Skip $logStart) } else { @() }
    $heads = @(for ($i = 0; $i -lt $new.Count; $i++) { if ($new[$i] -match '^=== client report \((F12|/atmos probe)\) ===') { $i } })
    # Each probe of the run (2026-10-04): a check reads the last unless it names one (probeAt, from 1). A probe's
    # lines end at the next probe's own start, or its report.
    $probes = @()
    foreach ($hd in $heads) {
        $from = $hd
        $begin = -1
        for ($i = $from - 1; $i -ge 0 -and $new[$i] -notmatch '^=== (end of )?client report'; $i--) {
            if ($new[$i] -match '^--- probe: ') { $begin = $i; break }
            if ($begin -lt 0 -and $new[$i] -match '^--- begin frame \d+ capture ---') { $begin = $i }
        }
        if ($begin -ge 0) { $from = $begin }
        $to = $new.Count
        for ($i = $hd + 1; $i -lt $new.Count; $i++) { if ($new[$i] -match '^=== client report|^--- probe: ') { $to = $i; break } }
        $probes += ,@($new[$from..($to - 1)])
    }
    $probe = if ($probes.Count) { $probes[-1] } else { @() }

    # Every screenshot of the run, in order: a test can take one in each debug view.
    $shots = @(Get-ChildItem (Join-Path $Client 'Screenshots') -File -ErrorAction SilentlyContinue |
               Where-Object { $_.LastWriteTime -ge $started } | Sort-Object LastWriteTime)

    $results = @()
    $checks = @()   # for the page: each check, whether it passed, and the screenshot it read (0: the probe)
    $pass = $true
    foreach ($e in $t.expect) {
        if ($null -ne $e.shot -and $null -ne $e.box) {
            # A box of a screenshot (2026-10-04): how much of it jumps from one pixel to the next. Counting
            # half-grey pixels missed speckle that came as hard black and white dots.
            $n = [int]$e.shot
            if ($n -lt 1 -or $n -gt $shots.Count) { $results += "FAIL  no screenshot $n ($($shots.Count) taken): $($e.about)"; $checks += [pscustomobject]@{ ok = $false; about = $e.about; got = "no screenshot $n ($($shots.Count) taken)"; shot = 0 }; $pass = $false; continue }
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
            $checks += [pscustomobject]@{ ok = $ok; about = $e.about; got = ('{0}% of the box jumps from one pixel to the next (at most {1}%)' -f $share, $e.jumpMax); shot = $n }
            continue
        }
        if ($null -ne $e.shot) {
            # A row of a screenshot (2026-10-04): the Debug View 5 shot is one grey where the shade is even, and
            # a strip of shade that should not be there shows as a dip.
            $n = [int]$e.shot
            if ($n -lt 1 -or $n -gt $shots.Count) { $results += "FAIL  no screenshot $n ($($shots.Count) taken): $($e.about)"; $checks += [pscustomobject]@{ ok = $false; about = $e.about; got = "no screenshot $n ($($shots.Count) taken)"; shot = 0 }; $pass = $false; continue }
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
            # meanMax (2026-10-04): the most the average may be, for a row that must stay in shade.
            $ok = ($null -eq $e.min -or $low -ge [int]$e.min) -and ($null -eq $e.mean -or $avg -ge [int]$e.mean) -and
                  ($null -eq $e.meanMax -or $avg -le [int]$e.meanMax)
            $wantAvg = @()
            if ($null -ne $e.mean) { $wantAvg += "at least $($e.mean)" }
            if ($null -ne $e.meanMax) { $wantAvg += "at most $($e.meanMax)" }
            $wantAvg = if ($wantAvg.Count) { $wantAvg -join ' and ' } else { 'any' }
            if (-not $ok) { $pass = $false }
            $results += ('{0}  {1}: row {2} averages {3}, its darkest pixel {4} at column {5} (wanted: average {6}, darkest {7})' -f $(if ($ok) { 'pass' } else { 'FAIL' }), $e.about, $e.row, $avg, $low, $at, $wantAvg, $(if ($null -ne $e.min) { "at least $($e.min)" } else { 'any' }))
            $checks += [pscustomobject]@{ ok = $ok; about = $e.about; got = ('row {0} averages {1}, its darkest pixel {2} at column {3} (wanted: average {4}, darkest {5})' -f $e.row, $avg, $low, $at, $wantAvg, $(if ($null -ne $e.min) { "at least $($e.min)" } else { 'any' })); shot = $n }
            continue
        }
        if (-not $heads.Count) { $results += "FAIL  no probe in the log: $($e.about)"; $checks += [pscustomobject]@{ ok = $false; about = $e.about; got = 'no probe in the log'; shot = 0 }; $pass = $false; continue }
        $slice = $probe
        if ($null -ne $e.probeAt) {
            $k = [int]$e.probeAt
            $slice = if ($k -ge 1 -and $k -le $probes.Count) { $probes[$k - 1] } else { @() }
        }
        $hits = @($slice | Where-Object { $_ -match $e.probe })
        $ok = $true
        if ($null -ne $e.max -and $hits.Count -gt [int]$e.max) { $ok = $false }
        if ($null -ne $e.min -and $hits.Count -lt [int]$e.min) { $ok = $false }
        if (-not $ok) { $pass = $false }
        $results += ('{0}  {1}: {2} line(s) match' -f $(if ($ok) { 'pass' } else { 'FAIL' }), $e.about, $hits.Count)
        $checks += [pscustomobject]@{ ok = $ok; about = $e.about; got = ('{0} line(s) of the probe match' -f $hits.Count); shot = 0 }
        $results += @($hits | Select-Object -First 10 | ForEach-Object { "        $_" })
    }

    $shotCopies = @()
    for ($i = 0; $i -lt $shots.Count; $i++) {
        $suffix = if ($shots.Count -gt 1) { "-$($i + 1)" } else { '' }
        $copy = Join-Path $resultsDir "$stamp-$($t.name)$suffix$($shots[$i].Extension)"
        Copy-Item $shots[$i].FullName $copy
        $shotCopies += $copy
    }
    # Every recording of the run (2026-10-05), with its sheet of frames: the step record.
    $recs = @(Get-ChildItem (Join-Path $Client 'Recordings') -Filter *.mp4 -File -ErrorAction SilentlyContinue |
              Where-Object { $_.LastWriteTime -ge $started } | Sort-Object LastWriteTime)
    $recCopies = @()
    for ($i = 0; $i -lt $recs.Count; $i++) {
        $copy = Join-Path $resultsDir "$stamp-$($t.name)-rec$($i + 1).mp4"
        Copy-Item $recs[$i].FullName $copy
        $sheet = $recs[$i].FullName -replace '\.mp4$', '-sheet.jpg'
        if (Test-Path $sheet) { Copy-Item $sheet ($copy -replace '\.mp4$', '-sheet.jpg') }
        $recCopies += $copy
    }

    $verdict = if ($pass) { 'PASS' } else { 'FAIL' }
    $report = @("$verdict  $($t.name)  ($stamp)") + $results
    if ($positions.Count) { $report += 'where the character was:'; $report += @($positions | ForEach-Object { "        $_" }) }
    if ($warnings.Count) { $report += 'errors from the game:'; $report += @($warnings | ForEach-Object { "        $_" }) }
    if ($shotCopies.Count) { $report += @($shotCopies | ForEach-Object { "screenshot: $_" }) } else { $report += 'screenshot: none taken' }
    $report += @($recCopies | ForEach-Object { "recording: $_" })
    $text = $report -join "`r`n"
    Write-Host $text
    [IO.File]::WriteAllText((Join-Path $resultsDir "$stamp-$($t.name).txt"), $text + "`r`n`r`n" + ($probe -join "`r`n") + "`r`n")
    $summary += "$verdict  $($t.name)"
    if ($Accept -and $shotCopies.Count) {
        New-Item -ItemType Directory -Force $expectedDir | Out-Null
        for ($i = 0; $i -lt $shotCopies.Count; $i++) {
            Copy-Item $shotCopies[$i] (Join-Path $expectedDir "$($t.name)-$($i + 1)$([IO.Path]::GetExtension($shotCopies[$i]))") -Force
        }
    }
    # The recordings too (2026-10-05): expected\<test>-rec<n>.mp4, played beside this run's on the page.
    if ($Accept -and $recCopies.Count) {
        New-Item -ItemType Directory -Force $expectedDir | Out-Null
        for ($i = 0; $i -lt $recCopies.Count; $i++) {
            Copy-Item $recCopies[$i] (Join-Path $expectedDir "$($t.name)-rec$($i + 1).mp4") -Force
        }
    }
    $records += [pscustomobject]@{
        name = $t.name; about = $t.about; pass = $pass; stamp = $stamp; checks = $checks
        positions = $positions; warnings = $warnings; shots = $shotCopies; views = $shotViews
        recordings = $recCopies; recViews = $recViews; recLabels = $recLabels
    }
}

# The character is left as a login leaves it: the camera behind it (face turns the camera alone), flight off.
$end = @('say putting the camera back, flight off', 'camback', 'flight off', 'say done')
$script = Join-Path $resultsDir "$((Get-Date).ToString('yyyyMMdd-HHmmss'))-end.script.txt"
[IO.File]::WriteAllText($script, ($end -join "`r`n") + "`r`n")
& (Join-Path $tool 'Run-Test.ps1') $script -Client $Client 6>&1 | Out-Null

# The page (2026-10-04): every test, its checks, and each screenshot beside the expected one.
& (Join-Path $here 'Write-Report.ps1') -Records $records -Page (Join-Path $resultsDir "$runStamp-report.html") `
    -ExpectedDir $expectedDir -ViewNames $viewNames -Started $runStarted
# The same page as results\last-results.html (2026-10-05): a tab kept open on it shows the newest run when
# refreshed. Its links are relative to results\, so the copy works there unchanged.
Copy-Item (Join-Path $resultsDir "$runStamp-report.html") (Join-Path $resultsDir 'last-results.html') -Force

Write-Host ''
Write-Host ($summary -join "`r`n")
Write-Host "report: $(Join-Path $resultsDir "$runStamp-report.html")"
if (-not $NoOpen) { Start-Process (Join-Path $resultsDir "$runStamp-report.html") }
