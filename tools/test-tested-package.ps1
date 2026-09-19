$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$out=Join-Path $root ('.analysis/tested-package-tests/'+[guid]::NewGuid().ToString('N'))
$source=Join-Path $out 'source'
New-Item -ItemType Directory -Path $source -Force | Out-Null
# Minimal PE headers exercise architecture validation; no game or compiler needed.
$pe=New-Object byte[] 128
$pe[0]=0x4D;$pe[1]=0x5A;$pe[0x3C]=0x40;$pe[0x40]=0x50;$pe[0x41]=0x45;$pe[0x44]=0x4C;$pe[0x45]=1
foreach($name in @('d3d9.dll','openxr_loader.dll')){[IO.File]::WriteAllBytes((Join-Path $source $name),$pe)}
[IO.File]::WriteAllText((Join-Path $source 'mevr.ini'),"Debug = on`r`nArmSwingDebug = on`r`n; keep exactly`r`n")
$frozen=Join-Path $out 'frozen'
& "$PSScriptRoot/package-tested-build.ps1" -BuildDirectory $source -OutputDirectory $frozen -ArchiveName mevr-tested
$zip=Join-Path $frozen 'mevr-tested.zip';$manifest=Join-Path $frozen 'tested-files.json'
foreach($name in @('d3d9.dll','openxr_loader.dll','mevr.ini')) {
    if((Get-FileHash (Join-Path $source $name)).Hash -ne (Get-FileHash (Join-Path $frozen "files/$name")).Hash){throw 'snapshot changed bytes'}
}
function MustFail([scriptblock]$Action) { $failed=$false;try{& $Action}catch{$failed=$true};if(-not $failed){throw 'Expected rejection'} }
MustFail { & "$PSScriptRoot/package-tested-build.ps1" -BuildDirectory $source -OutputDirectory $frozen -ArchiveName mevr-tested }
# Changed settings, changed binary, and extra ZIP entries are each rejected.
foreach($changed in @('mevr.ini','d3d9.dll','extra.txt')) {
    $archive=[IO.Compression.ZipFile]::Open($zip,[IO.Compression.ZipArchiveMode]::Update)
    try { $entry=$archive.GetEntry($changed);if($entry){$entry.Delete()};$entry=$archive.CreateEntry($changed);$writer=[IO.StreamWriter]::new($entry.Open());try{$writer.Write('changed')}finally{$writer.Dispose()} }
    finally{$archive.Dispose()}
    MustFail { & "$PSScriptRoot/check-tested-package.ps1" -ZipPath $zip -ManifestPath $manifest }
    # Restore a known-good archive from the frozen files, never the changed input.
    Remove-Item -LiteralPath $zip
    Compress-Archive -LiteralPath (Join-Path $frozen 'files/d3d9.dll'),(Join-Path $frozen 'files/openxr_loader.dll'),(Join-Path $frozen 'files/mevr.ini') -DestinationPath $zip
}
& "$PSScriptRoot/check-tested-package.ps1" -ZipPath $zip -ManifestPath $manifest
MustFail { & "$root/src/build.ps1" -Package }
Write-Host 'PASS exact bytes/settings, immutable output, tampered INI/DLL, extra entries, and rebuild-for-release rejection.'
