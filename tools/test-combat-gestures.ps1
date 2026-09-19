$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $root '.analysis/combat-tests'
New-Item -ItemType Directory -Force $out | Out-Null
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vc = Join-Path $vs 'VC/Auxiliary/Build/vcvarsall.bat'
$source = Join-Path $PSScriptRoot 'test-combat-gestures.cpp'
$exe = Join-Path $out 'test-combat.exe'
$obj = Join-Path $out 'test-combat.obj'
$command = '"' + $vc + '" x86 >nul && cl /nologo /EHsc /W4 /std:c++17 /Fe:"' + $exe + '" /Fo:"' + $obj + '" "' + $source + '"'
cmd /c $command
if ($LASTEXITCODE -ne 0) { throw 'combat test compile failed' }
& $exe
if ($LASTEXITCODE -ne 0) { throw 'combat tests failed' }
