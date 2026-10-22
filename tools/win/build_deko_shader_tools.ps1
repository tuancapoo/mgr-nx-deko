param([int]$Jobs = 8)
$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
. (Join-Path $PSScriptRoot 'env.ps1')
$ErrorActionPreference = 'Stop'
$toolOut = Join-Path $projectRoot 'out\deko3d_tools'
$crossRevision = (Get-Content (Join-Path $toolOut 'spirv-cross-revision.txt') -Raw).Trim()
$crossSource = Join-Path $toolOut ('SPIRV-Cross-' + $crossRevision)
& cmake -S $crossSource -B (Join-Path $toolOut 'spirv-cross-build') -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DSPIRV_CROSS_ENABLE_TESTS=OFF -DSPIRV_CROSS_ENABLE_C_API=OFF -DSPIRV_CROSS_ENABLE_HLSL=ON -DSPIRV_CROSS_ENABLE_MSL=ON
if ($LASTEXITCODE) { exit $LASTEXITCODE }
& cmake --build (Join-Path $toolOut 'spirv-cross-build') --target spirv-cross --parallel $Jobs
if ($LASTEXITCODE) { exit $LASTEXITCODE }
& cmake -S (Join-Path $projectRoot 'app\thirdparty\reblue-XenosRecomp') -B (Join-Path $toolOut 'xenos-build') -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DXENOS_RECOMP_RENUT=ON -DXENOS_RECOMP_DXIL=OFF -DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_TESTS=OFF -DZSTD_BUILD_SHARED=OFF
if ($LASTEXITCODE) { exit $LASTEXITCODE }
& cmake --build (Join-Path $toolOut 'xenos-build') --target XenosRecomp --parallel $Jobs
exit $LASTEXITCODE
