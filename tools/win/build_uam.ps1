# Build the pinned, patched deko3d compiler as a native Windows host tool.
param([ValidateRange(1,32)][int]$Jobs = 4)
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
. (Join-Path $PSScriptRoot 'env.ps1')
$build = Join-Path $root 'out\deko3d_tools\uam-build'
& cmake -S (Join-Path $root 'app\thirdparty\uam') -B $build -G Ninja `
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
if ($LASTEXITCODE) { exit $LASTEXITCODE }
& cmake --build $build --target uam_tool -j $Jobs
exit $LASTEXITCODE
