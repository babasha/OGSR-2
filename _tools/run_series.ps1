# Repeat the two-load repro N times and summarise. The crash it hunts is
# probabilistic (about 2 runs in 5 before the fix), so a single clean run
# proves nothing -- the series is the instrument. ASCII only.
param([int]$N = 5, [string]$Tag = "s", [int]$MiGuard = -1, [int]$Loads = 2, [switch]$MiVerbose)

$caught = 0
for ($i = 1; $i -le $N; $i++) {
    Write-Output "===== run $i / $N ====="
    $call = @{ Tag = "$Tag$i"; MiGuard = $MiGuard; Loads = $Loads }
    if ($MiVerbose) { $call.MiVerbose = $true }
    $out = & "$PSScriptRoot\run_twice.ps1" @call
    $out | Where-Object { $_ -notmatch "option " } | ForEach-Object { Write-Output $_ }
    if (($out -match "--- CAUGHT ---") -or ($out -match "STALLED") -or ($out -match "DIED")) { $caught++ }
}
Write-Output "===== series done: $caught of $N runs caught something ====="
