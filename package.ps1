<#
.SYNOPSIS
  Builds the release zip from pkg\ (run build.cmd first): dist\lbp1120-driver-<Version>-x64.zip
  with pkg\, install.ps1, uninstall.ps1, README.md and COPYING under a lbp1120-driver-<Version>-x64\
  folder, and prints its SHA-256. Works in Windows PowerShell 5.1 and PowerShell 7.
#>
param([Parameter(Mandatory)][string]$Version)
$ErrorActionPreference = 'Stop'
$name  = "lbp1120-driver-$Version-x64"
$dist  = Join-Path $PSScriptRoot 'dist'
$zip   = Join-Path $dist "$name.zip"
$files = [ordered]@{}
foreach ($f in 'lbp1120v3.inf', 'lbp1120v3.cdf', 'lbp1120.gpd', 'lbp1120.ini', 'lbp1120rnd.dll', 'lbp1120lm.dll') {
    $files["$name/pkg/$f"] = Join-Path $PSScriptRoot "pkg\$f"
}
foreach ($f in 'install.ps1', 'uninstall.ps1', 'README.md', 'COPYING') { $files["$name/$f"] = Join-Path $PSScriptRoot $f }
foreach ($src in $files.Values) { if (-not (Test-Path -LiteralPath $src)) { throw "Missing $src - run build.cmd first." } }

if (Test-Path $dist) { Remove-Item $dist -Recurse -Force }
[void](New-Item -ItemType Directory $dist)
# Entries are written one by one with "/" separators: Compress-Archive and ZipFile in Windows
# PowerShell 5.1 write "\", which tools outside Windows do not read as folders.
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::Open($zip, 'Create')
try {
    foreach ($e in $files.GetEnumerator()) {
        [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $e.Value, $e.Key, 'Optimal')
    }
} finally { $archive.Dispose() }

$sha = [Security.Cryptography.SHA256]::Create()
$stream = [IO.File]::OpenRead($zip)
try { $hash = [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '') } finally { $stream.Dispose() }
$zip
"SHA-256 $hash"
