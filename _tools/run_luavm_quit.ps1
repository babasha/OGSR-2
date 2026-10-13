# Verify the shutdown summary: the harness normally KILLS the client, so
# ~CScriptEngine never runs and the automatic dump never appears. This one
# leaves through the console `quit` instead. ASCII only.
$game = "D:\OGSR Gunslinger Addon"
$logs = "$game\_appdata_\logs"
$chan = "D:\OGSR-ONLINE\_tools\cl_cmds_QUIT.txt"

if (Test-Path $chan) { Remove-Item -LiteralPath $chan -Force }
New-Item -ItemType File -Path $chan | Out-Null

while (Get-Process xrEngine_VK -ErrorAction SilentlyContinue) {
    Stop-Process -Name xrEngine_VK -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2
}
Start-Sleep -Seconds 2

$before = @(Get-ChildItem "$logs\xray_egorb_*.log" | Select-Object -ExpandProperty Name)
$env:XROS_CLIENT_CONSOLE = $chan
$p = Start-Process -FilePath "$game\bin_x64\xrEngine_VK.exe" -WorkingDirectory "$game\bin_x64" -PassThru

$fresh = $null
for ($i = 0; $i -lt 60; $i++) {
    Start-Sleep -Seconds 1
    $now = Get-ChildItem "$logs\xray_egorb_*.log" -ErrorAction SilentlyContinue
    $fresh = $now | Where-Object { $before -notcontains $_.Name } | Sort-Object LastWriteTime | Select-Object -Last 1
    if ($fresh) { break }
}
if (-not $fresh) { Write-Output "NO FRESH LOG"; exit 1 }

for ($i = 0; $i -lt 60; $i++) {
    Start-Sleep -Seconds 1
    if (Select-String -Path $fresh.FullName -Pattern "\[cl_console\] watching" -Quiet -ErrorAction SilentlyContinue) { break }
}
Start-Sleep -Seconds 5
Add-Content -Path $chan -Value "quit" -Encoding Ascii

for ($i = 0; $i -lt 40; $i++) { Start-Sleep -Seconds 1; if ($p.HasExited) { break } }
Write-Output ("exited=" + $p.HasExited)
Write-Output "---- tail of [lua vm] ----"
Select-String -Path $fresh.FullName -Pattern "\[lua vm\]" | Select-Object -Last 5 | ForEach-Object { $_.Line }
Write-Output $fresh.FullName
