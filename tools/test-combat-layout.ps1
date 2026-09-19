# Check the production reflection requests against the user's shipped package metadata.
# Requires a decompressed TdGame.u and the existing Eliot.UELib assembly. No assets
# are emitted or distributed. -SimulateOldMeshType must fail to reproduce test run 1.
param(
    [Parameter(Mandatory=$true)][string]$Package,
    [Parameter(Mandatory=$true)][string]$UELib,
    [string]$EnginePackage,
    [switch]$SimulateOldMeshType
)
$ErrorActionPreference = 'Stop'
Add-Type -Path (Resolve-Path -LiteralPath $UELib).Path
$pkg = [UELib.UnrealLoader]::LoadPackage((Resolve-Path -LiteralPath $Package).Path,
    [UELib.UnrealPackage+GameBuild+BuildName]::MirrorsEdge, [IO.FileAccess]::Read)
try {
    $source = Get-Content (Join-Path $PSScriptRoot '../src/combat.inl') -Raw
    if ($SimulateOldMeshType) {
        $source = $source.Replace('"Mesh1p", "ComponentProperty"', '"Mesh1p", "ObjectProperty"')
    }
    $requests = [regex]::Matches($source,
        '\{"(?<owner>\w+)",\s*"(?<name>\w+)",\s*"(?<type>\w+Property)",\s*&g_combat\w+\}')
    if ($requests.Count -ne 2) { throw 'Expected both combat field requests in production source' }
    foreach ($request in $requests) {
        $owner = $request.Groups['owner'].Value
        $name = $request.Groups['name'].Value
        $kind = $request.Groups['type'].Value
        $exports = @($pkg.Exports | Where-Object {
            [string]$_.ObjectName -eq $name -and [string]$_.OuterName -eq $owner
        })
        if ($exports.Count -ne 1 -or [string]$exports[0].ClassName -ne $kind) {
            $actual = ($exports | ForEach-Object { [string]$_.ClassName }) -join ', '
            throw "${owner}::${name}: production requests $kind; shipped package declares $actual"
        }
        Write-Host "PASS ${owner}::${name} is $kind"
    }
} finally { $pkg.Dispose() }

if($EnginePackage) {
    $packages=@()
    try {
        foreach($path in @($Package,$EnginePackage)) {
            $packages += [UELib.UnrealLoader]::LoadPackage((Resolve-Path -LiteralPath $path).Path,
                [UELib.UnrealPackage+GameBuild+BuildName]::MirrorsEdge,[IO.FileAccess]::Read)
        }
        $interaction=Get-Content (Join-Path $PSScriptRoot '../src/combat_interaction.inl') -Raw
        $fields=[regex]::Matches($interaction,'\{"(?<owner>\w+)",\s*"(?<name>\w+)",\s*"(?<type>\w+Property)"\}')
        foreach($field in $fields) {
            $owner=$field.Groups['owner'].Value;$name=$field.Groups['name'].Value;$kind=$field.Groups['type'].Value
            $matches=@($packages | ForEach-Object {$_.Exports} | Where-Object {
                [string]$_.ObjectName -eq $name -and [string]$_.OuterName -eq $owner
            })
            if($matches.Count -ne 1 -or [string]$matches[0].ClassName -ne $kind) {
                throw "${owner}::${name}: expected $kind, got $(($matches | ForEach-Object {[string]$_.ClassName}) -join ', ')"
            }
        }
        Write-Host "PASS all $($fields.Count) interaction/spatial field types, including pickup mesh"
        foreach($function in @(@('TdMove_ZipLine','UpdateViewRotation'),@('TdMove','AbortLookAtTarget'),
            @('PrimitiveComponent','GetRootBodyInstance'),@('RB_BodyInstance','GetUnrealWorldTM'),@('TdWeapon','StartFire'),
            @('TdPlayerPawn','CalcCamera'),@('LensFlareComponent','SetIsActive'))) {
            $matches=@($packages | ForEach-Object {$_.Exports} | Where-Object {
                [string]$_.ObjectName -eq $function[1] -and [string]$_.OuterName -eq $function[0] -and
                [string]$_.ClassName -eq 'Function'
            })
            if($matches.Count -ne 1){throw "Missing unique $($function[0])::$($function[1])"}
        }
        Write-Host 'PASS zipline, pickup rigid-body, weapon firing, camera and lens-flare function identities'
        foreach($field in @(@('CalcCamera','out_Rotation','StructProperty'),@('CalcCamera','ReturnValue','BoolProperty'),
            @('SetIsActive','bInIsActive','BoolProperty'),@('LensFlareComponent','bIsActive','BoolProperty'))) {
            $matches=@($packages | ForEach-Object {$_.Exports} | Where-Object {
                [string]$_.ObjectName -eq $field[1] -and [string]$_.OuterName -eq $field[0]
            })
            if(!$matches.Count -or @($matches | Where-Object {[string]$_.ClassName -ne $field[2]}).Count){
                throw "Missing or incompatible $($field[0])::$($field[1])"
            }
        }
        Write-Host 'PASS camera output and native flare parameter/flag types (byte offsets checked at runtime)'
    } finally {foreach($loaded in $packages){$loaded.Dispose()}}
}
