# Step 6 - the GI field in froxel form (r_gi_froxel): load a level, photograph the
# A/B, and read the field's own audit line back out of the log.
#
# ONE run, four shots, because comparing two runs is invalid here - weather and
# actor position drift between them. XROS_AUTO_SHOT_CMDS runs one console command
# one frame before each capture, so every shot comes from the SAME camera.
#
# The shots are spaced 120 level-frames apart on purpose: the fog carries a
# temporal EMA (r_vol_ta), so a cvar flipped one frame before the capture would be
# photographed mid-converge.
#
# ASCII only (the .ps1 encoding trap).
param([string]$Tag = "GIFROX", [string]$Level = "pripyat_full", [int]$Play = 90,
      [int]$Validate = 1, [string]$Shots = "300,420,540,660",
      [string]$Cmds = "r_gi_froxel 0|r_gi_froxel 1|r_gi_froxel_dir 0|r_gi_froxel_dir 1",
      [string]$Pre = "r_profiler 2;r_gi_froxel_stats 1", [string]$OutDir = "")

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
Write-Output "---- [VK GiFroxel] ----"
Select-String -Path $fresh.FullName -Pattern "\[VK GiFroxel\]" | ForEach-Object { $_.Line }
Write-Output "---- [VK DDGI] / [VK Vol] init ----"
Select-String -Path $fresh.FullName -Pattern "\[VK DDGI\] \d+ cascades|\[VK Vol\] init OK|AutoShot" | ForEach-Object { $_.Line }
Write-Output "---- profiler: the pass cost ----"
Select-String -Path $fresh.FullName -Pattern "GiFroxel|VolInject" | Select-Object -Last 12 | ForEach-Object { $_.Line }
Write-Output "---- validation ----"
Select-String -Path $fresh.FullName -Pattern "VUID|SYNC-HAZARD" | Select-Object -First 25 | ForEach-Object { $_.Line }
Write-Output "---- end ----"
Write-Output $fresh.FullName
