# Freeze the installed test files and package them without compiling or changing settings.
# OutputDirectory must be new. Keep its manifest for local AND downloaded-asset verification.
param(
    [Parameter(Mandatory=$true)][string]$BuildDirectory,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [Parameter(Mandatory=$true)][ValidatePattern('^[A-Za-z0-9][A-Za-z0-9._-]*$')][string]$ArchiveName
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$source = (Resolve-Path -LiteralPath $BuildDirectory).Path
$output = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $output) { throw 'OutputDirectory already exists; frozen builds are never overwritten' }
$names = @('d3d9.dll','mevr.ini','openxr_loader.dll')
$streams = @{}
try {
    # Hold all three files read-only for the entire snapshot. Windows denies
    # concurrent saves/replacements so settings cannot change halfway through it.
    foreach ($name in $names) {
        $streams[$name] = [IO.File]::Open((Join-Path $source $name),[IO.FileMode]::Open,
            [IO.FileAccess]::Read,[IO.FileShare]::Read)
    }
    foreach ($name in @('d3d9.dll','openxr_loader.dll')) {
        $s = $streams[$name]
        $reader = [IO.BinaryReader]::new($s,[Text.Encoding]::UTF8,$true)
        try {
            if ($s.Length -lt 64 -or $reader.ReadUInt16() -ne 0x5A4D) { throw "$name is not a PE binary" }
            $s.Position=0x3C; $pe=$reader.ReadInt32()
            if ($pe -lt 0 -or $pe -gt $s.Length-6) { throw "$name has an invalid PE header" }
            $s.Position=$pe
            if ($reader.ReadUInt32() -ne 0x4550 -or $reader.ReadUInt16() -ne 0x14C) { throw "$name is not x86" }
        } finally { $reader.Dispose(); $s.Position=0 }
    }
    New-Item -ItemType Directory -Path $output | Out-Null
    $stage = Join-Path $output 'files'
    New-Item -ItemType Directory -Path $stage | Out-Null
    $records = foreach ($name in $names) {
        $s=$streams[$name]; $sha=[Security.Cryptography.SHA256]::Create()
        try { $hash=[BitConverter]::ToString($sha.ComputeHash($s)).Replace('-','') }
        finally { $sha.Dispose(); $s.Position=0 }
        $dest=[IO.File]::Open((Join-Path $stage $name),[IO.FileMode]::CreateNew,[IO.FileAccess]::Write)
        try { $s.CopyTo($dest) } finally { $dest.Dispose() }
        [ordered]@{ name=$name; bytes=$s.Length; sha256=$hash }
    }
} finally { foreach ($s in $streams.Values) { $s.Dispose() } }
$manifestPath=Join-Path $output 'tested-files.json'
[ordered]@{ schema=1; capturedUtc=[DateTime]::UtcNow.ToString('o'); files=@($records) } |
    ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $manifestPath -Encoding UTF8
$zip=Join-Path $output "$ArchiveName.zip"
Compress-Archive -LiteralPath @($names | ForEach-Object { Join-Path $stage $_ }) -DestinationPath $zip
& (Join-Path $PSScriptRoot 'check-tested-package.ps1') -ZipPath $zip -ManifestPath $manifestPath
# Notices accompany the release as a separate asset when the approved INI does
# not contain them. Never edit the approved INI just to insert comments.
$notices = foreach ($file in @('LICENSE','THIRD-PARTY-NOTICES.txt','third_party/minhook/LICENSE.txt')) {
    "===== $file =====`r`n" + [IO.File]::ReadAllText((Join-Path $root $file))
}
[IO.File]::WriteAllText((Join-Path $output 'LICENSES.txt'),($notices -join "`r`n`r`n"))
Write-Host "Frozen: $zip"
Write-Host ('ZIP SHA256: ' + (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash)
Write-Host 'No compilation, INI edits, version replacement, or diagnostic changes were performed.'
Write-Host 'Publish LICENSES.txt with the ZIP. Only attach a PDB proven to match this exact DLL.'
