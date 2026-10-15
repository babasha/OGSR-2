# LuaJIT VM ownership watch: launch the client, load a level, play for a while,
# then dump the inventory of who has been inside the VM. ASCII only.
#
# The watch itself changes no behaviour - it only records which threads reach
# the LuaJIT state, through which call sites, and whether any two of them ever
# overlap. See ogsr_engine/xrCore/lua_vm_watch.h.
# -Extra runs one more console command right after the watch is armed. Its
# reason for existing is the positive control: `-Extra "lua_gc_async 1"` puts the
# collector back on its own worker, which MUST light the detector up. A detector
# that has never been seen to fire is not evidence of anything.
# -Post is the load: semicolon-separated console commands sent once the level is
# up (e.g. "sv_npc_spawn stalker 30 stalker;sv_npc_spawn stalker 30 bandit").
# WARNING: one STRING, split here -- a PowerShell array parameter passed through -File
# is torn apart by the argument parser, which has cost a session before.
param([string]$Tag = "LVM", [string]$Level = "pripyat_full", [int]$Play = 180, [int]$Watch = 2, [string]$Extra = "", [string]$Post = "", [string]$LaunchArgs = "")

$game = "D:\OGSR Gunslinger Addon"
$logs = "$game\_appdata_\logs"
$scratch = $PSScriptRoot
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
Add-Content -Path $chan -Value "lua_vm_watch $Watch" -Encoding Ascii
Start-Sleep -Milliseconds 400
if ($Extra -ne "") {
    Add-Content -Path $chan -Value $Extra -Encoding Ascii
    Start-Sleep -Milliseconds 400
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

if ($Post -ne "") {
    Start-Sleep -Seconds 8
    foreach ($c in $Post.Split(";")) {
        $c = $c.Trim()
        if ($c -eq "") { continue }
        # "wait N" is a harness pseudo-command, not a console one: an A/B that
        # toggles a cvar needs a PLATEAU on each side, and both plateaus have to
        # happen in ONE run -- comparing two runs is invalid here, weather and
        # actor position drift between them.
        if ($c.StartsWith("wait ")) {
            $secs = [int]$c.Substring(5).Trim()
            Write-Output "wait: $secs s"
            Start-Sleep -Seconds $secs
            continue
        }
        Write-Output "post: $c"
        Add-Content -Path $chan -Value $c -Encoding Ascii
        Start-Sleep -Seconds 2
    }
}

# Play. The dump every 30 s is what turns a silent run into a timeline: if the
# entry counts stop moving, the frame stopped, and that is worth knowing too.
$elapsed = 0
while ($elapsed -lt $Play -and -not $p.HasExited) {
    Start-Sleep -Seconds 30
    $elapsed += 30
    Add-Content -Path $chan -Value "lua_vm_watch_dump" -Encoding Ascii
}
Start-Sleep -Seconds 3
Add-Content -Path $chan -Value "lua_vm_watch_dump" -Encoding Ascii
Start-Sleep -Seconds 3
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2

Write-Output "---- [lua vm] lines ----"
Select-String -Path $fresh.FullName -Pattern "\[lua vm\]" | ForEach-Object { $_.Line }
Write-Output "---- end ----"
Write-Output $fresh.FullName


