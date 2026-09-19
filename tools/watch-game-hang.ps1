# Run externally before reproducing a chapter-load freeze. Does not change the game.
param(
    [Parameter(Mandatory=$true)][string]$GameDirectory,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [ValidateRange(1,60)][int]$DurationMinutes=20
)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$out=[IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $out | Out-Null
$gameExe=[IO.Path]::GetFullPath((Join-Path $GameDirectory 'MirrorsEdge.exe'))
$helper=Join-Path $root '.analysis/hang-capture-tools/capture-hang.exe'
if(-not(Test-Path -LiteralPath $helper)){throw 'Build tools/capture-hang.cpp as x86 into .analysis/hang-capture-tools first'}
$log=Join-Path $env:LOCALAPPDATA 'MirrorsEdgeVR/mevr.log'
$status=Join-Path $out 'watcher-status.txt'
$deadline=(Get-Date).AddMinutes($DurationMinutes)
$gameId=0;$unresponsiveSince=$null;$foundAt=$null
'Waiting for the specified game. No files or settings will be changed.' | Set-Content -LiteralPath $status
while((Get-Date) -lt $deadline){
    $process=Get-Process -Name MirrorsEdge -ErrorAction SilentlyContinue | Where-Object {
        try{$_.Path -ieq $gameExe}catch{$false}
    } | Select-Object -First 1
    if(-not $process){
        if($gameId){'Game exited before a hang capture; no dump available.' | Add-Content -LiteralPath $status;exit 0}
        Start-Sleep -Seconds 1;continue
    }
    if($gameId -ne $process.Id){
        $gameId=$process.Id;$foundAt=Get-Date;$unresponsiveSince=$null
        "Watching game PID $gameId. Leave a freeze in place for at least 30 seconds." | Add-Content -LiteralPath $status
        foreach($name in @('d3d9.dll','openxr_loader.dll','mevr.ini')){
            $path=Join-Path $GameDirectory $name
            if(Test-Path -LiteralPath $path){
                "$name $((Get-FileHash -LiteralPath $path).Hash)" | Add-Content -LiteralPath (Join-Path $out 'installed-hashes.txt')
            }
        }
    }
    $process.Refresh()
    if($process.MainWindowHandle -ne 0 -and -not $process.Responding){
        if(-not $unresponsiveSince){$unresponsiveSince=Get-Date}
    }else{$unresponsiveSince=$null}
    $now=Get-Date
    $unresponsive=$unresponsiveSince -and ($now-$unresponsiveSince).TotalSeconds -ge 15
    $staleLog=(Test-Path -LiteralPath $log) -and ($now-$foundAt).TotalSeconds -ge 30 -and
        (Get-Item -LiteralPath $log).LastWriteTime -ge $foundAt.AddSeconds(-10) -and
        ($now-(Get-Item -LiteralPath $log).LastWriteTime).TotalSeconds -ge 20
    if($unresponsive -or $staleLog){
        "Capture trigger at $($now.ToString('o')); unresponsive=$unresponsive staleLog=$staleLog" | Add-Content -LiteralPath $status
        for($sample=1;$sample -le 2;$sample++){
            $dump=Join-Path $out ("MirrorsEdge-$gameId-$sample.dmp")
            & $helper $gameId $dump >> $status
            if($LASTEXITCODE -ne 0){throw "Dump capture failed; inspect $status"}
            if($sample -eq 1){Start-Sleep -Seconds 5}
        }
        if(Test-Path -LiteralPath $log){Copy-Item -LiteralPath $log -Destination (Join-Path $out "mevr-$gameId.log")}
        'CAPTURE COMPLETE: both dumps saved. The game can now be closed.' | Add-Content -LiteralPath $status
        exit 0
    }
    Start-Sleep -Seconds 1
}
'Capture window expired without a detected hang.' | Add-Content -LiteralPath $status
