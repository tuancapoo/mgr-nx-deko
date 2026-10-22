# Reproducible, muted MGR render probe. Captures the presenter's guest output even when minimized.
param(
    [switch]$Native,
    [switch]$DumpShaders,
    [switch]$XmaProfile,
    [string]$ShaderMisses = '',
    [int]$FpsLimit = 0,
    [int]$Seconds = 48,
    [int]$CaptureAt = 32,
    [int]$CaptureFrames = 60,
    [int]$TraceAt = 40,
    [string[]]$ExtraArgs = @()
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$exe = (Resolve-Path (Join-Path $root 'out\pc\sa2.exe')).Path
$dir = Split-Path $exe
$started = Get-Date
$tag = if ($Native) { 'native' } else { 'reference' }
$output = Join-Path $dir ('captures\' + $tag + '_' + $started.ToString('yyyyMMdd_HHmmss'))
New-Item -ItemType Directory -Path $output -Force | Out-Null
$profile = Join-Path $output 'userdata'
New-Item -ItemType Directory -Path $profile -Force | Out-Null
$saveSource = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'mgr'
if (Test-Path -LiteralPath $saveSource) {
    Get-ChildItem -LiteralPath $saveSource -Directory | Where-Object Name -Match '^[A-Fa-f0-9]{16}$' |
        ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $profile -Recurse }
}
$arguments = @(
    '--fullscreen=false', '--audio_mute=true', '--window_start_minimized=true',
    ('--sa2_renderer_native=' + $Native.IsPresent.ToString().ToLowerInvariant()),
    "--sa2_video_s=$CaptureAt", "--sa2_video_frames=$CaptureFrames",
    '--sa2_video_half=false', '--sa2_video_every=6', '--sa2_video_lockstep=false',
    "--mgr_fps_limit=$FpsLimit",
    ('--user_data_root="' + $profile + '"'),
    ('--mgr_native_perf_csv="' + (Join-Path $output 'native.csv') + '"'),
    ('--perf_log_csv="' + (Join-Path $output 'sdk.csv') + '"'),
    "--banjo_trace_frame_s=$TraceAt"
)
if ($DumpShaders) {
    $arguments += '--mgr_dump_shaders=true', '--mgr_shader_dump_dir=shaders_corrected'
}
if ($ShaderMisses) { $arguments += "--sa2_shader_misses=$ShaderMisses" }
if ($XmaProfile) { $arguments += '--audio_xma_profile=true' }
$arguments += $ExtraArgs
$previousTrace = $env:SA2_TRACE_ALL_DRAWS
$env:SA2_TRACE_ALL_DRAWS = '1'
$process = $null
try {
    $process = Start-Process -FilePath $exe -ArgumentList $arguments -WorkingDirectory $dir -WindowStyle Hidden -PassThru
    Write-Output "probe=$tag pid=$($process.Id) output=$output"
    $exited = $process.WaitForExit($Seconds * 1000)
    if ($exited) {
        Write-Output ('exited=0x{0:X8}' -f $process.ExitCode)
        if ($process.ExitCode -ne 0) { throw "MGR exited before the probe completed (code $($process.ExitCode))" }
    } else {
        # Stop only the process this probe created.
        Stop-Process -Id $process.Id -Force
        $process.WaitForExit(20000) | Out-Null
        Write-Output "completed=${Seconds}s"
    }
} finally {
    if ($process -and -not $process.HasExited) { Stop-Process -Id $process.Id -Force }
    $env:SA2_TRACE_ALL_DRAWS = $previousTrace
}
$log = Get-ChildItem (Join-Path $dir 'logs') -Filter '*.log' |
    Where-Object { $_.LastWriteTime -ge $started.AddSeconds(-1) } |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($log) {
    Copy-Item -LiteralPath $log.FullName -Destination (Join-Path $output 'runtime.log')
    Select-String -LiteralPath $log.FullName -Pattern '\[capture\]|10.0 s:|library loaded|fatal|\[error\]' |
        Select-Object -Last 12 | ForEach-Object { $_.Line }
}
Get-ChildItem (Join-Path $dir 'captures') -Filter "video_${CaptureAt}s.*" |
    Where-Object { $_.LastWriteTime -ge $started.AddSeconds(-1) } |
    Copy-Item -Destination $output
$metadata = Join-Path $output "video_${CaptureAt}s.txt"
$video = Join-Path $output "video_${CaptureAt}s.rgba"
if (-not (Test-Path -LiteralPath $metadata) -or -not (Test-Path -LiteralPath $video)) {
    throw "Probe did not finish writing its capture; see $output\runtime.log"
}
$fields = (Get-Content -LiteralPath $metadata -Raw).Trim() -split '\s+'
if ($fields.Count -ne 4 -or [int]$fields[3] -ne $CaptureFrames -or
    (Get-Item -LiteralPath $video).Length -ne ([long]$fields[0] * [long]$fields[1] * 4 * [long]$fields[3])) {
    throw "Probe capture is incomplete; see $metadata"
}
Write-Output "result=$output"
