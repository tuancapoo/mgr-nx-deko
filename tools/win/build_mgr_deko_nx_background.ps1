param([int]$Jobs = 4, [string]$Target = 'sa2')
$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$report = Join-Path $projectRoot 'out\validation'
New-Item -ItemType Directory -Path $report -Force | Out-Null
$statusPath = Join-Path $report 'nx-build-status.json'
$state = [ordered]@{pid=$PID;state='running';target=$Target;phase='configure';started_utc=[DateTime]::UtcNow.ToString('o')}
function Save-State { $state | ConvertTo-Json | Set-Content -LiteralPath $statusPath -Encoding utf8 }
Save-State
try {
    $scratch = Join-Path $projectRoot 'out\scratch-nx'
    New-Item -ItemType Directory -Path $scratch -Force | Out-Null
    $env:TEMP = $scratch
    $env:TMP = $scratch
    . (Join-Path $PSScriptRoot 'env.ps1')
    # GCC finds GNU make on PATH before it can schedule parallel LTRANS jobs.
    # Keep MSYS last so its link/find never override the Windows build tools.
    $env:PATH += ';C:\devkitPro\msys2\usr\bin'
    Set-Location -LiteralPath $projectRoot
    $ErrorActionPreference = 'Continue'
    & cmake out/nx '-DMGR_SWITCH_LTO_JOBS=4' *> (Join-Path $report 'nx-configure.log')
    $configureExit = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    if ($configureExit) { throw "Switch configure exited $configureExit" }
    $state.phase = 'compile'
    Save-State
    $ErrorActionPreference = 'Continue'
    & cmake --build out/nx --target $Target -j $Jobs *> (Join-Path $report 'nx-build.log')
    $buildExit = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    if ($buildExit) { throw "Switch build exited $buildExit" }
    $state.state = 'completed'
    $state.phase = 'complete'
    $state.exit_code = 0
} catch {
    $state.state = 'failed'
    $state.error = $_.Exception.Message
    $state.exit_code = 1
} finally {
    $state.finished_utc = [DateTime]::UtcNow.ToString('o')
    Save-State
}
