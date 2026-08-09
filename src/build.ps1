# Builds the VR shim as x86, and optionally installs it beside the game exe.
# Compiles MinHook's C sources alongside the C++ shim (no separate lib step needed).
#
# Everything machine-specific lives in `paths.local.ps1` (gitignored) or in the
# MEVR_OPENXR_SDK / MEVR_GAME_BIN environment variables. This script contains no paths.

param([switch]$Install)

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Resolve-Path (Join-Path $here "..")

# ---- machine-local paths ----
# Env vars win, so a second machine or a CI runner needs no file. Otherwise read
# paths.local.ps1. Failing loudly with the fix in the message beats a confusing
# compiler error forty lines later.
$OpenXrSdk = $env:MEVR_OPENXR_SDK
$GameBin   = $env:MEVR_GAME_BIN
$localCfg  = Join-Path $here "paths.local.ps1"
if ((-not $OpenXrSdk) -or (-not $GameBin)) {
    if (Test-Path $localCfg) {
        . $localCfg
        if (-not $OpenXrSdk) { $OpenXrSdk = $script:OpenXrSdk }
        if (-not $GameBin)   { $GameBin   = $script:GameBin }
    }
}

$mh = Join-Path $root "third_party\minhook"
if (-not (Test-Path $mh)) { throw "missing: $mh" }

# ---- does this build actually need OpenXR? ----
#
# The source declares its own dependency, so read it rather than carry a flag that can
# disagree with reality. This is not a convenience: an unused openxr_loader.lib still puts
# openxr_loader.dll in our import table, and a d3d9.dll that cannot resolve its imports is
# silently not loaded by the game at all - which looks exactly like "the game does not use
# D3D9", the precise question the early rungs exist to answer.
$needsOpenXr = [bool](Select-String -LiteralPath (Join-Path $here "d3d9.cpp") `
                                    -Pattern '#include\s*[<"]openxr' -Quiet)

# MinHook is likewise only linked once something detours. Rung 0 forwards and hooks nothing.
$needsMinHook = [bool](Select-String -LiteralPath (Join-Path $here "d3d9.cpp") `
                                     -Pattern '#include\s*[<"]MinHook\.h' -Quiet)

$inc = $null; $libDir = $null; $binDir = $null
if ($needsOpenXr) {
    if (-not $OpenXrSdk) {
        throw "d3d9.cpp includes an OpenXR header but no SDK path is set. Copy " +
              "src\paths.local.ps1.example to src\paths.local.ps1 and fill it in, " +
              "or set `$env:MEVR_OPENXR_SDK."
    }
    $inc    = Join-Path $OpenXrSdk "include"
    $libDir = Join-Path $OpenXrSdk "native\Win32\release\lib"
    $binDir = Join-Path $OpenXrSdk "native\Win32\release\bin"
    foreach ($p in @($inc, $libDir, $binDir)) { if (-not (Test-Path $p)) { throw "missing: $p" } }
}
Write-Host ("OpenXR: {0}   MinHook: {1}" -f
            $(if ($needsOpenXr) { "linked" } else { "not needed by this source" }),
            $(if ($needsMinHook) { "compiled in" } else { "not needed by this source" }))

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvarsall.bat"

Push-Location $here
try {
    $mhSrc = ""
    if ($needsMinHook) {
        $mhSrc = @(
            "`"$mh\src\buffer.c`"",
            "`"$mh\src\hook.c`"",
            "`"$mh\src\trampoline.c`"",
            "`"$mh\src\hde\hde32.c`""
        ) -join " "
    }
    $incArgs = "/I`"$mh\include`""
    if ($needsOpenXr) { $incArgs = "/I`"$inc`" $incArgs" }
    # d3d11/dxgi come with OpenXR here: the XR_KHR_D3D11 binding needs a D3D11 device created
    # on the adapter the runtime names, so they are never wanted independently of it.
    $xrLib = if ($needsOpenXr) { "/LIBPATH:`"$libDir`" openxr_loader.lib d3d11.lib dxgi.lib" } else { "" }

    # ---- static analysis pass on d3d9.cpp, before the real build ----
    #
    # Inherited from the Singularity project, where run 27 lost four runs to a printf call
    # with nine conversions and ten arguments: %s consumed an int, which is harmless while
    # that int is 0 and an access violation inside a held critical section the moment it is
    # not. It presented as an unexplained startup hang.
    #
    # /W1../W4 do not catch it. Log() is annotated _Printf_format_string_, and /analyze reads
    # that annotation and reports C6067. Verified there: without /analyze the mistake compiles
    # silently even at /W4; with it, it is an error here.
    #
    # Analysis-only, on our source alone - MinHook's C is third-party and not our business.
    # NB no `2>&1` here. vcvarsall writes a harmless "vswhere.exe is not recognized" line to
    # stderr, and redirecting a native command's stderr inside PowerShell wraps each line in an
    # ErrorRecord - which $ErrorActionPreference = "Stop" then treats as fatal. cl writes its
    # warnings to stdout, so there is nothing to gain from the redirect anyway.
    $an = "`"$vcvars`" x86 >nul && cl /nologo /EHsc /W3 /w34505 /MD /std:c++17 /analyze:only " +
          "$incArgs /c d3d9.cpp"
    $anOut = cmd /c $an
    $anOut | Write-Host
    # Format-string mistakes are the ones that have actually cost time, so they fail the
    # build. The rest of /analyze's output is advisory.
    # C6270/C6271: a literal '%' left unescaped in a Log() string. "86-89% enriched" becomes
    # the float specifier %e with no argument, which reads garbage off the stack. If a warning
    # says the format string and the arguments disagree, it is fatal - there is no benign
    # version of that.
    $fatal = $anOut | Select-String -Pattern "warning C6067|warning C6063|warning C6064|warning C6066|warning C6270|warning C6271|warning C6272|warning C6273|warning C6328"
    if ($fatal) { throw "static analysis found a format-string defect - fix it before building" }

    # C4505 - a static function defined and never called. Normally level 4 and therefore
    # invisible here, promoted to level 3 and made fatal because it has already cost a run:
    # a whole subsystem was written, compiled and shipped without its call site, and the build
    # was perfectly happy. In this file an uncalled static is a wiring mistake, not a spare.
    $unused = $anOut | Select-String -Pattern "warning C4505"
    if ($unused) {
        $unused | ForEach-Object { Write-Host $_.Line }
        throw "a static function is defined but never called - almost certainly a missing call site"
    }

    $cmd = "`"$vcvars`" x86 && cl /nologo /LD /EHsc /W3 /w34505 /Zi /MD /std:c++17 " +
           "$incArgs " +
           "/Fe:d3d9.dll d3d9.cpp $mhSrc " +
           "/link /DEF:d3d9.def $xrLib user32.lib shell32.lib psapi.lib"
    cmd /c $cmd
    if ($LASTEXITCODE -ne 0) { throw "build failed (exit $LASTEXITCODE)" }
    Write-Host ""
    Write-Host "Built: d3d9.dll"

    if ($Install) {
        if (-not $GameBin) {
            throw "Game Binaries path not set. Fill in `$GameBin in src\paths.local.ps1, " +
                  "or set `$env:MEVR_GAME_BIN."
        }
        if (-not (Test-Path $GameBin)) { throw "game Binaries folder not found: $GameBin" }
        Copy-Item (Join-Path $here "d3d9.dll") $GameBin -Force
        if ($needsOpenXr) {
            Copy-Item (Join-Path $binDir "openxr_loader.dll") $GameBin -Force
            Write-Host "Installed d3d9.dll + openxr_loader.dll to the configured game folder."
        } else {
            Write-Host "Installed d3d9.dll to the configured game folder."
        }

        # The example goes too, so the dev install matches what a release looks like: the mod
        # reads mevr.ini from beside the DLL, and that is only testable if this folder is laid
        # out the way a user's would be.
        #
        # The EXAMPLE, never mevr.ini itself - overwriting somebody's edited settings on every
        # build is exactly the kind of quiet loss this project keeps paying for elsewhere.
        $iniExample = Join-Path $root "mevr.ini.example"
        if (Test-Path $iniExample) {
            Copy-Item $iniExample $GameBin -Force
            $live = Join-Path $GameBin "mevr.ini"
            if (-not (Test-Path $live)) {
                Write-Host "Copied mevr.ini.example. Rename it to mevr.ini there to use it."
            }
        }
    }
} finally {
    Pop-Location
}
