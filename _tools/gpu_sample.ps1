# GPU HARDWARE TELEMETRY ALONGSIDE A RUN - clocks, temperature, power, throttle reason.
#
# Why this exists: on 25-08 World/Cull spent one evening reading 6.9 ms instead of its
# usual 0.23 on an unchanged level, and every software explanation tried (PCIe eviction,
# buffer priority, VRAM oversubscription to 114%) failed to reproduce it. What the
# software instruments CANNOT see is the machine underneath: this laptop's memory clock
# steps are 12001 / 11001 / 9001 / 810 / 405 MHz, so a drop of one step is an 11x loss of
# bandwidth - and a pass that only streams memory (the 63 MB cull scan) would collapse by
# roughly that factor while ALU- and fill-bound neighbours barely moved. That is exactly
# the shape of the observed anomaly, and it cannot be confirmed or dismissed without
# sampling the hardware.
#
# Usage:  .\gpu_sample.ps1 -Out clocks.csv          (Ctrl+C or -Seconds N to stop)
# Then join by timestamp against the [VK Perf] lines in the game log.
#
# ASCII only (the .ps1 encoding trap).
param([string]$Out = "$PSScriptRoot\gpu_clocks.csv", [int]$Seconds = 0, [int]$IntervalMs = 1000)

$smi = "C:\Windows\System32\nvidia-smi.exe"
if (-not (Test-Path $smi)) { Write-Output "nvidia-smi not found at $smi"; exit 1 }

# Canonical names. The short aliases (clocks.sm) work interactively but were rejected
# when the same string was passed through a background job - use the full ones.
$q = "timestamp,clocks.current.sm,clocks.current.memory,temperature.gpu,power.draw,utilization.gpu,memory.used,clocks_event_reasons.active"

"timestamp,sm_mhz,mem_mhz,temp_c,power_w,util_pct,vram_mib,throttle" | Out-File -FilePath $Out -Encoding Ascii
$deadline = if ($Seconds -gt 0) { (Get-Date).AddSeconds($Seconds) } else { [datetime]::MaxValue }
Write-Output "sampling -> $Out"
while ((Get-Date) -lt $deadline) {
    $line = & $smi --query-gpu=$q --format=csv,noheader 2>$null
    if ($line) { ($line -replace '\s*MHz','' -replace '\s*W','' -replace '\s*%','' -replace '\s*MiB','') | Add-Content -Path $Out -Encoding Ascii }
    Start-Sleep -Milliseconds $IntervalMs
}
