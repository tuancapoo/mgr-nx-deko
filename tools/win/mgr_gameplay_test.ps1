# Interactive native-renderer session, with a private copy of the player's save.
param([int]$FpsLimit = 0, [switch]$DryRun, [string]$ResumeSession = '')
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$exe = (Resolve-Path (Join-Path $root 'out\pc\sa2.exe')).Path
$dir = Split-Path $exe
foreach ($file in 'sa2_shaders.mesp', 'sa2_shaders.mesp.idx') {
    if (-not (Test-Path -LiteralPath (Join-Path $dir $file))) { throw "Missing $file next to $exe" }
}
$session = Join-Path $dir ('benchmarks\gameplay_' + (Get-Date).ToString('yyyyMMdd_HHmmss'))
$profile = Join-Path $session 'userdata'
New-Item -ItemType Directory -Path $profile -Force | Out-Null
$saveSource = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'mgr'
if ($ResumeSession) {
    $saveSource = Join-Path (Resolve-Path -LiteralPath $ResumeSession).Path 'userdata'
    if (-not (Test-Path -LiteralPath $saveSource)) { throw "No private save in $ResumeSession" }
}
if (Test-Path -LiteralPath $saveSource) {
    Get-ChildItem -LiteralPath $saveSource -Directory |
        Where-Object Name -Match '^[A-Fa-f0-9]{16}$' |
        ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $profile -Recurse }
}
@((Get-ChildItem -LiteralPath $profile -File -Recurse | ForEach-Object {
    [ordered]@{ path = $_.FullName.Substring($profile.Length + 1); bytes = $_.Length;
        sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash }
})) | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath (Join-Path $session 'save_before.json') -Encoding UTF8
$arguments = @(
    '--fullscreen=false', '--window_start_minimized=false', '--audio_mute=false',
    '--input_backend=sdl', '--mnk_mode=true', '--sa2_renderer_native=true',
    "--mgr_fps_limit=$FpsLimit", '--mgr_dump_shaders=true',
    ('--user_data_root="' + $profile + '"'),
    ('--mgr_shader_dump_dir="' + (Join-Path $session 'shaders') + '"'),
    ('--sa2_shader_misses="' + (Join-Path $session 'shader_misses') + '"'),
    ('--mgr_native_perf_csv="' + (Join-Path $session 'native.csv') + '"'),
    ('--perf_log_csv="' + (Join-Path $session 'sdk.csv') + '"')
)
@'
MGR gameplay validation

- Use the controller to load your checkpoint and enter a fight.
- F10 toggles the game's normal limit and 30 FPS; guest time remains real time.
- Compare movement, attacks, camera, Blade Mode, audio and cutscene speed in both modes.
- Trigger a checkpoint save, close the game normally, and retain this session's userdata folder for a reload.
- The original save was copied into this session and is not used for writes.
- native.csv: per-swap wall time and draws; process CPU includes all host threads.
- GPU columns are cumulative completed query times, not exact per-frame durations (queries complete later).
- Ring active time includes GPU waits. Its difference from CPU time is intentional.
'@ | Set-Content -LiteralPath (Join-Path $session 'README.txt') -Encoding UTF8
$launch = [ordered]@{ exe = $exe; args = $arguments; session = $session; userdata = $profile;
    started = (Get-Date).ToString('o'); dry_run = $DryRun.IsPresent }
if ($DryRun) {
    $launch | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $session 'launch.json') -Encoding UTF8
    Write-Output "Prepared only: $session"
    exit 0
}
$running = @(Get-Process -Name sa2 -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe })
if ($running.Count) { throw "MGR is already running (PID $($running.Id -join ', ')); close it before starting a benchmark." }
# This is the interactive test the user needs to see and control.
$process = Start-Process -FilePath $exe -ArgumentList $arguments -WorkingDirectory $dir -WindowStyle Normal -PassThru
$launch.pid = $process.Id
$launch | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $session 'launch.json') -Encoding UTF8
Write-Output "pid=$($process.Id)"
Write-Output "session=$session"
Write-Output 'F10: normal / 30 FPS. Close normally after a checkpoint save.'
