# Builds out\nx\sa2.nro with reNut's Direct3D frontend and deko3d. No Vulkan driver is linked.
param(
    [ValidateRange(1,64)][int]$Jobs = 4,
    [switch]$Reconfigure,
    [switch]$CompileObjectsOnly,
    [switch]$NoNvk,
    [switch]$ConfigureOnly
)
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
. (Join-Path $PSScriptRoot 'env.ps1')
$reference = Join-Path (Split-Path $root) 'mgr-nx'
$nvk = if ($env:NVK_SDK) { $env:NVK_SDK } else {
    Join-Path $reference 'out\switch-deps\nvk-1a8c1a66d6f\sdk'
}
$devkitpro = if ($env:DEVKITPRO -and (Test-Path -LiteralPath $env:DEVKITPRO)) { $env:DEVKITPRO } else { 'C:/devkitPro' }
$build = Join-Path $root 'out\nx'
$hostPython = $env:MGR_HOST_PYTHON
if (-not $hostPython) {
    foreach ($cache in @((Join-Path $root 'out\pc\CMakeCache.txt'), (Join-Path $reference 'out\pc\CMakeCache.txt'))) {
        if (Test-Path -LiteralPath $cache) {
            $entry = Select-String -LiteralPath $cache -Pattern '^PYTHON_EXECUTABLE:FILEPATH=(.+)$' | Select-Object -First 1
            if ($entry -and (Test-Path -LiteralPath $entry.Matches[0].Groups[1].Value)) {
                $hostPython = $entry.Matches[0].Groups[1].Value
                break
            }
        }
    }
}
if (-not $hostPython) { throw 'Set MGR_HOST_PYTHON to a native Windows python.exe.' }
if (-not (Test-Path -LiteralPath "$devkitpro\libnx\lib\libdeko3d.a")) {
    throw "deko3d is missing from $devkitpro. Install the devkitPro switch-deko3d package."
}
$configure = @('-S', (Join-Path $root 'app'), '-B', $build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
    "-DCMAKE_TOOLCHAIN_FILE=$root\tools\switch\cmake\switch-devkitA64.cmake",
    "-DREXGLUE_SWITCH_NVK_SDK=$nvk", "-DDEVKITPRO=$devkitpro",
    '-DREXGLUE_SWITCH_NO_NVK=ON', '-DMGR_RENUT_RENDERER=ON', '-DMGR_NATIVE_RENDERER=OFF',
    '-DREXGLUE_RECOMP_DEBUG_INFO=none', '-DREXGLUE_BUILD_HOST_TOOLS=OFF', "-DPYTHON_EXECUTABLE=$hostPython", "-DPython3_EXECUTABLE=$hostPython",
    "-DCMAKE_MAKE_PROGRAM=$((Get-Command ninja).Source)", '-DGIT_EXECUTABLE=C:/Program Files/Git/cmd/git.exe')
if ($Reconfigure) { $configure += '--fresh' }
& cmake @configure
if ($LASTEXITCODE) { exit $LASTEXITCODE }
if ($ConfigureOnly) { exit 0 }
$target = if ($CompileObjectsOnly) { 'sa2_recomp' } else { 'sa2' }
& cmake --build $build --target $target -j $Jobs
exit $LASTEXITCODE
