<#
.SYNOPSIS
  Writes the page of a test run: Run-Tests.ps1 calls it at the end of each run (2026-10-04).

.DESCRIPTION
  One section a test: whether it passed, what it is about, each check and what it measured, and each
  screenshot beside the expected one (expected\<test>-<n>.jpg, taken with Run-Tests.ps1 -Accept). A check that
  reads a screenshot is shown with that screenshot. The page lies in results\ beside the screenshots, so it
  links them by name and the expected ones by ..\expected\.
#>
param(
    [Parameter(Mandatory)][object[]]$Records,
    [Parameter(Mandatory)][string]$Page,
    [Parameter(Mandatory)][string]$ExpectedDir,
    [string[]]$ViewNames = @(),
    [datetime]$Started = (Get-Date)
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
img.sheet { width: 100%; height: auto; display: block; margin-top: 6px; border-radius: 6px; }
figcaption { font-size: 12px; color: var(--dim); margin-top: 4px; }
.none { border: 1px dashed var(--line); border-radius: 6px; aspect-ratio: 16 / 10; display: grid; place-items: center; color: var(--dim); font-size: 13px; text-align: center; padding: 8px; }
details { margin-top: 12px; color: var(--dim); font-size: 13px; }
pre { white-space: pre-wrap; margin: 6px 0 0; font-size: 12px; }
</style></head><body><main>
<h1>Atmosphere tests</h1>
<p class="sum">$passed of $($Records.Count) passed &middot; $($Started.ToString('yyyy-MM-dd HH:mm')) &middot; $minutes min</p>
<nav>
"@)
foreach ($r in $Records) {
    $mark = if ($r.pass) { '&#10003;' } else { '&#10007;' }
    [void]$sb.Append("<a href=""#$(Html $r.name)"" class=""$(if ($r.pass) { 'pass' } else { 'fail' })"">$mark $(Html $r.name)</a>`n")
}
[void]$sb.Append("</nav>`n")

foreach ($r in $Records) {
    $cls = if ($r.pass) { 'pass' } else { 'fail' }
    [void]$sb.Append("<section id=""$(Html $r.name)""><h2>$(Html $r.name) <span class=""tag $cls"">$($cls.ToUpper())</span></h2>`n")
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
        [void]$sb.Append("<video src=""$(Html $got)"" autoplay muted loop playsinline controls></video>")
        [void]$sb.Append("<details><summary>20 frames of it</summary><a href=""$(Html $sheet)""><img class=""sheet"" src=""$(Html $sheet)"" alt=""20 frames of recording $n"" loading=""lazy""></a></details></div>`n")
    }
    if (-not $r.shots.Count -and -not $r.recordings.Count) { [void]$sb.Append("<p class=""about"">No screenshot taken.</p>`n") }
    $more = @()
    if ($r.positions.Count) { $more += 'Where the character was:'; $more += @($r.positions | ForEach-Object { "  $_" }) }
    if ($r.warnings.Count) { $more += 'Errors from the game:'; $more += @($r.warnings | ForEach-Object { "  $_" }) }
    if ($more.Count) { [void]$sb.Append("<details><summary>Positions and messages</summary><pre>$(Html ($more -join "`n"))</pre></details>`n") }
    [void]$sb.Append("</section>`n")
}
[void]$sb.Append("</main></body></html>`n")
[IO.File]::WriteAllText($Page, $sb.ToString(), (New-Object System.Text.UTF8Encoding $false))
