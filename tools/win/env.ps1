# Loads the Windows build environment of the port into the current PowerShell session:
#   MSVC x64 libraries and Windows SDK (VS 2022 Build Tools, for clang's msvc target),
#   LLVM clang 20 (ReXGlue needs clang), and one single CMake + Ninja.
#
#   . tools\win\env.ps1
#
# Override the locations with $env:SA2_LLVM, $env:SA2_VSDIR, $env:SA2_CMAKE_DIR if yours differ.
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$llvm = if ($env:SA2_LLVM) { $env:SA2_LLVM } else { Join-Path $root '..\_tools\llvm' }
$vs = if ($env:SA2_VSDIR) { $env:SA2_VSDIR } else { 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools' }
$cm = if ($env:SA2_CMAKE_DIR) { $env:SA2_CMAKE_DIR } else { Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake' }

$llvm = (Resolve-Path $llvm).Path
if (-not (Test-Path "$llvm\bin\clang++.exe")) { throw "clang not found in $llvm\bin" }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found under $vs" }

if (-not $env:SA2_ENV_LOADED) {
    # devkitPro puts MSYS2's usr/bin on PATH, which has its own `cmd`, `link` and `find`: keep it out of this
    # session, and call Windows' cmd.exe by its full path.
    $env:PATH = (($env:PATH -split ';') | Where-Object { $_ -and $_ -notmatch 'msys2\\usr\\bin' }) -join ';'
    $env:PATH = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer;$env:PATH"
    & $env:ComSpec /c "`"$vcvars`" >nul && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
    }
    if (-not $env:VCToolsInstallDir) { throw "vcvars64.bat did not set up MSVC" }
    $env:PATH = "$llvm\bin;$cm\CMake\bin;$cm\Ninja;$env:PATH"
    $env:CC = 'clang'
    $env:CXX = 'clang++'
    $env:SA2_ENV_LOADED = '1'
}
$ErrorActionPreference = 'Continue'
