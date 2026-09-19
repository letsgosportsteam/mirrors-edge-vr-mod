# Compile twice, keep build 1 for testing, and package only independent build 2.
param(
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [Parameter(Mandatory=$true)][ValidatePattern('^[A-Za-z0-9][A-Za-z0-9._-]*$')][string]$ArchiveName
)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$out=[IO.Path]::GetFullPath($OutputDirectory)
if(Test-Path -LiteralPath $out){throw 'OutputDirectory must be new'}
New-Item -ItemType Directory -Path $out | Out-Null
$sdk=$env:MEVR_OPENXR_SDK
if(-not $sdk){
    . (Join-Path $root 'src/paths.local.ps1')
    $sdk=$OpenXrSdk
}
$loader=Join-Path $sdk 'native/Win32/release/bin/openxr_loader.dll'
if(-not(Test-Path -LiteralPath $loader)){throw 'OpenXR loader not found'}
$inputs=@(
    Get-ChildItem -LiteralPath (Join-Path $root 'src') -File | Where-Object {
        $_.Extension -in @('.cpp','.inl','.h','.def') -and $_.Name -notin @('vr_defaults.inl','build_identity.inl')
    }
    Get-Item -LiteralPath (Join-Path $root 'src/build.ps1'),(Join-Path $root 'mevr.ini.example'),
        (Join-Path $root 'LICENSE'),(Join-Path $root 'THIRD-PARTY-NOTICES.txt')
    Get-ChildItem -LiteralPath (Join-Path $root 'third_party/minhook') -Recurse -File | Where-Object {
        $_.Extension -in @('.c','.h') -or $_.Name -eq 'LICENSE.txt'
    }
) | Sort-Object FullName
$records=foreach($inputFile in $inputs){
    $relative=$inputFile.FullName.Substring($root.Length+1)
    $dest=Join-Path $out ('source/'+$relative)
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dest) | Out-Null
    Copy-Item -LiteralPath $inputFile.FullName -Destination $dest
    [ordered]@{name=$relative.Replace('\','/');sha256=(Get-FileHash -LiteralPath $inputFile.FullName).Hash}
}
$records | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $out 'source-files.json')
$names=@('d3d9.dll','mevr.ini','openxr_loader.dll')
for($build=1;$build -le 2;$build++){
    foreach($record in $records){
        if((Get-FileHash -LiteralPath (Join-Path $root $record.name)).Hash -cne $record.sha256){throw 'Source changed during candidate preparation'}
    }
    & (Join-Path $root 'src/build.ps1') -Reproducible *> (Join-Path $out "build-$build.log")
    # build.ps1 throws on analyzer/compiler failures; native exit codes are also
    # checked there. Never package stale products after an unsuccessful compile.
    $folder=Join-Path $out "build-$build"
    New-Item -ItemType Directory -Path $folder | Out-Null
    Copy-Item -LiteralPath (Join-Path $root 'src/d3d9.dll'),(Join-Path $root 'src/mevr.ini'),
        (Join-Path $root 'src/d3d9.pdb') -Destination $folder
    Copy-Item -LiteralPath $loader -Destination (Join-Path $folder 'openxr_loader.dll')
    $files=foreach($name in $names){
        $path=Join-Path $folder $name
        [ordered]@{name=$name;bytes=(Get-Item -LiteralPath $path).Length;sha256=(Get-FileHash -LiteralPath $path).Hash}
    }
    [ordered]@{schema=1;files=@($files)} | ConvertTo-Json -Depth 5 |
        Set-Content -LiteralPath (Join-Path $out "build-$build-files.json")
    foreach($record in $records){
        if((Get-FileHash -LiteralPath (Join-Path $root $record.name)).Hash -cne $record.sha256){throw 'Source changed during compilation'}
    }
    Write-Host "Completed independent source build $build."
}
$first=Get-Content -LiteralPath (Join-Path $out 'build-1-files.json') -Raw | ConvertFrom-Json
$second=Get-Content -LiteralPath (Join-Path $out 'build-2-files.json') -Raw | ConvertFrom-Json
for($i=0;$i -lt 3;$i++){
    if($first.files[$i].name -cne $second.files[$i].name -or
       $first.files[$i].bytes -ne $second.files[$i].bytes -or
       $first.files[$i].sha256 -cne $second.files[$i].sha256){
        throw "Independent rebuild mismatch: $($first.files[$i].name). No release ZIP created."
    }
}
$package=Join-Path $out 'package'
& (Join-Path $PSScriptRoot 'package-tested-build.ps1') -BuildDirectory (Join-Path $out 'build-2') `
    -OutputDirectory $package -ArchiveName $ArchiveName
# The verifier's reference is build 1, not the files that were put in the ZIP.
& (Join-Path $PSScriptRoot 'check-tested-package.ps1') -ZipPath (Join-Path $package "$ArchiveName.zip") `
    -ManifestPath (Join-Path $out 'build-1-files.json')
$pdb=Join-Path $out 'build-2/d3d9.pdb'
Copy-Item -LiteralPath $pdb -Destination (Join-Path $package "$ArchiveName.pdb")
[ordered]@{
    schema=1;headsetVerified=$false;gitHead=(& git -C $root rev-parse HEAD)
    sourceBuildId=([IO.File]::ReadAllText((Join-Path $root 'src/build_identity.inl')))
    reference='build-1';packagedBuild='build-2';independentBuildsMatch=$true
    zipSha256=(Get-FileHash -LiteralPath (Join-Path $package "$ArchiveName.zip")).Hash
    pdbSha256=(Get-FileHash -LiteralPath $pdb).Hash
} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $out 'candidate-evidence.json')
Write-Host "PASS: build-2 ZIP matches build-1 candidate for all three files. Headset approval is still required."
