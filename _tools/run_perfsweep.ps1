# GENERIC PERF SWEEP - one run, N console steps, per-step [VK Frame] verdict.
#
# Image A/B between runs is invalid here (weather + actor drift), so every
# comparison this script can settle must happen INSIDE one run. Each step gets
# its own 500 level-frames; the parser reads the LAST [VK Frame] window of each
# bucket, which is the steady state after the change.
#
# TIME IS FROZEN BY DEFAULT (time_factor 0.001) and that is the point: the wind
# turns, the weather script moves the sun and the fog, and the Trees zone alone
# swings 4.6 -> 13.6 ms between frames of one run - larger than most differences
# a sweep looks for. The profiler prints on Device.dwTimeContinual (real ms) since
# 24-08, so freezing the game clock no longer stops [VK Perf]/[VK Frame].
# EXE OLDER THAN 24-08 21:35 CANNOT DO THIS - it gates on the game clock and goes
# silent instead. If a sweep comes back with "(no window)" everywhere, that is why.
#
# ASCII only (the .ps1 encoding trap).
param([string]$Tag = "PS", [string]$Level = "pripyat_full", [int]$Play = 260,
      [string]$Shots = "600,1100,1600,2100,2600,3100",
      [string]$Cmds  = "r_wrc 1|r_wrc 0|r_gather 0|r_ddgi 0|r_gi off|r_ssao 0",
      [string]$Pre = "r_profiler 2", [string]$Freeze = "1", [string]$OutDir = "",
      [string]$LaunchArgs = "")

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

# -LaunchArgs "-vk_validation": correctness gate. Validation layers wreck fps, so such a
# run is for the ERROR LIST only - never read timings out of it.
if ($LaunchArgs -ne "") {
    $p = Start-Process -FilePath "$game\bin_x64\xrEngine_VK.exe" -WorkingDirectory "$game\bin_x64" -PassThru -ArgumentList $LaunchArgs
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

# Freeze the world AFTER the load (before it, the level would not finish). The
# actor keeps standing where the spawn put him and nothing in the scene animates,
# so every step of the sweep looks at the same frame.
if ($Freeze -ne "0") {
    Start-Sleep -Seconds 6
    Add-Content -Path $chan -Value "time_factor 0.001" -Encoding Ascii
    Start-Sleep -Seconds 2
}

# Stop early once the last capture point has fired - no reason to burn wall clock.
$lastShot = ($Shots.Split(",") | ForEach-Object { [int]$_.Trim() } | Measure-Object -Maximum).Maximum
$elapsed = 0
while ($elapsed -lt $Play -and -not $p.HasExited) {
    Start-Sleep -Seconds 5; $elapsed += 5
    if (Select-String -Path $fresh.FullName -Pattern "level-frame $lastShot " -Quiet -ErrorAction SilentlyContinue) {
        Start-Sleep -Seconds 12; break
    }
}
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2
Write-Output "PLAYED=$elapsed"
Write-Output $fresh.FullName
