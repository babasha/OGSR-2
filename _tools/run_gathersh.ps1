# Step 4b - the final gather as a 2-band SH per pixel (r_gather_sh): load a level,
# photograph the A/B, and read the pass's own config line back out of the log.
#
# ONE run, eight shots, because comparing two runs is invalid here - weather and
# actor position drift between them. XROS_AUTO_SHOT_CMDS runs one console command
# one frame before each capture, so every shot comes from the SAME camera.
#
# The FIRST command freezes the clock: the actor RUNS during an auto-run, and two
# shots 120 frames apart are otherwise two different places, not two settings.
#
# The claim under test is falsifiable and narrow: the SH's deviation is built
# perpendicular to the gather's own normal, so it must REDISTRIBUTE light across
# normal-mapped detail without moving the mean. Shots 2/3 are the full frame,
# 4/5/6 the ambient term alone (where the memory measured an 8x larger signal),
# and 7/8 the two SH debug views - the binary "is this term alive at all" test.
#
# ASCII only (the .ps1 encoding trap).
param([string]$Tag = "GSH", [string]$Level = "pripyat_full", [int]$Play = 180,
      [int]$Validate = 1, [string]$Shots = "240,360,480,600,720,840,960,1080",
      [string]$Cmds = "time_factor 0.001|r_gather_sh 0|r_gather_sh 1|r_shade_debug 11|r_gather_sh 0|r_gather_sh 1|r_gather_debug 5|r_gather_debug 4",
      [string]$Pre = "r_profiler 2;r_gi 2;r_gather_sh 1", [string]$OutDir = "")

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

$before = @(Get-ChildItem "$logs\xray_egorb_*.log" | Select-Object -ExpandProperty Name)
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
    $now = Get-ChildItem "$logs\xray_egorb_*.log" -ErrorAction SilentlyContinue
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

Write-Output "---- shots ----"
Get-ChildItem "$OutDir\*.tga" -ErrorAction SilentlyContinue | ForEach-Object { Write-Output $_.FullName }
Write-Output "---- [VK Gather] (targets + config: does the line say SH 2-band?) ----"
Select-String -Path $fresh.FullName -Pattern "\[VK Gather\]" | ForEach-Object { $_.Line }
Write-Output "---- AutoShot ----"
Select-String -Path $fresh.FullName -Pattern "AutoShot" | ForEach-Object { $_.Line }
Write-Output "---- profiler: the pass cost (Gather zones must EXIST) ----"
Select-String -Path $fresh.FullName -Pattern "Gather" | Where-Object { $_.Line -match "VK Perf" } | Select-Object -Last 8 | ForEach-Object { $_.Line }
Write-Output "---- validation ----"
Select-String -Path $fresh.FullName -Pattern "VUID|SYNC-HAZARD" | Select-Object -First 25 | ForEach-Object { $_.Line }
Write-Output "---- errors ----"
Select-String -Path $fresh.FullName -Pattern "^!\[VK|FATAL" | Select-Object -First 15 | ForEach-Object { $_.Line }
Write-Output "---- end ----"
Write-Output $fresh.FullName
