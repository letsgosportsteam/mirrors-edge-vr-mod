$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $root '.analysis/xr-diagnostics-tests'
New-Item -ItemType Directory -Force $out | Out-Null
$OpenXrSdk = $env:MEVR_OPENXR_SDK
if (-not $OpenXrSdk) { . (Join-Path $root 'src/paths.local.ps1') }
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vc = Join-Path $vs 'VC/Auxiliary/Build/vcvarsall.bat'
$source = Join-Path $PSScriptRoot 'test-xr-diagnostics.cpp'
$exe = Join-Path $out 'test-xr-diagnostics.exe'
$inc = Join-Path $OpenXrSdk 'include'
$lib = Join-Path $OpenXrSdk 'native/Win32/release/lib'
$mh = Join-Path $root 'third_party/minhook'
$mhSources = @('src/buffer.c','src/hook.c','src/trampoline.c','src/hde/hde32.c') | ForEach-Object { '"' + (Join-Path $mh $_) + '"' }
Push-Location $out
try {
    cmd /c ('"' + $vc + '" x86 >nul && cl /nologo /EHsc /W4 /MD /std:c++17 /I"' + $inc + '" /I"' + (Join-Path $mh 'include') + '" /Fe:"' + $exe + '" "' + $source + '" ' + ($mhSources -join ' ') + ' /link /LIBPATH:"' + $lib + '" openxr_loader.lib user32.lib advapi32.lib')
    if ($LASTEXITCODE -ne 0) { throw 'OpenXR diagnostic test compile failed' }
    Copy-Item (Join-Path $OpenXrSdk 'native/Win32/release/bin/openxr_loader.dll') $out -Force
    & $exe $out
    if ($LASTEXITCODE -ne 0) { throw 'OpenXR diagnostic tests failed' }
} finally { Pop-Location }
