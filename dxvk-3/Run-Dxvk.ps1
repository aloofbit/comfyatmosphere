# Alternating sessions of two d3d9.dll builds on the test client. Each session: copy the DLL, log in,
# run dxvk-frames.txt, keep the ComfyTest frames lines and WoW_d3d9.log. nvidia-smi logs the GPU load.
#   .\Run-Dxvk.ps1 -Old <2.7.1 d3d9.dll> -New <3.x d3d9.dll>
param(
    [Parameter(Mandatory)] [string]$Old,
    [Parameter(Mandatory)] [string]$New,
    [string[]]$Order = @('old', 'new', 'old', 'new', 'old', 'new'),
    [string]$Out = "$PSScriptRoot\results"
)
$ErrorActionPreference = 'Continue'
$c = "$env:USERPROFILE\Desktop\wow-clients\octow - Copy"
$tool = "$env:USERPROFILE\Desktop\comfy-wow\tools\wow-test-tool"
$dll = @{ 'old' = $Old; 'new' = $New }
New-Item -ItemType Directory -Force $Out | Out-Null
$frames = "$c\Imports\comfytest-frames.txt"
$smi = Start-Process nvidia-smi -ArgumentList '--query-gpu=timestamp,utilization.gpu,clocks.gr,power.draw,temperature.gpu --format=csv -lms 1000' -RedirectStandardOutput "$Out\nvidia-smi.csv" -WindowStyle Hidden -PassThru

function Close-Client {
    foreach ($p in @(Get-Process WoW -ErrorAction SilentlyContinue)) {
        $null = $p.CloseMainWindow()
        if (-not $p.WaitForExit(15000)) { $null = $p.CloseMainWindow(); if (-not $p.WaitForExit(15000)) { Stop-Process -Id $p.Id -Force } }
    }
    Start-Sleep 3
}

$n = 0
foreach ($v in $Order) {
    $n++
    $tag = "$n-$v"
    Write-Host "=== session $tag $(Get-Date -Format HH:mm:ss)"
    Close-Client
    $ok = $false
    for ($i = 0; $i -lt 20 -and -not $ok; $i++) {
        try { Copy-Item $dll[$v] "$c\d3d9.dll" -Force -ErrorAction Stop; $ok = $true } catch { Start-Sleep 1 }
    }
    if (-not $ok) { Write-Host "copy failed"; break }
    & "$tool\Login.ps1" -Client $c -Character 1 2>&1 | Select-Object -Last 2 | Write-Host
    Copy-Item "$c\WoW_d3d9.log" "$Out\$tag-WoW_d3d9-start.log"
    Start-Sleep 15
    $before = if (Test-Path $frames) { @(Get-Content $frames).Count } else { 0 }
    & "$tool\Run-Test.ps1" "$PSScriptRoot\dxvk-frames.txt" -Client $c -TimeoutSeconds 300 2>&1 | Out-File "$Out\$tag-run.txt"
    $new = @(Get-Content $frames | Select-Object -Skip $before)
    if ($new.Count -lt 9) { $new = @(Get-Content $frames | Select-Object -Last 9) }
    $new | ForEach-Object { "$tag $_" } | Add-Content "$Out\frames.txt"
    $new | Write-Host
    Copy-Item "$c\WoW_d3d9.log" "$Out\$tag-WoW_d3d9.log"
}
Close-Client
Stop-Process -Id $smi.Id -ErrorAction SilentlyContinue
Write-Host "=== done $(Get-Date -Format HH:mm:ss)"
