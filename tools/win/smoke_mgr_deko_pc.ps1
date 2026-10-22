param([string]$Name = 'smoke', [int]$Seconds = 65, [string]$InputScript = '', [switch]$Reference,
      [int]$VideoAt = 35, [string[]]$ExtraArguments = @())
$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$report = Join-Path $projectRoot "out\validation\$Name"
$exeFolder = if ($Reference) { Join-Path $projectRoot 'out\validation\reference-runtime' } else { Join-Path $projectRoot 'out\pc' }
$exe = Join-Path $exeFolder 'sa2.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw 'PC executable has not been built' }
New-Item -ItemType Directory -Path $report,(Join-Path $report 'userdata') -Force | Out-Null
$state = [ordered]@{state='running';started_utc=[DateTime]::UtcNow.ToString('o');renderer='renut';backend='vulkan'}
$statusPath = Join-Path $report 'run-status.json'
$oldInput = $env:SA2_AUTO_INPUT
$startedUtc = [DateTime]::UtcNow
try {
    $env:SA2_AUTO_INPUT = $InputScript
    $arguments = @('--fullscreen=false','--window_start_minimized=true','--audio_mute=true',
        '--sa2_renderer=renut','--renut_gpu_backend=vulkan','--mnk_mode=true','--input_xbox_layout=true',
        '--mgr_shared_input=true','--sa2_render_height_handheld=360','--sa2_render_height_docked=360',
        '--banjo_renut_store_cards=false','--banjo_deko3d_alpha_spec=false',
        "--sa2_video_s=$VideoAt",'--sa2_video_frames=4','--sa2_video_every=60','--sa2_video_half=false',
        '--sa2_video_lockstep=false','--sa2_enhancements_report_s=0',
        "--log_file=`"$report\runtime.log`"",'--log_level=info','--log_flush_interval=1',
        "--user_data_root=`"$report\userdata`"", "--game_data_root=`"$projectRoot\assets\game_root`"")
    if ($Reference) {
        $arguments = @($arguments | Where-Object { $_ -notmatch '^--(sa2_renderer=|renut_|banjo_renut_|banjo_deko3d_)' })
        $arguments += @('--sa2_renderer_native=true','--mgr_startup_prewarm=false',
            '--sa2_native_pipelines_prewarm=false','--mgr_fps_limit=60')
        $state.renderer = 'native-reference'
    }
    $arguments += $ExtraArguments
    $arguments | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $report 'launch-arguments.json') -Encoding utf8
    $InputScript | Set-Content -LiteralPath (Join-Path $report 'input-script.txt')
    $process = Start-Process -FilePath $exe -ArgumentList $arguments -WorkingDirectory $exeFolder `
        -RedirectStandardOutput (Join-Path $report 'stdout.txt') -RedirectStandardError (Join-Path $report 'stderr.txt') `
        -WindowStyle Hidden -PassThru
    $state.pid = $process.Id
    $state | ConvertTo-Json | Set-Content -LiteralPath $statusPath -Encoding utf8
    $ended = $process.WaitForExit($Seconds * 1000)
    if ($ended) {
        $state.state = 'exited'
        $state.exit_code = $process.ExitCode
    } else {
        Stop-Process -Id $process.Id -ErrorAction Stop
        $process.WaitForExit()
        $state.state = 'time_limit'
    }
    $captures = Join-Path $exeFolder 'captures'
    if (Test-Path -LiteralPath $captures) {
        Get-ChildItem -LiteralPath $captures -File -Filter "video_${VideoAt}s.*" |
            Where-Object { $_.LastWriteTimeUtc -ge $startedUtc } | ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination $report
        }
    }
} catch {
    $state.state = 'failed'
    $state.error = $_.Exception.Message
} finally {
    $env:SA2_AUTO_INPUT = $oldInput
    $state.finished_utc = [DateTime]::UtcNow.ToString('o')
    $state | ConvertTo-Json | Set-Content -LiteralPath $statusPath -Encoding utf8
}
