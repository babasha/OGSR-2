# RT indirect light, step 0: load a level and photograph the RT preview.
#
# One run, four shots, because comparing two runs is invalid here - weather and
# actor position drift between them (the same rule that governs every A/B in this
# renderer). XROS_AUTO_SHOT_CMDS runs one console command per shot, so the raster
# reference and the three traced views all come from the SAME camera.
#
# ASCII only (the .ps1 encoding trap).
#
# -Validate 1 launches with -vk_validation: mandatory for RT work, it is what
# caught both acceleration-structure hazards when RTAO was written.
param([string]$Tag = "RTGI", [string]$Level = "pripyat_full", [int]$Play = 60,
      [int]$Validate = 1, [string]$Shots = "300,420,540,660",
      [string]$Cmds = "r_rtgi_debug 0|r_rtgi_debug 1|r_rtgi_debug 2|r_rtgi_debug 3",
      [string]$Pre = "", [string]$OutDir = "")

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

# NOT $args: that name is PowerShell's automatic argument array for the script.
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

# The shot seam lifts the start autopause itself after ~300 level frames, so the
# only thing to do here is wait long enough for the last requested frame.
$elapsed = 0
while ($elapsed -lt $Play -and -not $p.HasExited) { Start-Sleep -Seconds 5; $elapsed += 5 }
Start-Sleep -Seconds 3
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2

Write-Output "---- shots ----"
Get-ChildItem "$OutDir\*.tga" -ErrorAction SilentlyContinue | ForEach-Object { Write-Output $_.FullName }
Write-Output "---- [VK RT] / [VK RTGI] / [VK RTAO] ----"
Select-String -Path $fresh.FullName -Pattern "\[VK RT" | ForEach-Object { $_.Line }
Write-Output "---- validation ----"
Select-String -Path $fresh.FullName -Pattern "VUID|SYNC-HAZARD" | Select-Object -First 25 | ForEach-Object { $_.Line }
Write-Output "---- end ----"
Write-Output $fresh.FullName
