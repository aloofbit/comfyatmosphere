<#
.SYNOPSIS
  Writes the page of a test run: Run-Tests.ps1 calls it at the end of each run (2026-10-04).

.DESCRIPTION
  One section a test: whether it passed, what it is about, each check and what it measured, and each
  screenshot beside the expected one (expected\<test>-<n>.jpg, taken with Run-Tests.ps1 -Accept). A check that
  reads a screenshot is shown with that screenshot. The page lies in results\ beside the screenshots, so it
  links them by name and the expected ones by ..\expected\.

  At the top, the Performance panel (2026-10-06): every number the performance tests record, from
  results\perf-history.json, so it shows on every run's page: the latest value, its limit, the change from the
  run before, and the last 20 runs as a small graph with the limit as a dashed line.
#>
param(
    [Parameter(Mandatory)][object[]]$Records,
    [Parameter(Mandatory)][string]$Page,
    [Parameter(Mandatory)][string]$ExpectedDir,
    [string[]]$ViewNames = @(),
    [datetime]$Started = (Get-Date),
    [string]$History = '',
    [string]$Client = '',
    # A page of several runs (Build-Latest.ps1, 2026-10-06): this line in place of the run's date and minutes, and
    # each test's own run time beside its name.
    [string]$Summary = ''
)

function Html([string]$text) { [System.Net.WebUtility]::HtmlEncode($text) }

function Checks($list) {
    if (-not $list.Count) { return '' }
    $out = '<ul class="checks">'
    foreach ($c in $list) {
        $out += "<li class=""$(if ($c.ok) { 'ok' } else { 'bad' })"">$(Html $c.about)<span class=""got"">$(Html $c.got)</span></li>"
    }
    return $out + '</ul>'
}

# The Performance panel (2026-10-06).
function Spark([double[]]$values, $max, $min, [bool]$lastOk) {
    $w = 200; $h = 46; $pad = 5
    $all = @($values)
    if ($null -ne $max) { $all += [double]$max }
    if ($null -ne $min) { $all += [double]$min }
    $lo = ($all | Measure-Object -Minimum).Minimum; $hi = ($all | Measure-Object -Maximum).Maximum
    if ($hi - $lo -lt 1e-6) { $hi = $lo + 1 }
    $span = $hi - $lo; $lo -= $span * 0.1; $hi += $span * 0.1
    $n = $values.Count
    $inv = [Globalization.CultureInfo]::InvariantCulture
    $X = { param($i) if ($n -le 1) { $w / 2 } else { $pad + $i * ($w - 2 * $pad) / ($n - 1) } }
    $Y = { param($v) $h - $pad - ($v - $lo) / ($hi - $lo) * ($h - 2 * $pad) }
    $pts = @(for ($i = 0; $i -lt $n; $i++) { '{0},{1}' -f (& $X $i).ToString('0.#', $inv), (& $Y $values[$i]).ToString('0.#', $inv) })
    $svg = "<svg class=""spark"" viewBox=""0 0 $w $h"" preserveAspectRatio=""none"" role=""img"" aria-label=""the last $n runs"">"
    foreach ($lim in @($max, $min)) {
        if ($null -ne $lim) { $y = (& $Y ([double]$lim)).ToString('0.#', $inv); $svg += "<line x1=""0"" x2=""$w"" y1=""$y"" y2=""$y"" class=""limit""/>" }
    }
    if ($n -gt 1) { $svg += "<polyline points=""$($pts -join ' ')"" class=""trend""/>" }
    $last = $pts[-1] -split ','
    $svg += "<circle cx=""$($last[0])"" cy=""$($last[1])"" r=""3.5"" class=""$(if ($lastOk) { 'dot ok' } else { 'dot bad' })""/>"
    return $svg + '</svg>'
}
# A chart of runs (the owner, 2026-10-06: one larger chart, a coloured line for each place): $series is a list of
# @{ name; color; values }, each values one per run in $labels (null where that run has none).
function LineChart([string]$title, [string[]]$labels, $series, [string]$unit, $limit) {
    $inv = [Globalization.CultureInfo]::InvariantCulture
    $w = 420; $h = 190; $l = 40; $r = 10; $t = 14; $b = 24
    $vals = @(foreach ($sr in $series) { foreach ($v in $sr.values) { if ($null -ne $v) { [double]$v } } })
    if ($null -ne $limit) { $vals += [double]$limit }
    if (-not $vals.Count) { return '' }
    $lo = ($vals | Measure-Object -Minimum).Minimum; $hi = ($vals | Measure-Object -Maximum).Maximum
    if ($hi - $lo -lt 1e-6) { $hi = $lo + 1 }
    $pad = ($hi - $lo) * 0.12; $lo = [Math]::Max(0.0, $lo - $pad); $hi += $pad
    $fmt = if ($hi - $lo -lt 3) { '0.##' } elseif ($hi - $lo -lt 30) { '0.#' } else { '0' }
    $f = { param($v) ([double]$v).ToString($fmt, $inv) }
    $n = $labels.Count
    $PosX = { param($i) if ($n -le 1) { ($l + $w - $r) / 2 } else { $l + $i * ($w - $l - $r) / ($n - 1) } }
    $PosY = { param($v) $t + ($hi - $v) / ($hi - $lo) * ($h - $t - $b) }
    # Not $X, $Y: PowerShell's names ignore case, and $y below is a number.
    $svg = "<svg class=""chart"" viewBox=""0 0 $w $h"" role=""img"" aria-label=""$(Html $title)"">"
    for ($k = 0; $k -le 3; $k++) {
        $v = $lo + ($hi - $lo) * $k / 3; $y = & $f (& $PosY $v)
        $svg += "<line x1=""$l"" x2=""$($w - $r)"" y1=""$y"" y2=""$y"" class=""grid""/><text x=""$($l - 6)"" y=""$y"" class=""ax"" text-anchor=""end"" dominant-baseline=""middle"">$(& $f $v)</text>"
    }
    if ($null -ne $limit) {
        $y = & $f (& $PosY ([double]$limit))
        $svg += "<line x1=""$l"" x2=""$($w - $r)"" y1=""$y"" y2=""$y"" class=""limit""/><text x=""$($w - $r)"" y=""$($y - 4)"" class=""ax"" text-anchor=""end"">limit $(& $f $limit)$unit</text>"
    }
    if ($n -ge 1) {
        $svg += "<text x=""$l"" y=""$($h - 6)"" class=""ax"">$(Html $labels[0])</text>"
        if ($n -gt 1) { $svg += "<text x=""$($w - $r)"" y=""$($h - 6)"" class=""ax"" text-anchor=""end"">$(Html $labels[-1])</text>" }
    }
    $legend = ''
    foreach ($sr in $series) {
        $pts = @(for ($i = 0; $i -lt $n; $i++) { if ($null -ne $sr.values[$i]) { '{0},{1}' -f (& $f (& $PosX $i)), (& $f (& $PosY ([double]$sr.values[$i]))) } })
        if ($pts.Count -gt 1) { $svg += "<polyline points=""$($pts -join ' ')"" fill=""none"" stroke=""$($sr.color)"" stroke-width=""2.2"" stroke-linejoin=""round""/>" }
        foreach ($pt in $pts) { $xy = $pt -split ','; $svg += "<circle cx=""$($xy[0])"" cy=""$($xy[1])"" r=""3"" fill=""$($sr.color)""/>" }
        $lastV = @($sr.values | Where-Object { $null -ne $_ })
        $now = if ($lastV.Count) { " $(& $f $lastV[-1])$unit" } else { '' }
        $legend += "<span class=""key""><i style=""background:$($sr.color)""></i>$(Html $sr.name)<b>$now</b></span>"
    }
    return "<figure class=""chartbox""><figcaption>$(Html $title)</figcaption>$svg</svg><div class=""legend"">$legend</div></figure>"
}
function RunLabels([string[]]$stamps) {
    @($stamps | ForEach-Object { [datetime]::ParseExact($_, 'yyyyMMdd-HHmmss', [Globalization.CultureInfo]::InvariantCulture).ToString('MMM d HH:mm') })
}

# A label and its value (the owner, 2026-10-06: labels, not sentences), with the limit after it in grey.
function KV([string]$label, [string]$value, [string]$note = '') {
    "<div class=""kv""><span class=""k"">$(Html $label)</span><span class=""v"">$value$(if ($note) { "" <span class=lim>$(Html $note)</span>"" })</span></div>"
}

# The controls a test sets, as a dialog (the owner, 2026-10-06: a link that opens them): each Atmosphere page
# control by its tab, with the label and the tooltip the game shows, read from the addon's own tables, and the value
# as its slider shows it. Returns the dialog's HTML, or '' when the test sets none.
function ControlsDialog([string]$test, [string]$id) {
    $testsDir = Split-Path $ExpectedDir
    $file = Join-Path $testsDir "$test.json"
    if (-not (Test-Path $file)) { return '' }
    $cfg = (Get-Content $file -Raw | ConvertFrom-Json).config
    if (-not $cfg.cvars) { return '' }
    $values = [ordered]@{}
    foreach ($pr in $cfg.cvars.PSObject.Properties) { $values[$pr.Name] = $pr.Value }
    $label = @{}; $tip = @{}; $tabOf = @{}; $tabs = @()
    $lua = Join-Path $testsDir '..\addon\ComfyAtmosphere\ComfyAtmosphere.lua'
    if (Test-Path $lua) {
        $src = Get-Content $lua -Raw
        $names = @{}
        foreach ($m in [regex]::Matches($src, '(?m)^(COMFYATMOSPHERE_\w+)\s*=\s*"([^"]*)";')) { $names[$m.Groups[1].Value] = $m.Groups[2].Value }
        foreach ($m in [regex]::Matches($src, 'name = "(COMFYATMOSPHERE_\w+)"')) {
            $rest = $src.Substring($m.Index, [Math]::Min(900, $src.Length - $m.Index))
            $c = [regex]::Match($rest, 'cvar = "(\w+)"')
            if (-not $c.Success -or $label.ContainsKey($c.Groups[1].Value)) { continue }
            $cv = $c.Groups[1].Value
            $label[$cv] = if ($names.ContainsKey($m.Groups[1].Value)) { $names[$m.Groups[1].Value] } else { $cv }
            $d = [regex]::Match($rest, 'desc = "((?:[^"\\]|\\.)*)"')
            if ($d.Success) { $tip[$cv] = $d.Groups[1].Value }
        }
        foreach ($m in [regex]::Matches($src, '\{ "(\w+)", \{([^}]*)\} \}')) {
            $tabs += $m.Groups[1].Value
            foreach ($cv in [regex]::Matches($m.Groups[2].Value, '"(\w+)"')) { if (-not $tabOf.ContainsKey($cv.Groups[1].Value)) { $tabOf[$cv.Groups[1].Value] = $m.Groups[1].Value } }
        }
    }
    $out = "<dialog id=""$id"" class=""ctl""><div class=""dhead""><h3>Test settings: $(Html $test)</h3><form method=""dialog""><button aria-label=""Close"">&#10005;</button></form></div>"
    $out += "<p class=""lim"">The Atmosphere page controls this test applies at the start of every run, so results compare across runs. Captured from a reference setup on 2026-10-06. Values are shown as on the in-game sliders. Settings without a slider use the defaults in comfyatmos.ini.</p>"
    if ($cfg.sun) { $out += "<p><b>Sun</b> fixed at azimuth $($cfg.sun.azimuth) and elevation $($cfg.sun.elevation) degrees, the same light every run.</p>" }
    $order = @($tabs) + @('Other')
    foreach ($tab in $order) {
        $rows = @($values.Keys | Where-Object { $(if ($tabOf.ContainsKey($_)) { $tabOf[$_] } else { 'Other' }) -eq $tab })
        if (-not $rows.Count) { continue }
        $out += "<h4>$(Html $tab)</h4><table>"
        foreach ($cv in $rows) {
            $name = if ($label.ContainsKey($cv)) { $label[$cv] } else { $cv }
            $t = if ($tip.ContainsKey($cv)) { (' title="{0}"' -f (Html $tip[$cv])) } else { '' }
            $out += "<tr$t><td>$(Html $name)</td><td class=""val"">$(Html "$($values[$cv])")</td><td class=""cv"">$(Html $cv)</td></tr>"
        }
        $out += '</table>'
    }
    return $out + '</dialog>'
}

# The test system (2026-10-06): the page is read by others, who need the hardware to judge the numbers. From the
# client report comfyatmos.dll writes at each start: the GPU, the CPU and memory, Windows, the game's window, the
# client's build and the mod's.
# comfyatmos.ini as a dialog (2026-10-06): the test client's file, each section with its own description, and each key
# with its value and comment. Comments that run on (an indented ";" line) join the key's.
function IniDialog([string]$path, [string]$id) {
    if (-not (Test-Path $path)) { return '' }
    $sections = @(); $cur = $null; $pending = @(); $lastKey = $null
    foreach ($line in Get-Content $path) {
        if ($line -match '^\[(\w+)\]') {
            $cur = [pscustomobject]@{ name = $Matches[1]; about = ($pending -join ' ').Trim(); keys = New-Object System.Collections.ArrayList }
            $sections += $cur; $pending = @(); $lastKey = $null; continue
        }
        if ($line -match '^;\s?(.*)$') { $pending += $Matches[1]; $lastKey = $null; continue }
        if ($line -match '^\s+;\s?(.*)$' -and $lastKey) { $lastKey.comment = ($lastKey.comment + ' ' + $Matches[1]).Trim(); continue }
        if ($line -match '^\s*$') { $pending = @(); $lastKey = $null; continue }
        if ($cur -and $line -match '^(\w+)\s*=\s*([^;]*?)\s*(?:;\s?(.*))?$') {
            $lastKey = [pscustomobject]@{ key = $Matches[1]; value = $Matches[2]; comment = "$($Matches[3])".Trim() }
            [void]$cur.keys.Add($lastKey)
        }
    }
    $out = "<dialog id=""$id"" class=""ctl""><div class=""dhead""><h3>comfyatmos.ini</h3><form method=""dialog""><button aria-label=""Close"">&#10005;</button></form></div>"
    $out += "<p class=""lim"">The settings file of the test client, as the tests ran with it. A test's own Atmosphere controls (its test settings) are applied over the matching values here.</p>"
    foreach ($sec in $sections) {
        $out += "<h4>[$(Html $sec.name)]</h4>"
        if ($sec.about) { $out += "<p class=""lim"">$(Html $sec.about)</p>" }
        $out += '<table>'
        foreach ($k in $sec.keys) {
            $out += "<tr><td class=""ik"">$(Html $k.key)</td><td class=""iv"">$(Html $k.value)</td><td class=""ic"">$(Html $k.comment)</td></tr>"
        }
        $out += '</table>'
    }
    return $out + '</dialog>'
}

function TestSystem([string]$settingsDialog = '', [string]$settingsText = '') {
    if (-not $Client) { return '' }
    # In the client's Logs folder since 2026-10-06, in the client folder before. The newest is the last run's.
    $log = @('Logs\comfyatmos.log', 'comfyatmos.log') | ForEach-Object { Join-Path $Client $_ } | Where-Object { Test-Path $_ } |
           Sort-Object { (Get-Item $_).LastWriteTime } -Descending | Select-Object -First 1
    if (-not $log) { return '' }
    if (-not (Test-Path $log)) { return '' }
    $lines = @(Get-Content $log -TotalCount 400)
    $last = { param($pat) @($lines | Where-Object { $_ -match $pat }) | Select-Object -Last 1 }
    $rows = ''
    $g = & $last '^graphics: '
    if ($g -match '^graphics: (.+?) \(vendor') { $rows += (KV 'GPU' (Html $Matches[1])) }
    $cpu = @($lines | Where-Object { $_ -match '^system: .*threads' }) | Select-Object -Last 1
    if ($cpu -match '^system: (.+?), (\d+) threads, ([\d.]+) GB memory') {
        $name = ($Matches[1] -replace '\(R\)|\(TM\)', '' -replace '\s+CPU\s+', ' ' -replace '\s{2,}', ' ').Trim()
        $rows += (KV 'CPU' (Html "$name, $($Matches[2]) threads")) + (KV 'Memory' (Html "$($Matches[3]) GB"))
    }
    $os = @($lines | Where-Object { $_ -match '^system: Windows' }) | Select-Object -Last 1
    if ($os -match '^system: Windows ([\d.]+) build (\d+)') { $rows += (KV 'Windows' (Html "build $($Matches[2])")) }
    if ($g -match 'back buffer (\d+)x(\d+), format \d+, (\w+), multisample (\d+)(?:, refresh \d+ Hz)?(?:, vsync (\w+))?') {
        $win = "$($Matches[1]) x $($Matches[2]), $($Matches[3]), multisampling $($Matches[4])x"
        if ($Matches[5]) { $win += ", vsync $($Matches[5])" }
        $rows += (KV 'Game window' (Html $win))
    }
    $wow = & $last '^WoW\.exe: '
    if ($wow -match 'version ([\d.]+)') { $rows += (KV 'Client' (Html "World of Warcraft $($Matches[1]), DXVK")) }
    $mod = & $last '^comfyatmos: v'
    if ($mod -match '^comfyatmos: (v[^,]+), built ([^,]+),') { $rows += (KV 'comfyatmos.dll' (Html "$($Matches[1]), built $($Matches[2] -replace '\s+', ' ')")) }
    $ini = IniDialog (Join-Path $Client 'comfyatmos.ini') 'ini-dialog'
    if ($ini) {
        $n = @(Get-Content (Join-Path $Client 'comfyatmos.ini') | Where-Object { $_ -match '^\w+\s*=' }).Count
        $rows += (KV 'comfyatmos.ini' "<a href=""#"" class=""dlg"" data-dialog=""ini-dialog"">$n settings</a>")
    }
    if ($settingsDialog) { $rows += (KV 'Test settings' "<a href=""#"" class=""dlg"" data-dialog=""ctl-frame"">$(Html $settingsText)</a>") }
    if (-not $rows) { return '' }
    return "<div class=""block""><div class=""bhead""><h3>Test system</h3></div><div class=""kvs"">$rows</div></div>" + $ini + $settingsDialog
}

function PerfPanel {
    # For people, not a list of numbers (the owner, 2026-10-06): a line saying whether all is well, the sign-in in
    # words, a row for each place with its frame rate with and without the effects and what they cost, and every
    # number, as before, folded away under "All numbers".
    $out = '<section class="perf" id="performance"><h2>Performance</h2>'
    $h = @()
    # Unrolled: Windows PowerShell's ConvertFrom-Json gives a JSON array as one object.
    if ($History -and (Test-Path $History)) { $h = @((Get-Content $History -Raw | ConvertFrom-Json) | ForEach-Object { $_ }) }
    if (-not $h.Count) {
        return $out + '<p class="about">No performance runs yet. Run load-performance and frame-rate-performance.</p></section>'
    }
    $inv = [Globalization.CultureInfo]::InvariantCulture
    $F = { param($v, $fmt) ([double]$v).ToString($fmt, $inv) }
    # The latest run of each test, and each metric's last 20 values.
    $latest = @{}
    $series = @{}
    foreach ($test in @($h | ForEach-Object { $_.test } | Select-Object -Unique)) {
        $rows = @($h | Where-Object { $_.test -eq $test })
        $last = ($rows | Sort-Object stamp | Select-Object -Last 1).stamp
        foreach ($r in @($rows | Where-Object { $_.stamp -eq $last })) { $latest[$r.metric] = $r }
        foreach ($m in @($rows | ForEach-Object { $_.metric } | Select-Object -Unique)) {
            $series[$m] = [double[]]@($rows | Where-Object { $_.metric -eq $m } | Sort-Object stamp | Select-Object -Last 20 | ForEach-Object { [double]$_.value })
        }
    }
    $bad = @($latest.Values | Where-Object { -not $_.ok -and ($null -ne $_.max -or $null -ne $_.min) })
    if ($bad.Count) {
        $out += "<p class=""verdict bad"">&#10007; $($bad.Count) measurement$(if ($bad.Count -ne 1) { 's' }) outside the limit: $(Html (($bad | ForEach-Object { $_.metric }) -join ', '))</p>"
    }
    else { $out += '<p class="verdict ok">&#10003; All measurements within their limits.</p>' }
    # The frame-rate test's own controls (its test settings), in the Test system block with comfyatmos.ini.
    $frameTest = @($h | Where-Object { $_.metric -like '* fps' } | ForEach-Object { $_.test } | Select-Object -Unique) | Select-Object -First 1
    $setDlg = if ($frameTest) { ControlsDialog $frameTest 'ctl-frame' } else { '' }
    $setText = ''
    if ($setDlg) {
        $tc = (Get-Content (Join-Path (Split-Path $ExpectedDir) "$frameTest.json") -Raw | ConvertFrom-Json).config
        $setText = "$(@($tc.cvars.PSObject.Properties).Count) Atmosphere controls$(if ($tc.sun) { ' and a fixed sun' }), in $frameTest"
    }
    $out += (TestSystem $setDlg $setText)

    # Signing in.
    $comp = $latest["Compiling on the game's thread"]; $worst = $latest['Worst frame as the effects start']
    if ($comp -and $worst) {
        $slow = $latest['Slow frames as the effects start']; $start = $latest['Effects start after the world']; $work = $latest['Shader worker']
        $ok = $comp.ok -and $worst.ok -and (-not $slow -or $slow.ok)
        $word = if ($ok) { 'No freeze' } else { 'Freezes' }
        $out += "<div class=""block""><div class=""bhead""><h3>Signing in</h3><span class=""when"">$(Html (Stamp $comp.stamp))</span></div>"
        $out += "<div class=""big $(if ($ok) { 'ok' } else { 'bad' })"">$word</div><div class=""kvs"">"
        $out += (KV 'Largest stutter' "$(& $F ($worst.value / 1000) '0.00') s" "limit $(& $F ($worst.max / 1000) '0.0') s")
        if ($slow) { $out += (KV 'Slow frames after it' (& $F $slow.value '0') "over 40 ms, limit $($slow.max)") }
        $out += (KV 'Compiling in the game' "$(& $F $comp.value '0') ms" 'limit 100 ms; 11,091 ms before the shader cache')
        if ($start) { $out += (KV 'Effects on after' "$(& $F $start.value '0.0') s" 'after the world first appears') }
        if ($work) { $out += (KV 'Shader worker' "$(& $F $work.value '0') ms" 'until every shader is ready') }
        $out += '</div>'
        $lstamps = @($h | Where-Object { $_.test -eq $comp.test } | ForEach-Object { $_.stamp } | Select-Object -Unique | Sort-Object | Select-Object -Last 20)
        $hv = @(foreach ($st in $lstamps) { $r = $h | Where-Object { $_.stamp -eq $st -and $_.metric -eq 'Worst frame as the effects start' } | Select-Object -First 1; if ($r) { [double]$r.value / 1000 } else { $null } })
        $out += '<div class="charts">' + (LineChart 'Largest stutter at sign-in (s)' (RunLabels $lstamps) @(@{ name = 'Largest stutter'; color = '#3b82c4'; values = $hv }) ' s' ($worst.max / 1000)) + '</div>'
        $out += '</div>'
    }

    # Frame rate by place.
    $places = @($latest.Keys | Where-Object { $_ -like '* fps' -and $_ -notlike '* without effects' } | ForEach-Object { $_ -replace ' fps$', '' })
    $order = @('Harbour', 'Elwynn', 'Ironforge')
    $places = @($order | Where-Object { $places -contains $_ }) + @($places | Where-Object { $order -notcontains $_ })
    if ($places.Count) {
        $anyFps = $latest["$($places[0]) fps"]
        $out += "<div class=""block""><div class=""bhead""><h3>Frame rate</h3><span class=""when"">$(Html (Stamp $anyFps.stamp))</span></div>"
        $out += '<p class="lim">Measured for 10 s at each place <span class="sw game"></span> fps with effects <span class="sw fx"></span> fps the effects take</p>'
        $out += '<div class="places">'
        # The bar in frames a second (the owner, 2026-10-06): its whole length the place's own rate without effects,
        # grey to the rate with them, orange the rest, what the effects take. On one scale for every place, a place
        # slower than the fastest left an empty end; the fps chart below compares places.
        foreach ($pl in $places) {
            $on = $latest["$pl fps"]; $off = $latest["$pl fps without effects"]; $stut = $latest["$pl slowest 1%"]
            $msOn = 1000.0 / [double]$on.value
            $okRow = $on.ok -and (-not $stut -or $stut.ok)
            $out += "<div class=""place $(if ($okRow) { 'ok' } else { 'bad' })""><div class=""pname"">$(Html $pl)</div>"
            $out += "<div class=""pfps""><b>$(& $F $on.value '0')</b> fps</div>"
            if ($off) {
                $msOff = 1000.0 / [double]$off.value
                $cost = [Math]::Max(0.0, $msOn - $msOff)
                $fOn = [double]$on.value; $fOff = [Math]::Max($fOn, [double]$off.value)
                $wGame = 100.0 * $fOn / [Math]::Max(1.0, $fOff); $wFx = 100.0 - $wGame
                $out += "<div class=""bar"" title=""$(& $F $fOn '0') fps with effects, $(& $F ($fOff - $fOn) '0') fps taken by them, $(& $F $fOff '0') fps without""><span class=""game"" style=""width:$(& $F $wGame '0.#')%""></span><span class=""fx"" style=""width:$(& $F $wFx '0.#')%""></span></div>"
            }
            $out += '<div class="kvs">'
            if ($off) {
                $out += (KV 'Without effects' "$(& $F $off.value '0') fps")
                $out += (KV 'Effects cost' "$(& $F $cost '0.0') ms")
                $out += (KV 'Frame time' "$(& $F $msOn '0.0') ms" "game $(& $F $msOff '0.0')")
            }
            if ($stut) { $out += (KV 'Stutter' "$(& $F $stut.value '0.0') ms" "limit $($stut.max)") }
            $out += '</div></div>'
        }
        $out += '</div>'
        # The charts: a coloured line for each place, over the last 20 runs of the test.
        $colors = @{ Harbour = '#3b82c4'; Elwynn = '#4caf50'; Ironforge = '#a66ad1' }
        $spare = @('#d08a2e', '#d4547a', '#3fb5b0')
        $ftest = $anyFps.test
        $fstamps = @($h | Where-Object { $_.test -eq $ftest } | ForEach-Object { $_.stamp } | Select-Object -Unique | Sort-Object | Select-Object -Last 20)
        $V = { param($st, $m) $r = $h | Where-Object { $_.stamp -eq $st -and $_.metric -eq $m } | Select-Object -First 1; if ($r) { [double]$r.value } else { $null } }
        $fpsS = @(); $costS = @(); $stutS = @(); $ci = 0
        foreach ($pl in $places) {
            $col = if ($colors.ContainsKey($pl)) { $colors[$pl] } else { $spare[$ci++ % $spare.Count] }
            $fpsS += @{ name = $pl; color = $col; values = @(foreach ($st in $fstamps) { & $V $st "$pl fps" }) }
            $costS += @{ name = $pl; color = $col; values = @(foreach ($st in $fstamps) {
                $a1 = & $V $st "$pl fps"; $a0 = & $V $st "$pl fps without effects"
                if ($null -ne $a1 -and $null -ne $a0 -and $a1 -gt 0 -and $a0 -gt 0) { [Math]::Max(0.0, 1000.0 / $a1 - 1000.0 / $a0) } else { $null } }) }
            $stutS += @{ name = $pl; color = $col; values = @(foreach ($st in $fstamps) { & $V $st "$pl slowest 1%" }) }
        }
        $labels = RunLabels $fstamps
        $out += '<div class="charts">'
        $out += (LineChart 'Frames per second' $labels $fpsS ' fps' $anyFps.min)
        $out += (LineChart 'Effects cost (ms a frame)' $labels $costS ' ms' $null)
        $out += (LineChart 'Stutter, slowest 1% (ms)' $labels $stutS ' ms' ($latest["$($places[0]) slowest 1%"].max))
        $out += '</div></div>'
    }

    # Every number, as cards, folded away.
    $out += '<details class="allnum"><summary>All numbers</summary>'
    foreach ($test in @($h | ForEach-Object { $_.test } | Select-Object -Unique)) {
        $rows = @($h | Where-Object { $_.test -eq $test })
        $out += "<h4>$(Html $test)</h4><div class=""metrics"">"
        foreach ($metric in @($rows | ForEach-Object { $_.metric } | Select-Object -Unique)) {
            $cur = $latest[$metric]
            if (-not $cur) { continue }
            $unit = if ($cur.unit) { " $($cur.unit)" } else { '' }
            $lim = @()
            if ($null -ne $cur.max) { $lim += "at most $($cur.max)$unit" }
            if ($null -ne $cur.min) { $lim += "at least $($cur.min)$unit" }
            $limText = if ($lim.Count) { 'limit ' + ($lim -join ', ') } else { 'recorded only' }
            $cls = if ($null -eq $cur.max -and $null -eq $cur.min) { 'rec' } elseif ($cur.ok) { 'ok' } else { 'bad' }
            $out += "<div class=""metric $cls""><div class=""mname"">$(Html $metric)</div><div class=""mval"">$(& $F $cur.value '0.#')<span class=""munit"">$(Html $unit)</span></div><div class=""mlim"">$(Html $limText)</div>"
            $out += (Spark $series[$metric] $cur.max $cur.min ([bool]$cur.ok)) + '</div>'
        }
        $out += '</div>'
    }
    return $out + '</details></section>'
}
function Stamp([string]$st) {
    'last run ' + [datetime]::ParseExact($st, 'yyyyMMdd-HHmmss', [Globalization.CultureInfo]::InvariantCulture).ToString('yyyy-MM-dd HH:mm')
}

$passed = @($Records | Where-Object { $_.pass }).Count
$minutes = [int]((Get-Date) - $Started).TotalMinutes
$sb = New-Object System.Text.StringBuilder
[void]$sb.Append(@"
<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>Atmosphere Tests</title>
<style>
:root { --bg: #f6f5f2; --card: #ffffff; --ink: #1d1c1a; --dim: #6b6862; --line: #e2dfd8; --pass: #2e7d4f; --fail: #b3261e; }
@media (prefers-color-scheme: dark) {
  :root:not([data-theme="light"]) { --bg: #161615; --card: #201f1d; --ink: #ecebe7; --dim: #9c988f; --line: #33312d; --pass: #4caf7a; --fail: #e5534b; }
}
:root[data-theme="dark"] { --bg: #161615; --card: #201f1d; --ink: #ecebe7; --dim: #9c988f; --line: #33312d; --pass: #4caf7a; --fail: #e5534b; }
* { box-sizing: border-box; }
body { margin: 0; background: var(--bg); color: var(--ink); font: 15px/1.5 system-ui, -apple-system, "Segoe UI", sans-serif; }
main { max-width: 1400px; margin: 0 auto; padding: 24px 16px 64px; }
h1 { font-size: 22px; margin: 0 0 4px; }
.sum { color: var(--dim); margin: 0 0 16px; }
nav { display: flex; flex-wrap: wrap; gap: 8px; margin-bottom: 24px; }
nav a { text-decoration: none; color: var(--ink); background: var(--card); border: 1px solid var(--line); border-radius: 999px; padding: 4px 12px; font-size: 14px; }
nav a.fail { border-color: var(--fail); }
section { background: var(--card); border: 1px solid var(--line); border-radius: 10px; padding: 18px; margin-bottom: 22px; }
h2 { font-size: 18px; margin: 0 0 6px; display: flex; gap: 10px; align-items: center; flex-wrap: wrap; overflow-wrap: anywhere; }
.tag { font-size: 12px; font-weight: 700; letter-spacing: .04em; padding: 2px 8px; border-radius: 4px; color: #fff; }
.tag.pass { background: var(--pass); }
.tag.fail { background: var(--fail); }
.about { color: var(--dim); margin: 0 0 12px; }
ul.checks { list-style: none; padding: 0; margin: 0 0 12px; }
ul.checks li { padding: 4px 0 4px 22px; position: relative; }
ul.checks li::before { position: absolute; left: 0; font-weight: 700; }
ul.checks li.ok::before { content: "\2713"; color: var(--pass); }
ul.checks li.bad::before { content: "\2717"; color: var(--fail); }
.got { color: var(--dim); font-size: 13px; display: block; }
.shot { border-top: 1px solid var(--line); padding-top: 12px; margin-top: 12px; }
.shot h3 { font-size: 14px; margin: 0 0 8px; font-weight: 600; }
.pair { display: grid; grid-template-columns: 1fr 1fr; gap: 12px; }
@media (max-width: 760px) { .pair { grid-template-columns: 1fr; } }
figure { margin: 0; min-width: 0; }
figure img { width: 100%; height: auto; display: block; border-radius: 6px; border: 1px solid var(--line); }
video { width: 100%; height: auto; display: block; border-radius: 6px; border: 1px solid var(--line); background: #000; }
/* The two sides of a pair in one frame (2026-10-07): a shot taken at another window size (1280x800 against 1152x864)
   sat shorter than the one beside it. 4:3 is the test client's; cover crops the other shape's edges. The link opens
   the whole shot. */
.pair figure img, .pair video { aspect-ratio: 4 / 3; object-fit: cover; }
img.sheet { width: 100%; height: auto; display: block; margin-top: 6px; border-radius: 6px; }
figcaption { font-size: 12px; color: var(--dim); margin-top: 4px; }
.none { border: 1px dashed var(--line); border-radius: 6px; aspect-ratio: 16 / 10; display: grid; place-items: center; color: var(--dim); font-size: 13px; text-align: center; padding: 8px; }
details { margin-top: 12px; color: var(--dim); font-size: 13px; }
pre { white-space: pre-wrap; margin: 6px 0 0; font-size: 12px; }
section.perf h3 { font-size: 16px; margin: 0; }
.verdict { font-size: 16px; font-weight: 600; margin: 4px 0 16px; }
.verdict.ok { color: var(--pass); }
.verdict.bad { color: var(--fail); }
.block { border-top: 1px solid var(--line); padding-top: 14px; margin-top: 14px; }
.bhead { display: flex; gap: 10px; align-items: baseline; flex-wrap: wrap; margin-bottom: 6px; }
.when { font-size: 12px; font-weight: 400; color: var(--dim); }
.big { font-size: 26px; font-weight: 700; margin: 2px 0 6px; }
.big.ok { color: var(--pass); }
.big.bad { color: var(--fail); }
.kvs { display: grid; gap: 3px; margin: 4px 0 8px; }
.kv { display: grid; grid-template-columns: 11em 1fr; gap: 10px; align-items: baseline; }
.kv .k { color: var(--dim); font-size: 13px; }
.kv .v { font-weight: 600; font-variant-numeric: tabular-nums; }
.kv .v .lim { font-weight: 400; margin-left: 6px; }
.place .kv { grid-template-columns: 7.5em 1fr; }
.sw { display: inline-block; width: 10px; height: 10px; border-radius: 2px; vertical-align: -1px; margin-left: 4px; }
.sw.game { background: var(--dim); }
.sw.fx { background: #d08a2e; }
.lim { font-size: 13px; color: var(--dim); }
.places { display: grid; grid-template-columns: repeat(auto-fill, minmax(260px, 1fr)); gap: 14px; margin-top: 10px; }
.place { border: 1px solid var(--line); border-radius: 8px; padding: 12px 14px; min-width: 0; }
.place.bad { border-color: var(--fail); }
.pname { font-weight: 600; }
.pfps { font-size: 15px; margin: 2px 0 8px; }
.pfps b { font-size: 26px; }
.place.ok .pfps b { color: var(--pass); }
.place.bad .pfps b { color: var(--fail); }
.bar { position: relative; height: 14px; border-radius: 4px; background: var(--line); overflow: hidden; display: flex; margin-bottom: 6px; }
.bar .game { background: var(--dim); }
.bar .fx { background: #d08a2e; }
.bar .mark { position: absolute; top: 0; bottom: 0; width: 2px; background: var(--ink); }
.charts { display: grid; grid-template-columns: repeat(auto-fill, minmax(340px, 1fr)); gap: 16px; margin-top: 16px; }
@media (max-width: 480px) { .charts { grid-template-columns: 1fr; } }
figure.chartbox { margin: 10px 0 0; min-width: 0; }
figure.chartbox figcaption { font-size: 13px; font-weight: 600; color: var(--ink); margin-bottom: 4px; }
svg.chart { width: 100%; height: auto; display: block; }
svg.chart .grid { stroke: var(--line); stroke-width: 1; }
svg.chart .limit { stroke: var(--fail); stroke-dasharray: 5 4; stroke-width: 1.2; opacity: .8; }
svg.chart .ax { font-size: 12px; fill: var(--dim); }
.legend { display: flex; flex-wrap: wrap; gap: 6px 14px; font-size: 13px; margin-top: 4px; }
.legend .key { display: inline-flex; align-items: center; gap: 6px; }
.legend .key i { width: 12px; height: 12px; border-radius: 3px; display: inline-block; }
.legend .key b { font-weight: 600; }
details.allnum { margin-top: 18px; }
details.fold { margin: 0; color: var(--ink); font-size: inherit; }
details.fold > summary { cursor: pointer; list-style: none; }
details.fold > summary::-webkit-details-marker { display: none; }
details.fold > summary h2 { display: inline; }
details.fold > summary h2::before { content: "+ "; color: var(--dim); }
details.fold[open] > summary h2::before { content: "- "; }
details.fold[open] > summary { display: block; margin-bottom: 12px; }
a.dlg { color: var(--ink); text-decoration: underline; text-decoration-style: dotted; text-underline-offset: 3px; }
dialog.ctl { background: var(--card); color: var(--ink); border: 1px solid var(--line); border-radius: 10px; padding: 16px 18px; width: min(640px, 92vw); max-height: 82vh; }
dialog.ctl::backdrop { background: rgba(0, 0, 0, .55); }
dialog.ctl .dhead { display: flex; justify-content: space-between; align-items: center; gap: 12px; }
dialog.ctl .dhead button { background: none; border: 1px solid var(--line); color: var(--ink); border-radius: 6px; width: 30px; height: 30px; cursor: pointer; font-size: 14px; }
dialog.ctl h4 { margin: 14px 0 4px; font-size: 14px; }
dialog.ctl table { width: 100%; border-collapse: collapse; font-size: 13px; }
dialog.ctl td { padding: 4px 6px; border-bottom: 1px solid var(--line); }
dialog.ctl td.val { text-align: right; font-weight: 600; font-variant-numeric: tabular-nums; width: 5em; }
dialog.ctl td.cv { color: var(--dim); font-size: 12px; width: 13em; overflow-wrap: anywhere; }
dialog.ctl td.ik { font-family: ui-monospace, Consolas, monospace; font-size: 12px; width: 10em; overflow-wrap: anywhere; vertical-align: top; }
dialog.ctl td.iv { font-family: ui-monospace, Consolas, monospace; font-size: 12px; font-weight: 600; width: 7em; overflow-wrap: anywhere; vertical-align: top; }
dialog.ctl td.ic { color: var(--dim); font-size: 12px; vertical-align: top; }
dialog.ctl p.lim { margin: 2px 0 6px; }
dialog.shotview { background: none; border: 0; padding: 0; max-width: 98vw; max-height: 98vh; overflow: visible; cursor: zoom-out; }
dialog.shotview::backdrop { background: rgba(0, 0, 0, .85); }
dialog.shotview img { display: block; max-width: 96vw; max-height: 90vh; margin: 0 auto; border-radius: 6px; }
dialog.shotview p { margin: 8px 0 0; text-align: center; color: #ecebe7; font-size: 14px; }
details.allnum h4 { font-size: 14px; margin: 12px 0 8px; color: var(--ink); }
.metric.rec .mval { color: var(--ink); }
.metrics { display: grid; grid-template-columns: repeat(auto-fill, minmax(220px, 1fr)); gap: 12px; }
.metric { border: 1px solid var(--line); border-radius: 8px; padding: 10px 12px; min-width: 0; }
.metric.bad { border-color: var(--fail); }
.mname { font-size: 13px; color: var(--dim); overflow-wrap: anywhere; }
.mval { font-size: 24px; font-weight: 700; font-variant-numeric: tabular-nums; }
.metric.ok .mval { color: var(--pass); }
.metric.bad .mval { color: var(--fail); }
.munit { font-size: 13px; font-weight: 400; color: var(--dim); }
.mlim { font-size: 12px; color: var(--dim); }
svg.spark { width: 100%; height: 46px; display: block; margin: 6px 0 2px; }
svg.spark .trend { fill: none; stroke: var(--ink); stroke-width: 1.5; vector-effect: non-scaling-stroke; }
svg.spark .limit { stroke: var(--dim); stroke-dasharray: 4 3; stroke-width: 1; vector-effect: non-scaling-stroke; }
svg.spark .dot.ok { fill: var(--pass); }
svg.spark .dot.bad { fill: var(--fail); }
</style></head><body><main>
<h1>Atmosphere tests</h1>
<p class="sum">$passed of $($Records.Count) passed &middot; $(if ($Summary) { Html $Summary } else { "$($Started.ToString('yyyy-MM-dd HH:mm')) &middot; $minutes min" })</p>
"@)
[void]$sb.Append((PerfPanel))
# The tests that measure come first (2026-10-08, the owner): the performance tests, then the ones that measure what a
# control costs in frame time (volume-cost, volume-parts, water-cost), then the rest by name.
$Records = @($Records | Sort-Object { if ($_.name -like '*-performance') { 0 } elseif ($_.name -match '-(cost|parts)$') { 1 } else { 2 } }, name)
[void]$sb.Append("<nav>`n")
foreach ($r in $Records) {
    $mark = if ($r.pass) { '&#10003;' } else { '&#10007;' }
    $target = if ($r.name -like '*-performance') { 'performance' } else { $r.name }
    [void]$sb.Append("<a href=""#$(Html $target)"" class=""$(if ($r.pass) { 'pass' } else { 'fail' })"">$mark $(Html $r.name)</a>`n")
}
[void]$sb.Append("</nav>`n")

# A performance test has no section of its own (the owner, 2026-10-06): the Performance panel above shows its
# numbers, all of them under "All numbers", and its pill links there.
foreach ($r in $Records) {
    if ($r.name -like '*-performance') { continue }
    $cls = if ($r.pass) { 'pass' } else { 'fail' }
    $runOf = if ($Summary -and $r.stamp) { " <span class=""when"">$(Html (Stamp $r.stamp))</span>" } else { '' }
    # A test that measures a control's cost is folded (2026-10-08, the owner: its numbers are not for a person to
    # read through); it opens when it failed, or from its link in the list.
    $fold = $r.name -match '-(cost|parts)$'
    $head = "<h2>$(Html $r.name) <span class=""tag $cls"">$($cls.ToUpper())</span>$runOf</h2>"
    if ($fold) { $head = "<details class=""fold""$(if (-not $r.pass) { ' open' })><summary>$head</summary>" }
    [void]$sb.Append("<section id=""$(Html $r.name)"">$head`n")
    [void]$sb.Append("<p class=""about"">$(Html $r.about)</p>`n")
    [void]$sb.Append((Checks @($r.checks | Where-Object { $_.shot -eq 0 })))
    for ($i = 0; $i -lt $r.shots.Count; $i++) {
        $n = $i + 1
        $v = if ($i -lt $r.views.Count) { [int]$r.views[$i] } else { 0 }
        $label = if ($v -eq 0) { 'Normal view' } elseif ($v -lt $ViewNames.Count) { "Debug View ${v}: $($ViewNames[$v])" } else { "Debug View $v" }
        $got = [IO.Path]::GetFileName($r.shots[$i])
        $expName = "$($r.name)-$n$([IO.Path]::GetExtension($r.shots[$i]))"
        [void]$sb.Append("<div class=""shot""><h3>Shot ${n}: $(Html $label)</h3>")
        [void]$sb.Append((Checks @($r.checks | Where-Object { $_.shot -eq $n })))
        [void]$sb.Append("<div class=""pair""><figure><a href=""$(Html $got)""><img src=""$(Html $got)"" alt=""This run, shot $n"" loading=""lazy""></a><figcaption>This run</figcaption></figure>")
        if (Test-Path (Join-Path $ExpectedDir $expName)) {
            $rel = "../expected/$expName"
            [void]$sb.Append("<figure><a href=""$(Html $rel)""><img src=""$(Html $rel)"" alt=""Expected, shot $n"" loading=""lazy""></a><figcaption>Expected</figcaption></figure>")
        }
        else {
            [void]$sb.Append("<figure><div class=""none"">No expected shot yet. Run with -Accept to take this one.</div><figcaption>Expected</figcaption></figure>")
        }
        [void]$sb.Append("</div></div>`n")
    }
    # The recordings (2026-10-05): each plays at once, without sound, over and over. The sheet of its frames
    # is under it, closed.
    for ($i = 0; $i -lt $r.recordings.Count; $i++) {
        $n = $i + 1
        $v = if ($i -lt $r.recViews.Count) { [int]$r.recViews[$i] } else { 0 }
        $label = if ($v -eq 0) { 'Normal view' } elseif ($v -lt $ViewNames.Count) { "Debug View ${v}: $($ViewNames[$v])" } else { "Debug View $v" }
        if ($i -lt $r.recLabels.Count -and $r.recLabels[$i]) { $label = "$($r.recLabels[$i]), $label" }
        $got = [IO.Path]::GetFileName($r.recordings[$i])
        $sheet = $got -replace '\.mp4$', '-sheet.jpg'
        [void]$sb.Append("<div class=""shot""><h3>Recording ${n}: $(Html $label)</h3>")
        # Beside the accepted one (expected\<test>-rec<n>.mp4, 2026-10-05), as a screenshot is.
        $expName = "$($r.name)-rec$n.mp4"
        [void]$sb.Append("<div class=""pair""><figure><video src=""$(Html $got)"" autoplay muted loop playsinline controls></video><figcaption>This run</figcaption></figure>")
        if (Test-Path (Join-Path $ExpectedDir $expName)) {
            [void]$sb.Append("<figure><video src=""../expected/$(Html $expName)"" autoplay muted loop playsinline controls></video><figcaption>Expected</figcaption></figure>")
        }
        else {
            [void]$sb.Append("<figure><div class=""none"">No expected recording yet. Run with -Accept to take this one.</div><figcaption>Expected</figcaption></figure>")
        }
        [void]$sb.Append("</div>")
        [void]$sb.Append("<details><summary>20 frames of it</summary><a href=""$(Html $sheet)""><img class=""sheet"" src=""$(Html $sheet)"" alt=""20 frames of recording $n"" loading=""lazy""></a></details></div>`n")
    }
    if (-not $r.shots.Count -and -not $r.recordings.Count) { [void]$sb.Append("<p class=""about"">No screenshot taken.</p>`n") }
    $more = @()
    if ($r.positions.Count) { $more += 'Where the character was:'; $more += @($r.positions | ForEach-Object { "  $_" }) }
    if ($r.warnings.Count) { $more += 'Errors from the game:'; $more += @($r.warnings | ForEach-Object { "  $_" }) }
    if ($more.Count) { [void]$sb.Append("<details><summary>Positions and messages</summary><pre>$(Html ($more -join "`n"))</pre></details>`n") }
    [void]$sb.Append("$(if ($fold) { '</details>' })</section>`n")
}
# The dialogs' links (2026-10-06): a click opens the dialog; a click on its backdrop closes it.
[void]$sb.Append(@'
<script>
// A link to a folded test opens it.
function openFold() { var s = location.hash && document.getElementById(location.hash.slice(1)); var f = s && s.querySelector('details.fold'); if (f) f.open = true; }
window.addEventListener('hashchange', openFold); openFold();
document.querySelectorAll('a.dlg').forEach(function (a) {
  a.addEventListener('click', function (e) { e.preventDefault(); var d = document.getElementById(a.dataset.dialog); if (d) d.showModal(); });
});
document.querySelectorAll('dialog').forEach(function (d) {
  d.addEventListener('click', function (e) { if (e.target === d) d.close(); });
});
// A shot opens large in a dialog (2026-10-08, the owner). The arrow keys go to the next or the last shot on the
// page, so this run and the expected one can be flipped between; a click anywhere or Escape closes it. A click with
// Ctrl or the middle button still opens the file.
(function () {
  var links = Array.prototype.slice.call(document.querySelectorAll('.pair figure a, details a'));
  var d = document.createElement('dialog'); d.className = 'shotview';
  d.innerHTML = '<img alt=""><p></p>';
  document.body.appendChild(d);
  var img = d.querySelector('img'), cap = d.querySelector('p'), at = 0;
  function show(i) {
    at = (i + links.length) % links.length;
    var a = links[at], sec = a.closest('section'), shot = a.closest('.shot'), fig = a.closest('figure');
    img.src = a.getAttribute('href');
    cap.textContent = [sec && sec.querySelector('h2') ? sec.id : '', shot && shot.querySelector('h3') ? shot.querySelector('h3').textContent : '',
      fig && fig.querySelector('figcaption') ? fig.querySelector('figcaption').textContent : ''].filter(Boolean).join('  ' + String.fromCharCode(183) + '  ') +
      '   (' + (at + 1) + ' of ' + links.length + ')';
  }
  links.forEach(function (a, i) {
    a.addEventListener('click', function (e) {
      if (e.ctrlKey || e.metaKey || e.shiftKey || e.button !== 0) return;
      e.preventDefault(); show(i); d.showModal();
    });
  });
  d.addEventListener('click', function () { d.close(); });
  d.addEventListener('keydown', function (e) {
    if (e.key === 'ArrowRight' || e.key === 'ArrowDown') { e.preventDefault(); show(at + 1); }
    else if (e.key === 'ArrowLeft' || e.key === 'ArrowUp') { e.preventDefault(); show(at - 1); }
  });
})();
</script>
'@)
[void]$sb.Append("</main></body></html>`n")
[IO.File]::WriteAllText($Page, $sb.ToString(), (New-Object System.Text.UTF8Encoding $false))
