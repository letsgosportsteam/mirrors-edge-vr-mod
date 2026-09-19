# Validate the install archive without extracting or launching anything.
param([Parameter(Mandatory=$true)][string]$ZipPath)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [System.IO.Compression.ZipFile]::OpenRead((Resolve-Path -LiteralPath $ZipPath).Path)
try {
    $expected = @('d3d9.dll', 'mevr.ini', 'openxr_loader.dll')
    $actual = @($archive.Entries | ForEach-Object { $_.FullName } | Sort-Object)
    if ($actual.Count -ne 3 -or ($actual -join '|') -cne ($expected -join '|')) {
        throw "Install ZIP must contain exactly three top-level files: $($expected -join ', ')"
    }
    foreach ($dll in @('d3d9.dll', 'openxr_loader.dll')) {
        $stream = $archive.GetEntry($dll).Open()
        $bytes = New-Object System.IO.MemoryStream
        try { $stream.CopyTo($bytes); $data = $bytes.ToArray() } finally { $stream.Dispose(); $bytes.Dispose() }
        if ($data.Length -lt 64 -or [BitConverter]::ToUInt16($data, 0) -ne 0x5A4D) { throw "$dll is not a PE binary" }
        $pe = [BitConverter]::ToInt32($data, 0x3C)
        if ($pe -lt 0 -or $pe -gt ($data.Length - 6) -or
            [BitConverter]::ToUInt32($data, $pe) -ne 0x4550 -or
            [BitConverter]::ToUInt16($data, $pe + 4) -ne 0x014C) { throw "$dll must be an x86 PE binary" }
    }
    $reader = New-Object System.IO.StreamReader($archive.GetEntry('mevr.ini').Open())
    try { $ini = $reader.ReadToEnd() } finally { $reader.Dispose() }
    foreach ($setting in @('Debug = off', 'MotionHands = on', 'ArmSwing = on', 'Resolution = auto', 'GripToGrip = on', 'StickJumpTurn = on')) {
        $key = ($setting -split '=')[0].Trim()
        $matches = [regex]::Matches($ini, '(?im)^' + [regex]::Escape($key) + '\s*=.*$')
        if ($matches.Count -ne 1 -or $matches[0].Value.Trim() -cne $setting) { throw "Wrong or duplicate shipped default: $setting" }
    }
    if ($ini -match '(?im)^; Rename this file') { throw 'Shipped INI still asks the user to rename it' }
    foreach ($notice in @('LICENSE', 'THIRD-PARTY-NOTICES.txt', 'third_party/minhook/LICENSE.txt')) {
        $text = [System.IO.File]::ReadAllText((Join-Path $root $notice))
        $commented = (($text -split '\r?\n' | ForEach-Object { "; $_" }) -join "`n")
        if (-not $ini.Replace("`r`n", "`n").Contains($commented)) { throw "Missing full notice: $notice" }
    }
    Write-Host 'PASS install ZIP: exactly three files, x86 DLLs, release defaults, and complete license notices.'
} finally { $archive.Dispose() }
