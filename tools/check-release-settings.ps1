# Compare effective settings using the production loader in the menu harness.
# Run tools/test-vr-menu.ps1 first. All writes stay in ignored test directories.
param(
    [Parameter(Mandatory=$true)][string]$ReferenceIni,
    [Parameter(Mandatory=$true)][string]$CandidateIni
)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$exe=Join-Path $root '.analysis/vr-menu-tests/test-vr-menu.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw 'Run tools/test-vr-menu.ps1 first' }
$out=Join-Path $root '.analysis/release-settings-check'
function Read-Settings([string]$path) {
    $settings=@{}
    foreach($line in [IO.File]::ReadAllLines($path)) {
        if($line -match '^\s*([^;#=\s][^=]*?)\s*=\s*([^;\r\n]*?)\s*(?:;.*)?$') {
            $key=$matches[1].Trim();$value=$matches[2].Trim()
            if($settings.ContainsKey($key)) { throw "Duplicate setting: $key" }
            $settings[$key]=$value
        }
    }
    return $settings
}
$snapshots=@{}
foreach($item in @(@{Name='reference';Path=$ReferenceIni},@{Name='candidate';Path=$CandidateIni})) {
    $folder=Join-Path $out $item.Name
    New-Item -ItemType Directory -Path $folder -Force | Out-Null
    & $exe --snapshot (Resolve-Path -LiteralPath $item.Path).Path $folder
    if($LASTEXITCODE -ne 0) { throw "Settings snapshot failed: $($item.Name)" }
    $log=[IO.File]::ReadAllText((Join-Path $folder 'mevr.log'))
    if($log -notmatch '\[cfg\] \d+ applied, 0 ignored\.') { throw "Loader rejected settings: $($item.Name); inspect its audit log" }
    $snapshots[$item.Name]=Read-Settings (Join-Path $folder 'mevr.ini')
}
$expected=Read-Settings (Join-Path $root 'mevr.ini.example')
$debugKeys=@('Debug','MotionHandsDebug','ArmSwingDebug','ParkourDebug','PickupDebug','ParkourGeomCensus','TestStall','TestWideFov')
$differences=@()
foreach($key in $expected.Keys) {
    foreach($side in @('reference','candidate')) {
        if(-not $snapshots[$side].ContainsKey($key)) { throw "Snapshot does not cover $key ($side)" }
    }
    $before=$snapshots.reference[$key];$after=$snapshots.candidate[$key]
    if($key -in $debugKeys) {
        if($after -notin @('off','0')) { throw "Release diagnostics must be off: $key=$after" }
        if($before -ne $after) { Write-Host "Allowed diagnostic difference: $key $before -> $after" }
    } elseif($before -ne $after) {
        $differences += [pscustomobject]@{Setting=$key;TestBuild=$before;Release=$after}
    }
}
if($differences.Count) {
    $differences | Sort-Object Setting | Format-Table -AutoSize | Out-String | Write-Host
    throw 'Release settings differ from the test build'
}
Write-Host "PASS all $($expected.Count) effective settings match the test build, except disabled diagnostics."
