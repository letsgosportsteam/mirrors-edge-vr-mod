# Builds uedecompress3 as x64 - it is an analysis tool, not injected into the game, so it is
# not bound by the mod's 32-bit requirement and benefits from the larger address space when
# expanding a 30 MB package.

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvarsall.bat"

Push-Location $here
try {
    # NB no `2>&1` - vcvarsall writes a harmless line to stderr, and redirecting a native
    # command's stderr in PowerShell wraps each line in an ErrorRecord, which
    # $ErrorActionPreference = "Stop" then treats as fatal.
    $cmd = "`"$vcvars`" x64 && cl /nologo /EHsc /W4 /O2 /std:c++17 " +
           "/Fe:uedecompress3.exe uedecompress3.cpp"
    cmd /c $cmd
    if ($LASTEXITCODE -ne 0) { throw "build failed (exit $LASTEXITCODE)" }
    Write-Host ""
    Write-Host "Built: uedecompress3.exe"
} finally {
    Pop-Location
}
