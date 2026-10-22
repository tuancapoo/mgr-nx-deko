# Builds and runs the SDK's unit tests of the CPU round of 2026-10-12 (sdk\tests\*.cpp, from sasrt-nx) on the PC with
# clang. Each test prints its own result and exits non-zero on a difference.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\win\run_sdk_tests.ps1 [-Only fp_any_mode] [-FpPairs 30000000]
#
# fp_any_mode_test runs twice: the helpers (must report 0 differences) and its negative control (the plain operations
# in the helpers' place: must report differences, which shows the inputs reach the cases where the mode matters).
param(
    [string]$Only = '',
    [int]$FpPairs = 30000000,
    [int]$SignalSeconds = 8
)
. (Join-Path $PSScriptRoot 'env.ps1')
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
Set-Location $root
New-Item -ItemType Directory -Force out\tools | Out-Null
$inc = @('-Isdk/include', '-Isdk/thirdparty/simde', '-Isdk/thirdparty/simde/simde', '-Iout/pc/rexglue-sdk/include')
$tests = @(
    @{ name = 'fp_moves_modeless_test'; flags = @('-O2', '-std=c++23', '-ffp-contract=off'); runs = @(@('1000000'), @('1000000', 'control')) },
    @{ name = 'fp_any_mode_test'; flags = @('-O2', '-std=c++23', '-ffp-contract=off'); runs = @(@("$FpPairs"), @('2000000', 'control')) },
    @{ name = 'rotate_word_test'; flags = @('-O2', '-std=c++20'); runs = @(, @()) },
    @{ name = 'signal_skip_idle_test'; flags = @('-O2', '-std=c++20'); runs = @(, @("$SignalSeconds")) },
    @{ name = 'vec_byteswap_test'; flags = @('-O2', '-std=c++23', '-march=x86-64-v2'); runs = @(, @()) },
    @{ name = 'vmx_helpers_neon_test'; flags = @('-O2', '-std=c++23', '-march=x86-64-v2'); runs = @(, @()) },
    @{ name = 'vmx_small_helpers_test'; flags = @('-O2', '-std=c++23', '-march=x86-64-v2'); runs = @(, @()) },
    @{ name = 'neon_shuffles_test'; flags = @('-O2', '-std=c++23', '-march=x86-64-v2'); runs = @(, @()) },
    @{ name = 'fcti_copy_test'; flags = @('-O2', '-std=c++23', '-ffp-contract=off'); runs = @(, @()) },
    @{ name = 'float_loads_test'; flags = @('-O2', '-std=c++23', '-march=x86-64-v2'); runs = @(, @()) }
)
$failed = 0
foreach ($t in $tests) {
    if ($Only -and $t.name -notlike "*$Only*") { continue }
    $exe = "out\tools\$($t.name).exe"
    "== $($t.name): building"
    & $env:ComSpec /c "clang++ $($t.flags -join ' ') $($inc -join ' ') sdk/tests/$($t.name).cpp -o $exe 2>&1"
    if ($LASTEXITCODE -ne 0) { "BUILD FAILED ($LASTEXITCODE)"; $failed++; continue }
    foreach ($run in $t.runs) {
        "== $($t.name) $($run -join ' ')"
        & $env:ComSpec /c "$exe $($run -join ' ') 2>&1"
        # The control run exits 0 when it DID find differences (the test's own rule), so every run must exit 0.
        if ($LASTEXITCODE -ne 0) { "FAILED (exit $LASTEXITCODE)"; $failed++ }
    }
}
"== done: $failed failed"
exit $failed
