$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $root '.analysis/vr-controls-tests'
New-Item -ItemType Directory -Force $out | Out-Null
. (Join-Path $root 'src/paths.local.ps1')
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vc = Join-Path $vs 'VC/Auxiliary/Build/vcvarsall.bat'
$source = Join-Path $PSScriptRoot 'test-vr-controls.cpp'
$exe = Join-Path $out 'test-vr-controls.exe'
$obj = Join-Path $out 'test-vr-controls.obj'
$inc = Join-Path $OpenXrSdk 'include'
$lib = Join-Path $OpenXrSdk 'native/Win32/release/lib'
$objects = @('buffer.obj','hook.obj','trampoline.obj','hde32.obj') | ForEach-Object { '"' + (Join-Path $root "src/$_") + '"' }
$command = '"' + $vc + '" x86 >nul && cl /nologo /EHsc /W3 /MD /std:c++17 /I"' + $inc + '" /I"' + (Join-Path $root 'third_party/minhook/include') + '" /Fe:"' + $exe + '" /Fo:"' + $obj + '" "' + $source + '" ' + ($objects -join ' ') + ' /link /LIBPATH:"' + $lib + '" openxr_loader.lib d3d11.lib dxgi.lib user32.lib shell32.lib psapi.lib advapi32.lib'
cmd /c $command
if ($LASTEXITCODE -ne 0) { throw 'VR controls test compile failed' }
Copy-Item (Join-Path $OpenXrSdk 'native/Win32/release/bin/openxr_loader.dll') $out -Force
& $exe $out
if ($LASTEXITCODE -ne 0) { throw 'VR controls tests failed' }
