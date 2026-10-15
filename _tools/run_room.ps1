# Unattended interior-GI / exposure run: launch the client, load an ASCII-named
# save parked inside a room, let XROS_AUTO_SHOT dismiss the start autopause,
# then walk a scripted A/B with instruments on and vk_shot captures per phase.
# ASCII only (Windows PowerShell 5.1 parses .ps1 as ANSI).
#
# -Save   save name (ASCII, no extension) in _appdata_\savedgames
# -Post   semicolon-separated console commands; "wait N" is a harness pause;
#         "shot NAME" writes <ShotDir>\NAME.tga via vk_shot
param([string]$Tag = "ROOM", [string]$Save = "room_test", [string]$Post = "", [string]$ShotDir = "", [int]$Settle = 30)

$game = "D:\OGSR Gunslinger Addon"
$logs = "$game\_appdata_\logs"
$scratch = $PSScriptRoot
$chan = "$scratch\cl_cmds_$Tag.txt"
if ($ShotDir -eq "") { $ShotDir = "$scratch\shots_$Tag" }
New-Item -ItemType Directory -Force $ShotDir | Out-Null
Remove-Item $chan -ErrorAction SilentlyContinue
New-Item -ItemType File $chan | Out-Null

if (Get-Process xrEngine_VK -ErrorAction SilentlyContinue) {
    Write-Output "CLIENT ALREADY RUNNING - refusing to kill the user's session"
    exit 2
}

$before = @(Get-ChildItem "$logs\xray_$($env:USERNAME)_*.log" | Select-Object -ExpandProperty Name)
$env:XROS_CLIENT_CONSOLE = $chan
$env:XROS_AUTO_SHOT = "$ShotDir\auto_"
$env:XROS_AUTO_SHOT_AT = "100000"          # never fires; the variable's presence is what dismisses the autopause
$env:VK_LOADER_LAYERS_DISABLE = "VK_LAYER_reshade"
$p = Start-Process -FilePath "$game\bin_x64\xrEngine_VK.exe" -WorkingDirectory "$game\bin_x64" -PassThru

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
Add-Content -Path $chan -Value "ui_input_mute 900" -Encoding Ascii
Start-Sleep -Milliseconds 700
Add-Content -Path $chan -Value "load $Save" -Encoding Ascii

$done = $false
for ($i = 0; $i -lt 240; $i++) {
    Start-Sleep -Seconds 1
    if (Select-String -Path $fresh.FullName -Pattern "st_client_synchronising" -Quiet -ErrorAction SilentlyContinue) { $done = $true; break }
    if ($p.HasExited) { break }
}
Write-Output "loaded=$done"
if (-not $done) {
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    Write-Output $fresh.FullName
    exit 1
}

# The autopause is dismissed ~300 frames after the level is up; give the scene a
# settle window after that (GI bounce loop, exposure adaptation).
for ($i = 0; $i -lt 120; $i++) {
    Start-Sleep -Seconds 1
    if (Select-String -Path $fresh.FullName -Pattern "dismissing the start autopause" -Quiet -ErrorAction SilentlyContinue) { break }
}
Write-Output "autopause dismissed; settling $Settle s"
Start-Sleep -Seconds $Settle

if ($Post -ne "") {
    foreach ($c in $Post.Split(";")) {
        $c = $c.Trim()
        if ($c -eq "") { continue }
        if ($c.StartsWith("wait ")) {
            $secs = [int]$c.Substring(5).Trim()
            Write-Output "wait: $secs s"
            Start-Sleep -Seconds $secs
            continue
        }
        if ($c.StartsWith("shot ")) {
            $name = $c.Substring(5).Trim()
            $path = "$ShotDir\$name.tga"
            Write-Output "shot: $path"
            Add-Content -Path $chan -Value "vk_shot $path" -Encoding Ascii
            Start-Sleep -Seconds 2
            continue
        }
        Write-Output "post: $c"
        Add-Content -Path $chan -Value $c -Encoding Ascii
        Start-Sleep -Seconds 2
    }
}

Start-Sleep -Seconds 3
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2
Write-Output "shots:"
Get-ChildItem $ShotDir -ErrorAction SilentlyContinue | ForEach-Object { Write-Output ("  " + $_.Name + " " + $_.Length) }
Write-Output $fresh.FullName
