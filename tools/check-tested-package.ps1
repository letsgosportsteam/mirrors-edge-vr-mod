# Verify the archive against a frozen record of the tested bytes, not today's source defaults.
param(
    [Parameter(Mandatory=$true)][string]$ZipPath,
    [Parameter(Mandatory=$true)][string]$ManifestPath
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$manifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
$names = @('d3d9.dll','mevr.ini','openxr_loader.dll')
if ($manifest.schema -ne 1 -or @($manifest.files).Count -ne 3 -or
    ((@($manifest.files | ForEach-Object { $_.name } | Sort-Object) -join '|') -cne ($names -join '|'))) {
    throw 'Invalid tested-build manifest'
}
$archive = [IO.Compression.ZipFile]::OpenRead((Resolve-Path -LiteralPath $ZipPath).Path)
try {
    if ($archive.Entries.Count -ne 3 -or
        ((@($archive.Entries | ForEach-Object { $_.FullName } | Sort-Object) -join '|') -cne ($names -join '|'))) {
        throw 'ZIP must contain exactly the three top-level install files'
    }
    foreach ($file in $manifest.files) {
        $entry = $archive.GetEntry($file.name)
        $stream = $entry.Open()
        $sha = [Security.Cryptography.SHA256]::Create()
        try { $hash = [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-','') }
        finally { $sha.Dispose(); $stream.Dispose() }
        if ($entry.Length -ne $file.bytes -or $hash -cne $file.sha256) {
            throw "ZIP differs from tested build: $($file.name)"
        }
    }
} finally { $archive.Dispose() }
Write-Host 'PASS all three ZIP entries match the frozen tested files byte for byte.'
