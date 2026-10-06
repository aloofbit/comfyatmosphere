<#
.SYNOPSIS
  The results cards: media\test-summary.svg for the README, media\test-results.svg for a big display (2026-10-06,
  the owner).

.DESCRIPTION
  Two images of where the mod stands, from the records Build-Latest.ps1 -Card has gathered.

  The small card (-SummaryPath) goes at the top of the README: how many tests pass, the frame rate with all
  effect on, and what the effects cost in a frame. The large card (-Path) adds the frame rate at each place with
  the effects on and off, the sign-in, and each test's latest result.

  The numbers come from results\perf-history.json, the newest run of each performance test. The test system comes
  from the client's comfyatmos.log. Each card is an SVG, so GitHub draws it sharp at any width and its diff can be
  read. Its colours follow the reader's light or dark theme. An SVG shown as an image loads no fonts, so the text
  uses the system's.
#>
param(
    [Parameter(Mandatory)] $Records,
    [Parameter(Mandatory)] [string]$History,
    [Parameter(Mandatory)] [string]$Path,
    [string]$SummaryPath = '',
    [string]$Client = ''
)

$ErrorActionPreference = 'Stop'
$inv = [Globalization.CultureInfo]::InvariantCulture
function X([string]$s) { [System.Security.SecurityElement]::Escape($s) }
function N($v, [string]$f = '0') { ([double]$v).ToString($f, $inv) }

# The newest run of each performance test, and its values by name.
$h = @()
if (Test-Path $History) { $h = @((Get-Content $History -Raw | ConvertFrom-Json) | ForEach-Object { $_ }) }
function Latest([string]$test) {
    $runs = @($h | Where-Object { $_.test -eq $test })
    if (-not $runs.Count) { return @{} }
    $stamp = ($runs | ForEach-Object { $_.stamp } | Sort-Object | Select-Object -Last 1)
    $m = @{}
    foreach ($e in ($runs | Where-Object { $_.stamp -eq $stamp })) { $m[$e.metric] = $e }
    $m['_stamp'] = $stamp
    return $m
}
$fr = Latest 'frame-rate-performance'
$ld = Latest 'load-performance'

# The places, in the order the test measures them.
$places = @()
foreach ($e in ($h | Where-Object { $_.test -eq 'frame-rate-performance' -and $_.stamp -eq $fr['_stamp'] })) {
    if ($e.metric -match '^(.+) fps$' -and $places -notcontains $Matches[1]) { $places += $Matches[1] }
}

# The GPU, from the client's log.
$gpu = ''
if ($Client) {
    $log = Join-Path $Client 'comfyatmos.log'
    if (Test-Path $log) {
        $lines = @(Get-Content $log -TotalCount 400)
        $g = @($lines | Where-Object { $_ -match '^graphics: ' }) | Select-Object -Last 1
        if ($g -match '^graphics: (.+?) \(vendor') { $gpu = $Matches[1] -replace '^NVIDIA GeForce ', 'GeForce ' }
    }
}

$records = @($Records | Sort-Object name)

# The version, only when every result shown came from one DLL. Run-Tests.ps1 saves it with each result since
# 2026-10-06. Not the client's log: that names the DLL installed now, which need not be the one the tests ran on.
$shown = @($records) + @($fr.Values + $ld.Values | Where-Object { $_ -isnot [string] })
$versions = @($shown | ForEach-Object { if ($_.PSObject.Properties['version']) { "$($_.version)" } else { '' } } | Sort-Object -Unique)
$version = if ($versions.Count -eq 1 -and $versions[0]) { $versions[0] } else { '' }
$passed = @($records | Where-Object { $_.pass }).Count
$newest = ($records | ForEach-Object { $_.stamp }) + @($fr['_stamp'], $ld['_stamp']) | Where-Object { $_ } | Sort-Object | Select-Object -Last 1
$date = [datetime]::ParseExact($newest, 'yyyyMMdd-HHmmss', $inv).ToString('yyyy-MM-dd', $inv)

# Layout.
$W = 860; $pad = 32
$o = New-Object System.Text.StringBuilder
function Add([string]$s) { [void]$o.AppendLine($s) }
$y = 0

# The header.
$y = 52
$sub = @('Latest results', $date)
if ($version) { $sub += $version }
if ($gpu) { $sub += $gpu }
Add "<text x=""$pad"" y=""$y"" class=""title"">comfyatmosphere</text>"
Add "<text x=""$($W - $pad)"" y=""$y"" class=""muted"" text-anchor=""end"" xml:space=""preserve"">$(X ($sub -join "  $([char]0xB7)  "))</text>"   # a middle dot: Windows PowerShell reads this file as ANSI
$y += 22
Add "<line x1=""$pad"" y1=""$y"" x2=""$($W - $pad)"" y2=""$y"" class=""rule""/>"

# The tiles.
$y += 22
$tiles = @()
$tiles += , @("$passed of $($records.Count)", 'tests pass', $(if ($passed -eq $records.Count) { 'good' } else { 'bad' }))
if ($places.Count) {
    $p = $places[0]
    $tiles += , @("$(N $fr["$p fps"].value) fps", "$p, effects on", '')
    $cpu = @($places | ForEach-Object { $fr["$_ our CPU"] } | Where-Object { $_ } | ForEach-Object { [double]$_.value })
    if ($cpu.Count) { $tiles += , @("$(N (($cpu | Measure-Object -Maximum).Maximum) '0.0') ms", 'our CPU a frame, at most', '') }
}
if ($ld.Count) {
    $c = $ld["Compiling on the game's thread"]
    if ($c) { $tiles += , @("$(N $c.value) ms", 'shader compiling at sign-in', '') }
}
$tw = ($W - 2 * $pad - 16 * ($tiles.Count - 1)) / $tiles.Count
for ($i = 0; $i -lt $tiles.Count; $i++) {
    $tx = $pad + $i * ($tw + 16)
    $cls = if ($tiles[$i][2]) { "big $($tiles[$i][2])" } else { 'big' }
    Add "<rect x=""$(N $tx)"" y=""$y"" width=""$(N $tw)"" height=""78"" rx=""10"" class=""tile""/>"
    Add "<text x=""$(N ($tx + 18))"" y=""$($y + 40)"" class=""$cls"">$(X $tiles[$i][0])</text>"
    Add "<text x=""$(N ($tx + 18))"" y=""$($y + 62)"" class=""muted"">$(X $tiles[$i][1])</text>"
}
$y += 78

# The frame rate: a bar for each place, the effects on over the effects off.
if ($places.Count) {
    $y += 46
    Add "<text x=""$pad"" y=""$y"" class=""head"">Frame rate</text>"
    Add "<rect x=""$($W - $pad - 196)"" y=""$($y - 10)"" width=""12"" height=""10"" rx=""2"" class=""on""/>"
    Add "<text x=""$($W - $pad - 178)"" y=""$y"" class=""small"">effects on</text>"
    Add "<rect x=""$($W - $pad - 104)"" y=""$($y - 10)"" width=""12"" height=""10"" rx=""2"" class=""off""/>"
    Add "<text x=""$($W - $pad - 86)"" y=""$y"" class=""small"">effects off</text>"
    $top = 0
    foreach ($p in $places) { foreach ($k in "$p fps", "$p fps without effects") { if ($fr[$k] -and [double]$fr[$k].value -gt $top) { $top = [double]$fr[$k].value } } }
    $top = [math]::Ceiling($top / 50) * 50
    $bx = $pad + 120; $bw = $W - $pad - 250 - $bx
    $y += 14
    foreach ($p in $places) {
        $on = [double]$fr["$p fps"].value
        $off = if ($fr["$p fps without effects"]) { [double]$fr["$p fps without effects"].value } else { 0 }
        $y += 16
        Add "<text x=""$pad"" y=""$($y + 15)"" class=""label"">$(X $p)</text>"
        if ($off) { Add "<rect x=""$bx"" y=""$y"" width=""$(N ($bw * $off / $top))"" height=""20"" rx=""4"" class=""off""/>" }
        Add "<rect x=""$bx"" y=""$y"" width=""$(N ($bw * $on / $top))"" height=""20"" rx=""4"" class=""on""/>"
        Add "<text x=""$($W - $pad - 236)"" y=""$($y + 15)"" class=""value"">$(N $on) fps</text>"
        $note = @()
        if ($off) { $note += "$(N $off) off" }
        $slow = $fr["$p slowest 1%"]
        if ($slow) { $note += "1% low $(N (1000 / [double]$slow.value)) fps" }
        Add "<text x=""$($W - $pad)"" y=""$($y + 15)"" class=""small"" text-anchor=""end"">$(X ($note -join ', '))</text>"
        $y += 20
    }
}

# The sign-in: when the effects start after the world shows, and what the first frames cost.
if ($ld.Count) {
    $y += 46
    Add "<text x=""$pad"" y=""$y"" class=""head"">Sign-in</text>"
    $y += 12
    $items = @()
    foreach ($m in @(
            @('Effects start after the world', 's', '0.0', 'effects start'),
            @('Worst frame as the effects start', 'ms', '0', 'worst frame'),
            @('Slow frames as the effects start', '', '0', 'slow frames'),
            @("Compiling on the game's thread", 'ms', '0', 'compiling on the game thread'))) {
        $e = $ld[$m[0]]
        if (-not $e) { continue }
        $v = (N $e.value $m[2]) + $(if ($m[1]) { " $($m[1])" } else { '' })
        $lim = if ($null -ne $e.max) { "limit $(N $e.max)$(if ($m[1]) { " $($m[1])" })" } else { '' }
        $items += , @($v, $m[3], $lim, [bool]$e.ok)
    }
    $cw = ($W - 2 * $pad) / [math]::Max(1, $items.Count)
    for ($i = 0; $i -lt $items.Count; $i++) {
        $cx = $pad + $i * $cw
        Add "<text x=""$(N $cx)"" y=""$($y + 26)"" class=""value"">$(X $items[$i][0])</text>"
        Add "<text x=""$(N $cx)"" y=""$($y + 46)"" class=""small"">$(X $items[$i][1])</text>"
        if ($items[$i][2]) {
            $cls = if ($items[$i][3]) { 'small good' } else { 'small bad' }
            Add "<text x=""$(N $cx)"" y=""$($y + 64)"" class=""$cls"">$(X $items[$i][2])</text>"
        }
    }
    $y += 64
}

# The tests: a mark and a name each, in three columns.
$y += 46
Add "<text x=""$pad"" y=""$y"" class=""head"">Tests</text>"
Add "<text x=""$($W - $pad)"" y=""$y"" class=""small"" text-anchor=""end"">each runs in the game with nobody at the keyboard</text>"
$y += 14
$cols = 3; $rows = [math]::Ceiling($records.Count / $cols)
$cw = ($W - 2 * $pad) / $cols
for ($i = 0; $i -lt $records.Count; $i++) {
    $r = $records[$i]
    $cx = $pad + [math]::Floor($i / $rows) * $cw
    $cy = $y + 22 + ($i % $rows) * 24
    if ($r.pass) {
        Add "<circle cx=""$(N ($cx + 8))"" cy=""$($cy - 5)"" r=""8"" class=""dot-good""/>"
        Add "<path d=""M$(N ($cx + 4)) $($cy - 5) l3 3 l5 -6"" class=""tick""/>"
    }
    else {
        Add "<circle cx=""$(N ($cx + 8))"" cy=""$($cy - 5)"" r=""8"" class=""dot-bad""/>"
        Add "<path d=""M$(N ($cx + 5)) $($cy - 8) l6 6 m0 -6 l-6 6"" class=""tick""/>"
    }
    Add "<text x=""$(N ($cx + 24))"" y=""$cy"" class=""name"">$(X $r.name)</text>"
}
$y += 22 + ($rows - 1) * 24 + 32

# Both cards share one style and are written the same way.
$style = @"
<style>
  :root { --bg: #ffffff; --tile: #f6f8fa; --line: #d0d7de; --text: #1f2328; --muted: #59636e;
          --on: #0969da; --off: #c8d1da; --good: #1a7f37; --bad: #cf222e; }
  @media (prefers-color-scheme: dark) {
    :root { --bg: #0d1117; --tile: #161b22; --line: #30363d; --text: #e6edf3; --muted: #9198a1;
            --on: #4493f8; --off: #2d333b; --good: #3fb950; --bad: #f85149; }
  }
  text { font-family: -apple-system, 'Segoe UI', 'Noto Sans', Helvetica, Arial, sans-serif; fill: var(--text); }
  .card { fill: var(--bg); stroke: var(--line); }
  .rule { stroke: var(--line); }
  .tile { fill: var(--tile); }
  .title { font-size: 24px; font-weight: 600; }
  .head { font-size: 16px; font-weight: 600; }
  .big { font-size: 26px; font-weight: 600; }
  .value { font-size: 15px; font-weight: 600; }
  .label, .name { font-size: 13.5px; }
  .muted, .small { fill: var(--muted); }
  .muted { font-size: 13px; }
  .small { font-size: 12.5px; }
  .good { fill: var(--good); }
  .bad { fill: var(--bad); }
  .on { fill: var(--on); }
  .off { fill: var(--off); }
  .dot-good { fill: var(--good); }
  .dot-bad { fill: var(--bad); }
  .tick { fill: none; stroke: #ffffff; stroke-width: 2; stroke-linecap: round; stroke-linejoin: round; }
</style>
"@
function Save([string]$file, [int]$height, [string]$body) {
    $label = "comfyatmosphere test results, ${date}: $passed of $($records.Count) tests pass"
    $svg = "<svg xmlns=""http://www.w3.org/2000/svg"" width=""$W"" height=""$height"" viewBox=""0 0 $W $height"" role=""img"" aria-label=""$(X $label)"">`n" +
           $style.TrimEnd() + "`n" +
           "<rect x=""0.5"" y=""0.5"" width=""$($W - 1)"" height=""$($height - 1)"" rx=""14"" class=""card""/>`n" +
           $body.TrimEnd() + "`n</svg>`n"
    $dir = Split-Path $file
    if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory $dir | Out-Null }
    [IO.File]::WriteAllText($file, $svg.Replace("`r`n", "`n"), (New-Object System.Text.UTF8Encoding $false))
    Write-Host "card: $file"
}
Save $Path $y $o.ToString()

# The small card, for the top of the README: one line of where it was measured, and three numbers.
if ($SummaryPath) {
    [void]$o.Clear()
    $sub = @($date)
    if ($version) { $sub += $version }
    if ($gpu) { $sub += $gpu }
    Add "<text x=""$pad"" y=""40"" class=""head"">Test results</text>"
    Add "<text x=""$($W - $pad)"" y=""40"" class=""muted"" text-anchor=""end"" xml:space=""preserve"">$(X ($sub -join "  $([char]0xB7)  "))</text>"
    $tiles = @()
    $tiles += , @("$passed of $($records.Count)", 'in-game tests', $(if ($passed -eq $records.Count) { 'good' } else { 'bad' }))
    $on = @($places | ForEach-Object { [double]$fr["$_ fps"].value })
    # What the effects cost, from the frame rate with them and without: 1000/on - 1000/off ms a frame.
    $cost = @($places | Where-Object { $fr["$_ fps without effects"] } |
              ForEach-Object { 1000 / [double]$fr["$_ fps"].value - 1000 / [double]$fr["$_ fps without effects"].value })
    function Span($v, [string]$f, [string]$unit) {
        $lo = ($v | Measure-Object -Minimum).Minimum; $hi = ($v | Measure-Object -Maximum).Maximum
        if ((N $lo $f) -eq (N $hi $f)) { return "$(N $lo $f) $unit" }
        return "$(N $lo $f) to $(N $hi $f) $unit"
    }
    if ($on.Count) { $tiles += , @((Span $on '0' 'fps'), "all effects, $($places.Count) places", '') }
    if ($cost.Count) { $tiles += , @((Span $cost '0.0' 'ms'), 'frame cost', '') }
    $tw = ($W - 2 * $pad - 16 * ($tiles.Count - 1)) / $tiles.Count
    $ty = 60
    for ($i = 0; $i -lt $tiles.Count; $i++) {
        $tx = $pad + $i * ($tw + 16)
        $cls = if ($tiles[$i][2]) { "big $($tiles[$i][2])" } else { 'big' }
        Add "<rect x=""$(N $tx)"" y=""$ty"" width=""$(N $tw)"" height=""78"" rx=""10"" class=""tile""/>"
        Add "<text x=""$(N ($tx + 18))"" y=""$($ty + 40)"" class=""$cls"">$(X $tiles[$i][0])</text>"
        Add "<text x=""$(N ($tx + 18))"" y=""$($ty + 62)"" class=""muted"">$(X $tiles[$i][1])</text>"
    }
    Save $SummaryPath ($ty + 78 + 28) $o.ToString()
}
