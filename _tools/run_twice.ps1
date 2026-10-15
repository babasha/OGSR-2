# Load-time run: launch the client, drive it to a level through the console
# channel, wait for the load to finish, kill it. ASCII only.
param(
    [string]$Tag = "twice",
    [string]$Level = "pripyat_full",
    # mimalloc guarded pages: -1 leaves the build default (1 in 4000), 0 turns
    # them off outright, N samples 1 in N. The Release lib is built MI_GUARDED=1,
    # so this is an env switch and not a rebuild.
    [int]$MiGuard = -1,
    # How many level loads in ONE process. 2 = the heap-corruption repro; 3 was
    # where the 13-08 session saw a hang and worked around it with "one load per
    # run" -- worth re-testing whenever something in the unload path changes.
    [int]$Loads = 2,
    [switch]$MiVerbose)

$game = "D:\OGSR Gunslinger Addon"
$logs = "$game\_appdata_\logs"
$scratch = $PSScriptRoot
$chan = "$scratch\cl_cmds_$Tag.txt"
Remove-Item $chan -ErrorAction SilentlyContinue
New-Item -ItemType File $chan | Out-Null

# One instance only: a launch on top of a still-closing client exits SILENTLY and
# writes no log at all, which reads as "the run produced nothing".
while (Get-Process xrEngine_VK -ErrorAction SilentlyContinue) {
    Stop-Process -Name xrEngine_VK -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2
}
Start-Sleep -Seconds 2

$before = @(Get-ChildItem "$logs\xray_$($env:USERNAME)_*.log" | Select-Object -ExpandProperty Name)
$env:XROS_CLIENT_CONSOLE = $chan

# mimalloc talks on stderr (WriteFile to STD_ERROR_HANDLE), which a windowed app
# drops on the floor unless the launch redirects it.
$errFile = "$scratch\mi_stderr_$Tag.txt"
Remove-Item $errFile -ErrorAction SilentlyContinue
if ($MiGuard -ge 0) { $env:MIMALLOC_GUARDED_SAMPLE_RATE = "$MiGuard" } else { Remove-Item Env:\MIMALLOC_GUARDED_SAMPLE_RATE -ErrorAction SilentlyContinue }
if ($MiVerbose) { $env:MIMALLOC_VERBOSE = "1"; $env:MIMALLOC_SHOW_ERRORS = "1" } else { Remove-Item Env:\MIMALLOC_VERBOSE -ErrorAction SilentlyContinue; Remove-Item Env:\MIMALLOC_SHOW_ERRORS -ErrorAction SilentlyContinue }
Write-Output "mi_guard=$MiGuard verbose=$($MiVerbose.IsPresent)"
$p = Start-Process -FilePath "$game\bin_x64\xrEngine_VK.exe" -WorkingDirectory "$game\bin_x64" -PassThru -RedirectStandardError $errFile

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
Write-Output "log: $($fresh.Name)"

# Wait for the channel to be picked up, then mute UI input (a stray click on the
# menu is what a focus change costs) and start the level.
for ($i = 0; $i -lt 60; $i++) {
    Start-Sleep -Seconds 1
    if (Select-String -Path $fresh.FullName -Pattern "\[cl_console\] watching" -Quiet -ErrorAction SilentlyContinue) { break }
}
Start-Sleep -Seconds 3
Add-Content -Path $chan -Value "ui_input_mute 900" -Encoding Ascii
Start-Sleep -Milliseconds 700
Add-Content -Path $chan -Value "load $Level" -Encoding Ascii

$done = $false
for ($i = 0; $i -lt 180; $i++) {
    Start-Sleep -Seconds 1
    if (Select-String -Path $fresh.FullName -Pattern "End of synchronization" -Quiet -ErrorAction SilentlyContinue) { $done = $true; break }
    if ($p.HasExited) { break }
}
Start-Sleep -Seconds 5

# Reloads in the SAME process. This is where a level_Unload defect shows: the
# damage is done on unload, the fault lands in whatever the next load allocates.
# A load that never finishes is reported as STALLED -- a hang and a crash need
# different next steps, so they must not read the same in the output.
$reached = 1
for ($k = 2; $k -le $Loads; $k++) {
    if ($p.HasExited) { break }
    Write-Output "--- load $k ---"
    Start-Sleep -Seconds 5
    Add-Content -Path $chan -Value "r_cluster_cache 0" -Encoding Ascii
    Start-Sleep -Milliseconds 700
    Add-Content -Path $chan -Value "disconnect" -Encoding Ascii
    Start-Sleep -Seconds 8
    Add-Content -Path $chan -Value "load $Level" -Encoding Ascii
    $ok = $false
    for ($i = 0; $i -lt 180; $i++) {
        Start-Sleep -Seconds 1
        $n = (Select-String -Path $fresh.FullName -Pattern "End of synchronization" -AllMatches).Count
        if ($n -ge $k) { $ok = $true; break }
        if ($p.HasExited) { break }
    }
    if ($ok) { $reached = $k; Write-Output "load $k : ok" }
    else { Write-Output "load $k : $(if ($p.HasExited) { 'DIED' } else { 'STALLED (180 s, process alive)' })"; break }
    Start-Sleep -Seconds 5
}
Write-Output "loads completed=$reached of $Loads  exited=$($p.HasExited)"
Start-Sleep -Seconds 3
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 3

# Verdict, so a run does not need a human to read a 3 MB log to answer "did it
# crash": the exception line, the fault region, and the visual guard.
$hits = Select-String -Path $fresh.FullName -Pattern "ExceptionCode: \[c0000005\]|!!\[fault\]|VisualGuard\]|threw \[non-std"
if ($hits) { Write-Output "--- CAUGHT ---"; $hits | ForEach-Object { Write-Output $_.Line.Trim() } }
else { Write-Output "--- clean run ---" }
if (Test-Path $errFile) {
    $mi = Get-Content $errFile -ErrorAction SilentlyContinue
    if ($mi) { Write-Output "--- mimalloc stderr ---"; $mi | Select-Object -First 40 }
}
Write-Output $fresh.FullName
