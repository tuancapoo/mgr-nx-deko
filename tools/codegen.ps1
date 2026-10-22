# Translates the game (default.xex) to C++ with the host code generator. It builds nothing: build the generator first
# (tools\win\run.ps1 out\host_build.log "cmake --build out/host --target rexglue").
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\codegen.ps1 [-Flavor classic|optimised]
#
# Input:   assets\game_root\default.xex   (tools\stfs_extract.py puts it there)
#          app\sa2_manifest.toml          (project, game paths, includes)
# Output:  app\generated\                 (C++ of the game and generated\rexglue.cmake; not committed)
#          out\codegen.log                (log of the generator)
#
# -Flavor: the codegen flavour (docs/enhancements.md, "Build-time"; build each as its own NRO for an A/B pair).
#   classic (default)  app\codegen_flavor.toml stays empty: the generator's plain output.
#   s1-fp-moves        only fp_moves_modeless; no optimised post-codegen passes.
#   s2-control-locals  s1 plus CR/CTR/XER/reserved locals; no optimised post-codegen passes.
#   optimised          app\codegen_flavor.toml = app\perf_overrides.toml (registers as locals, ...), then
#                      tools\pch_no_volatile.py and tools\direct_calls.py run on the generated code.
# All write app\generated\codegen_flavor.txt, which the build puts in the first "[enh]" log line.
# The PC build re-runs the generator by itself when an input changes; app/CMakeLists.txt appends tools/post_codegen.py
# to that command, so the flavour's steps survive it (the Switch cross build never runs the generator).
param(
    [string]$Rexglue = '',
    [switch]$DirectCalls,
    [ValidateSet('classic', 's1-fp-moves', 's2-control-locals', 'optimised')][string]$Flavor = 'classic'
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$app = Join-Path $root 'app'
if (-not $Rexglue) {
    # The newest generator: rexglue.exe (host build) or rexgluerd.exe (the PC tree's RelWithDebInfo build).
    $Rexglue = Get-ChildItem (Join-Path $root 'sdk\out') -Recurse -Include rexglue.exe, rexgluerd.exe -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1 -ExpandProperty FullName
}
if (-not $Rexglue -or -not (Test-Path $Rexglue)) { throw 'rexglue.exe not found; build it first (see the header)' }
$xex = Join-Path $root 'assets\game_root\default.xex'
if (-not (Test-Path $xex)) { throw "$xex not found; extract your package with tools\stfs_extract.py first" }

New-Item -ItemType Directory -Force (Join-Path $root 'out') | Out-Null
$flavorToml = Join-Path $app 'codegen_flavor.toml'
if ($Flavor -eq 'optimised') {
    $text = "# optimised codegen: written by tools\codegen.ps1 -Flavor optimised from perf_overrides.toml.`n" +
            (Get-Content -Raw (Join-Path $app 'perf_overrides.toml'))
} elseif ($Flavor -in 's1-fp-moves', 's2-control-locals') {
    # Generator options only. In particular, do NOT run the full post-codegen pipeline:
    # its constant folding and register rewrites must be tested as separate experiments.
    $text = "# codegen-stage: $Flavor`n# Generator-only experiment; written by tools/codegen.ps1.`n" +
            "fp_moves_modeless = true`n"
    if ($Flavor -eq 's2-control-locals') {
        $text += "cr_as_local = true`nctr_as_local = true`nxer_as_local = true`nreserved_as_local = true`n"
    }
} else {
    $text = "# classic codegen: no extra options (tools\codegen.ps1 -Flavor classic writes this file; -Flavor optimised`n" +
            "# copies perf_overrides.toml here). Included by sa2_manifest.toml.`n"
}
[System.IO.File]::WriteAllText($flavorToml, $text.Replace("`r`n", "`n"))
$log = Join-Path $root 'out\codegen.log'
Write-Host "== codegen ($Flavor) with $Rexglue"
Push-Location $app
try {
    $env:REX_MAX_JUMP_TABLE_ENTRIES = '1024'
    # The loaded image (decrypted, decompressed), for the post-codegen steps that read the game's read-only data
    # and look for function addresses in it (tools\ro_constants.py, the entry modes of tools\flush_mode_flow.py).
    $env:REXGLUE_DUMP_IMAGE = Join-Path $root 'out\default_image.bin'
    & $env:ComSpec /c "`"$Rexglue`" codegen sa2_manifest.toml > `"$log`" 2>&1"
    $code = $LASTEXITCODE
} finally { Pop-Location }
Get-Content $log -Tail 30
if ($code -ne 0) { throw "codegen failed (exit $code), see out\codegen.log" }

# The flavour's post-codegen steps (no-volatile pch, direct calls, localized natives, the build's name): the same script
# the build appends to its own codegen command (app/CMakeLists.txt), so a codegen the build re-runs keeps them too.
$env:REXSDK_DIR = Join-Path $root 'sdk'
python (Join-Path $root 'tools\post_codegen.py')
if ($LASTEXITCODE -ne 0) { throw 'tools\post_codegen.py failed' }
if ($DirectCalls -and $Flavor -eq 'classic') {
    python (Join-Path $root 'tools\direct_calls.py') --gen (Join-Path $app 'generated\default')
    if ($LASTEXITCODE -ne 0) { throw 'tools\direct_calls.py failed' }
}
Write-Host "done ($Flavor codegen): $app\generated"
