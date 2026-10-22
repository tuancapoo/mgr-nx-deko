param([int]$Jobs = 5, [switch]$SkipConfigure)
$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$report = Join-Path $projectRoot 'out\validation'
New-Item -ItemType Directory -Path $report -Force | Out-Null
$statusPath = Join-Path $report 'pc-build-status.json'
$state = [ordered]@{pid=$PID;state='running';phase='configure';started_utc=[DateTime]::UtcNow.ToString('o')}
function Save-State { $state | ConvertTo-Json | Set-Content -LiteralPath $statusPath -Encoding utf8 }
Save-State
try {
    $scratch = Join-Path $projectRoot 'out\scratch-pc'
    New-Item -ItemType Directory -Path $scratch -Force | Out-Null
    $env:TEMP = $scratch
    $env:TMP = $scratch
    . (Join-Path $PSScriptRoot 'env.ps1')
    $ErrorActionPreference = 'Stop'
    Set-Location -LiteralPath $projectRoot
    # DXC: MGR_DXC_DIR, by default ..\_tools\dxc beside the checkout. Python: MGR_PYTHON, by default the one on PATH.
    $dxc = if ($env:MGR_DXC_DIR) { $env:MGR_DXC_DIR } else { Join-Path $projectRoot '..\_tools\dxc' }
    $env:PATH = "$dxc;" + $env:PATH
    $python = if ($env:MGR_PYTHON) { $env:MGR_PYTHON } else { (Get-Command python).Source }
    # Windows PowerShell treats native stderr (including CMake warnings) as
    # NativeCommandError. Check the process exit code rather than stopping on it.
    $ErrorActionPreference = 'Continue'
    if (-not $SkipConfigure) {
      & cmake --preset win-pc -S app '-DMGR_RENUT_RENDERER=ON' '-DMGR_NATIVE_RENDERER=OFF' `
        '-DREXGLUE_BUILD_HOST_TOOLS=OFF' '-DREXGLUE_RECOMP_DEBUG_INFO=none' `
        "-DPython3_EXECUTABLE=$python" "-DPYTHON_EXECUTABLE=$python" `
        *> (Join-Path $report 'pc-configure.log')
      $configureExit = $LASTEXITCODE
    } else { $configureExit = 0 }
    $ErrorActionPreference = 'Stop'
    if ($configureExit) { throw "PC configure exited $configureExit" }
    $state.phase = 'compile'
    Save-State
    $ErrorActionPreference = 'Continue'
    & cmake --build out/pc --target sa2 -j $Jobs *> (Join-Path $report 'pc-build.log')
    $buildExit = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    if ($buildExit) { throw "PC build exited $buildExit" }
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
