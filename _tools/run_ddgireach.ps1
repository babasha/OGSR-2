# DDGI REACH vs THE TRACED WORLD - does the probe ladder have a world to measure?
#
# The question this run answers is NOT "are the probes good". It is: the ladder
# spans 128 m from the camera (5 cascades x 16 probes x 1 m, doubling), and the
# BLAS bubble the probes trace is r_rtao_range = 64 m. Everything past 64 m is an
# EMPTY structure, so the outer shell can only report unoccluded sky. Making the
# probes sparser to cover more ground therefore buys nothing until the bubble
# grows - and the bubble's price is the thing nobody has measured.
#
# What this run CAN decide:
#   * does the new "[VK DDGI] WASTED REACH" line print, and what does it name;
#   * what a bigger bubble COSTS: triangles, collect ms, BLAS MB, frame time, at
#     r_rtao_range 64 / 96 / 128 and then at a coarser r_rt_lod, all in ONE run;
#   * whether the "all cascades inside" line ever appears (i.e. the ladder and
#     the traced world finally agree).
# What it CANNOT decide:
#   * whether the picture is better. Image A/B between runs is invalid (weather
#     and actor position drift); the shots here are for cost, not for looks.
#
# WARNING r_rtao_range and r_rt_lod are SAVABLE cvars. The client is force-killed
# at the end, so user.ltx is normally not rewritten - but CHECK it afterwards, a
# stuck 128 there would silently become the baseline of every later run.
#
# ASCII only (the .ps1 encoding trap).
param([string]$Tag = "REACH", [string]$Level = "pripyat_full", [int]$Play = 220,
      [int]$Validate = 0, [string]$Shots = "300,600,900,1200,1500,1800",
      [string]$Cmds = "time_factor 0.001|r_rtao_range 96|r_rtao_range 128|r_rt_lod 100|r_rtao_range 64|r_rt_lod 200",
      [string]$Pre = "r_profiler 2;r_gi 3", [string]$OutDir = "")

$game = "D:\OGSR Gunslinger Addon"
$logs = "$game\_appdata_\logs"
$scratch = $PSScriptRoot
if ($OutDir -eq "") { $OutDir = "$scratch\shots_$Tag" }
if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory $OutDir | Out-Null }
$chan = "$scratch\cl_cmds_$Tag.txt"
Remove-Item $chan -ErrorAction SilentlyContinue
New-Item -ItemType File $chan | Out-Null

# One instance only: a launch on top of a still-closing client exits SILENTLY.
while (Get-Process xrEngine_VK -ErrorAction SilentlyContinue) {
    Stop-Process -Name xrEngine_VK -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2
}
Start-Sleep -Seconds 2

$before = @(Get-ChildItem "$logs\xray_$($env:USERNAME)_*.log" | Select-Object -ExpandProperty Name)
$env:XROS_CLIENT_CONSOLE = $chan
$env:XROS_AUTO_SHOT      = "$OutDir\shot_"
$env:XROS_AUTO_SHOT_AT   = $Shots
$env:XROS_AUTO_SHOT_CMDS = $Cmds

$launchArgs = ""
if ($Validate -ne 0) { $launchArgs = "-vk_validation" }
if ($launchArgs -ne "") {
    $p = Start-Process -FilePath "$game\bin_x64\xrEngine_VK.exe" -WorkingDirectory "$game\bin_x64" -PassThru -ArgumentList $launchArgs
} else {
    $p = Start-Process -FilePath "$game\bin_x64\xrEngine_VK.exe" -WorkingDirectory "$game\bin_x64" -PassThru
}

$fresh = $null
for ($i = 0; $i -lt 60; $i++) {
    Start-Sleep -Seconds 1
    $now = Get-ChildItem "$logs\xray_$($env:USERNAME)_*.log" -ErrorAction SilentlyContinue
    $fresh = $now | Where-Object { $before -notcontains $_.Name } | Sort-Object LastWriteTime | Select-Object -Last 1
    if ($fresh) { break }
}
if (-not $fresh) {
    Write-Output "NO FRESH LOG - the launch was swallowed"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    exit 1
}
Write-Output "log: $($fresh.FullName)"

for ($i = 0; $i -lt 60; $i++) {
    Start-Sleep -Seconds 1
    if (Select-String -Path $fresh.FullName -Pattern "\[cl_console\] watching" -Quiet -ErrorAction SilentlyContinue) { break }
}
Start-Sleep -Seconds 3
if ($Pre -ne "") {
    foreach ($c in $Pre.Split(";")) {
        $c = $c.Trim(); if ($c -eq "") { continue }
        Add-Content -Path $chan -Value $c -Encoding Ascii
        Start-Sleep -Milliseconds 400
    }
}
Add-Content -Path $chan -Value "ui_input_mute 900" -Encoding Ascii
Start-Sleep -Milliseconds 700
Add-Content -Path $chan -Value "load $Level" -Encoding Ascii

$done = $false
for ($i = 0; $i -lt 240; $i++) {
    Start-Sleep -Seconds 1
    if (Select-String -Path $fresh.FullName -Pattern "st_client_synchronising" -Quiet -ErrorAction SilentlyContinue) { $done = $true; break }
    if ($p.HasExited) { break }
}
Write-Output "loaded=$done"

$elapsed = 0
while ($elapsed -lt $Play -and -not $p.HasExited) { Start-Sleep -Seconds 5; $elapsed += 5 }
Start-Sleep -Seconds 3
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2

Write-Output "---- the sweep, in the order it happened ----"
Select-String -Path $fresh.FullName -Pattern "\[VK AutoShot\] cmd:" | ForEach-Object { $_.Line }
Write-Output "---- [VK DDGI] (grid config + the ladder-vs-world verdict) ----"
Select-String -Path $fresh.FullName -Pattern "\[VK DDGI\]" | ForEach-Object { $_.Line }
Write-Output "---- [VK RTAO] BLAS rebuilds: what a bigger bubble costs ----"
Select-String -Path $fresh.FullName -Pattern "\[VK RTAO\] BLAS rebuilt|\[VK RTWorld\] cut:" | ForEach-Object { $_.Line }
Write-Output "---- [VK Perf] frame cost, last 12 ----"
Select-String -Path $fresh.FullName -Pattern "\[VK Perf\]" | Select-Object -Last 12 | ForEach-Object { $_.Line }
Write-Output "---- profiler: DDGI / RT zones ----"
Select-String -Path $fresh.FullName -Pattern "VK Perf" | Where-Object { $_.Line -match "DDGI|BuildAS|RTScene" } | Select-Object -Last 8 | ForEach-Object { $_.Line }
Write-Output "---- validation ----"
Select-String -Path $fresh.FullName -Pattern "VUID|SYNC-HAZARD" | Select-Object -First 15 | ForEach-Object { $_.Line }
Write-Output "---- errors ----"
Select-String -Path $fresh.FullName -Pattern "^!\[VK|FATAL" | Select-Object -First 15 | ForEach-Object { $_.Line }
Write-Output "---- end ----"
Write-Output $fresh.FullName
