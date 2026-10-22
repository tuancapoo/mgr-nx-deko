param(
    # The containers of media/data000.cpk (tools/extract_mgr_cpk_shaders.py).
    [string]$InputDirectory = (Join-Path $PSScriptRoot '..\..\out\cpk-shader-inputs'),
    [int]$Jobs = 8
)
$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
. (Join-Path $PSScriptRoot 'env.ps1')
$ErrorActionPreference = 'Stop'
$buildDir = Join-Path $projectRoot 'out\deko3d_tools\xenos-build'
& cmake --build $buildDir --target XenosRecomp --parallel $Jobs
if ($LASTEXITCODE) { exit $LASTEXITCODE }
$outDir = Join-Path $projectRoot 'out\mgr-deko-shaders'
New-Item -ItemType Directory -Force -Path $outDir,(Join-Path $outDir 'spirv'),(Join-Path $outDir 'hlsl'),(Join-Path $outDir 'renut_gpu') | Out-Null
$env:BANJO_DEKO3D_SPIRV_DIR = Join-Path $outDir 'spirv'
& (Join-Path $buildDir 'XenosRecomp\XenosRecomp.exe') $InputDirectory (Join-Path $outDir 'renut_gpu\shader_cache.cpp') (Join-Path $projectRoot 'app\thirdparty\reblue-XenosRecomp\XenosRecomp\shader_common.h') (Join-Path $outDir 'hlsl')
$compileResult=$LASTEXITCODE
Set-Content -LiteralPath (Join-Path $outDir 'translate-exit-code.txt') -Value $compileResult
exit $compileResult
