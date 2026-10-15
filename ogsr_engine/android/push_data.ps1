# Installs the APK and pushes the game data to the phone over USB (Windows adb: WSL does not see USB devices).
#   powershell -File ogsr_engine\android\push_data.ps1 [-ApkOnly] [-DataOnly]
# Data: D:\android_stage\files (stage_data.sh) -> /sdcard/Android/data/com.ogsr.online/files.
# adb push skips nothing by itself, so the big archives go only when their size differs from the phone's copy.
param([switch]$ApkOnly, [switch]$DataOnly)
$ErrorActionPreference = 'Stop'
$adb = 'D:\android_stage\platform-tools\adb.exe'
$pkg = 'com.ogsr.online'
$dst = "/sdcard/Android/data/$pkg/files"
$src = 'D:\android_stage\files'

if (-not $DataOnly) {
    & $adb install -r 'D:\android_stage\ogsr.apk'
    if ($LASTEXITCODE -ne 0) { throw 'adb install failed' }
}
if ($ApkOnly) { return }

& $adb shell mkdir -p $dst
# Size listing of what is already there: "<size> <path>" per file.
$remote = @{}
& $adb shell "cd $dst && find . -type f -exec stat -c '%s %n' {} +" 2>$null | ForEach-Object {
    $i = $_.IndexOf(' ')
    if ($i -gt 0) { $remote[$_.Substring($i + 1).TrimStart('.', '/')] = [int64]$_.Substring(0, $i) }
}
# Top-level files one by one (the archives are big: skip them when unchanged), loose folders whole.
Get-ChildItem $src -File | ForEach-Object {
    if ($remote.ContainsKey($_.Name) -and $remote[$_.Name] -eq $_.Length) { Write-Host "same  $($_.Name)"; return }
    Write-Host "push  $($_.Name) ($([math]::Round($_.Length / 1MB)) MB)"
    & $adb push $_.FullName "$dst/$($_.Name)" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "adb push $($_.Name) failed" }
}
# adb prints its "N files pushed" summary to stderr: under 'Stop' Windows PowerShell turns that into an error.
$ErrorActionPreference = 'Continue'
Get-ChildItem $src -Directory | ForEach-Object {
    Write-Host "push  $($_.Name)\"
    & $adb push --sync $_.FullName "$dst/" | Select-Object -Last 1
    if ($LASTEXITCODE -ne 0) { throw "adb push $($_.Name) failed" }
}
# Folders adb creates are shell:ext_data_rw 2770 -- the app's uid cannot enter them, so every loose file
# (gamedata\shaders\*.spv, config\rspec_vk_switch.ltx) failed to open while the top-level archives worked.
& $adb shell "find $dst -type d -user shell -exec chmod 2777 {} +"
