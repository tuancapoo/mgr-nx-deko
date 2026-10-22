param(
    [Parameter(Mandatory=$true)][string]$GameFolder,
    [Parameter(Mandatory=$true)][string]$Chapter,
    [string]$OutputFolder = "$PSScriptRoot/exports"
)
$ErrorActionPreference = 'Stop'
if ($Chapter -notmatch '^[A-Za-z0-9][A-Za-z0-9_.-]{0,63}$') {
    throw 'Use a chapter label such as R-00 or R-01.'
}
$gameRoot = (Resolve-Path -LiteralPath $GameFolder).Path.TrimEnd('\','/')
if ((Get-Item -LiteralPath $gameRoot).Attributes -band [IO.FileAttributes]::ReparsePoint) {
    throw 'Choose the actual game directory, not a directory link.'
}
New-Item -ItemType Directory -Path $OutputFolder -Force | Out-Null
$outputRoot = (Resolve-Path -LiteralPath $OutputFolder).Path
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$output = [IO.Path]::GetFullPath((Join-Path $outputRoot ("MGR-{0}-{1}.zip" -f $Chapter,$stamp)))
$pending = $output + '.pending'
if (!$output.StartsWith($outputRoot.TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar,
                        [StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid export path.' }

# Do not follow links into unrelated data. Only captures, logs and two cache
# files are exported; disc assets and save data are never enumerated.
function Get-CaptureFiles([string]$Directory) {
    if (!(Test-Path -LiteralPath $Directory -PathType Container)) { return }
    if ((Get-Item -LiteralPath $Directory).Attributes -band [IO.FileAttributes]::ReparsePoint) { return }
    foreach ($item in Get-ChildItem -LiteralPath $Directory -Force) {
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { continue }
        if ($item.PSIsContainer) { Get-CaptureFiles $item.FullName }
        else { $item }
    }
}
$files = @(Get-CaptureFiles (Join-Path $gameRoot 'collect'))
$files += @(Get-CaptureFiles (Join-Path $gameRoot 'logs'))
foreach ($name in @('sa2_native_pipelines.bin','mgr_pipeline_seed.bin')) {
    $cacheDirectory = Join-Path $gameRoot 'cache'
    if ((Test-Path -LiteralPath $cacheDirectory) -and
        ((Get-Item -LiteralPath $cacheDirectory).Attributes -band [IO.FileAttributes]::ReparsePoint)) { continue }
    $path = Join-Path $gameRoot "cache/$name"
    if (Test-Path -LiteralPath $path -PathType Leaf) {
        $file = Get-Item -LiteralPath $path
        if (!($file.Attributes -band [IO.FileAttributes]::ReparsePoint)) { $files += $file }
    }
}
if (!$files.Count) { throw 'No collect/cache/log files found. Close the game and choose its NRO directory.' }
$total = 0L
foreach ($file in $files) {
    $total += $file.Length
    if ($file.Length -gt 256MB -or $total -gt 512MB) {
        throw 'Capture exceeds 512 MiB or contains a file over 256 MiB. Split the diagnostic logs first.'
    }
}
$manifest = [ordered]@{
    schema = 1; chapter = $Chapter; created_utc = (Get-Date).ToUniversalTime().ToString('o')
    coverage = 'Observed gameplay only; not proof of complete chapter coverage'
    files = [ordered]@{}
}
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$stream = [IO.FileStream]::new($pending, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
$archive = $null
try {
    $archive = [IO.Compression.ZipArchive]::new($stream, [IO.Compression.ZipArchiveMode]::Create, $false)
    foreach ($file in $files | Sort-Object FullName) {
        $name = $file.FullName.Substring($gameRoot.Length + 1).Replace('\','/')
        $sha = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $file.FullName, $name,
            [IO.Compression.CompressionLevel]::Fastest) | Out-Null
        $manifest.files[$name] = [ordered]@{ bytes = $file.Length; sha256 = $sha }
    }
    $entry = $archive.CreateEntry('snapshot.json')
    $writer = [IO.StreamWriter]::new($entry.Open(), [Text.UTF8Encoding]::new($false))
    try { $writer.Write(($manifest | ConvertTo-Json -Depth 8)) }
    finally { $writer.Dispose() }
} finally {
    if ($archive) { $archive.Dispose() }
    $stream.Dispose()
}
$check = [IO.Compression.ZipFile]::OpenRead($pending)
try {
    if ($check.Entries.Count -ne $files.Count + 1) { throw 'Incomplete ZIP; input files were preserved.' }
} finally { $check.Dispose() }
Move-Item -LiteralPath $pending -Destination $output
Write-Output "Chapter: $Chapter; exported $($files.Count) files. Input cache and saves were preserved."
Write-Output "Send this ZIP: $output"
