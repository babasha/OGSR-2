# DDGI PROBE RELOCATION (r_ddgi_reloc) - first run: is the pass alive, are its own
# numbers sane, and is the validation clean.
#
# What this run can and cannot decide:
#   * CAN: does DDGI/Reloc appear as a profiler zone (an absent zone means the
#     dispatch never happened - the trap that cost a session on the final gather);
#     does the self-audit line print; is the buried share plausible OUTDOORS
#     (a spawn in a forest must NOT report most of its probes buried - if it does
#     the test is wrong, not the world); is the backface rate a real signal
#     (near 50% would mean the winding carries nothing and reloc_back must go to 0);
#     any VUID / SYNC-HAZARD from the new strip writes and the new barrier.
#   * CANNOT: whether interiors look better. That is a room, and this is a forest.
#     The user's recipe for that is a room + r_gi 2 + r_ddgi_reloc 0 <-> 1.
#
# ⚠ Toggling r_ddgi_reloc WIPES the field on purpose (the atlas and the offsets are
# one measurement), so the shots are spaced to let every cascade re-measure: the
# toggle lands at 360 and the next comparable shot is 300+ frames later.
#
# ASCII only (the .ps1 encoding trap).
param([string]$Tag = "RELOC", [string]$Level = "pripyat_full", [int]$Play = 200,
      [int]$Validate = 1, [string]$Shots = "300,360,720,1080,1140,1500",
      [string]$Cmds = "time_factor 0.001|r_ddgi_reloc 1|r_ddgi_reloc 1|r_ddgi_reloc 1|r_ddgi_reloc 0|r_ddgi_reloc 0",
      [string]$Pre = "r_profiler 2;r_gi 1", [string]$OutDir = "")

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
Write-Output "---- [VK DDGI] (grid config + the offset strip + relocation audit) ----"
Select-String -Path $fresh.FullName -Pattern "\[VK DDGI\]" | ForEach-Object { $_.Line }
Write-Output "---- profiler: DDGI zones (Reloc must EXIST once it is on) ----"
Select-String -Path $fresh.FullName -Pattern "VK Perf" | Where-Object { $_.Line -match "DDGI" } | Select-Object -Last 6 | ForEach-Object { $_.Line }
Write-Output "---- validation ----"
Select-String -Path $fresh.FullName -Pattern "VUID|SYNC-HAZARD" | Select-Object -First 25 | ForEach-Object { $_.Line }
Write-Output "---- errors ----"
Select-String -Path $fresh.FullName -Pattern "^!\[VK|FATAL" | Select-Object -First 15 | ForEach-Object { $_.Line }
Write-Output "---- end ----"
Write-Output $fresh.FullName
