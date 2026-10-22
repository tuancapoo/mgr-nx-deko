# Stage the MGR hardware test for copying to an SD card. Existing saves are preserved.
param([switch]$IncludeGame, [string]$ShaderDir = '')
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$dest = Join-Path $root 'out\nx\sdcard\switch\mgr-nx'
$nro = Join-Path $root 'out\nx\sa2.nro'
if (-not (Test-Path -LiteralPath $nro)) { throw "No NRO at $nro. Build tools\win\build_switch.ps1 first." }
if (-not $ShaderDir) { $ShaderDir = Join-Path $root 'out\pc' }
foreach ($file in 'sa2_shaders.mesp', 'sa2_shaders.mesp.idx') {
    if (-not (Test-Path -LiteralPath (Join-Path $ShaderDir $file))) { throw "Missing shader file: $file in $ShaderDir" }
}
$stream = [IO.File]::OpenRead($nro)
try {
    $stream.Position = 16
    $magic = New-Object byte[] 4
    if ($stream.Read($magic, 0, 4) -ne 4 -or [Text.Encoding]::ASCII.GetString($magic) -ne 'NRO0') {
        throw "Invalid NRO header: $nro"
    }
} finally { $stream.Dispose() }
New-Item -ItemType Directory -Path (Join-Path $dest 'logs') -Force | Out-Null
Copy-Item -LiteralPath $nro -Destination (Join-Path $dest 'mgr.nro') -Force
if (-not (Test-Path -LiteralPath (Join-Path $dest 'mgr.toml'))) {
    Copy-Item -LiteralPath (Join-Path $root 'app\mgr_switch.toml') -Destination (Join-Path $dest 'mgr.toml')
}
Copy-Item -LiteralPath (Join-Path $root 'docs\mgr-switch-testing.md') -Destination (Join-Path $dest 'README.md') -Force
foreach ($file in 'sa2_shaders.mesp', 'sa2_shaders.mesp.idx') {
    Copy-Item -LiteralPath (Join-Path $ShaderDir $file) -Destination (Join-Path $dest $file) -Force
}
if ($IncludeGame) {
    $game = Join-Path $root 'assets\game_root'
    if (-not (Test-Path -LiteralPath (Join-Path $game 'default.xex'))) { throw "Missing game files: $game" }
    & robocopy $game (Join-Path $dest 'game_root') /E /NFL /NDL /NJH /NJS
    if ($LASTEXITCODE -ge 8) { throw "Game data copy failed ($LASTEXITCODE)." }
}
$manifest = [ordered]@{
    staged = (Get-Date).ToString('o')
    target = 'Nintendo Switch ARM64'
    hardware_tested = $false
    driver = 'mesa-switch 1a8c1a66d6f (no nfsmw patch)'
    driver_archive_sha256 = (Get-FileHash -LiteralPath (Join-Path $root 'out\switch-deps\nvk-1a8c1a66d6f\sdk\lib\libvulkan.a') -Algorithm SHA256).Hash
    files = @(foreach ($file in 'mgr.nro', 'sa2_shaders.mesp', 'sa2_shaders.mesp.idx') {
        $path = Join-Path $dest $file
        [ordered]@{ name = $file; bytes = (Get-Item -LiteralPath $path).Length;
            sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash }
    })
}
$manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $dest 'build_manifest.json') -Encoding UTF8
Get-ChildItem -LiteralPath $dest | Select-Object Name,Length
Write-Output "SD staging: $dest"
exit 0
