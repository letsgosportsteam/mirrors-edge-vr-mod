# Builds the VR shim as x86, and optionally installs it beside the game exe.
# Compiles MinHook's C sources alongside the C++ shim (no separate lib step needed).
#
# Everything machine-specific lives in `paths.local.ps1` (gitignored) or in the
# MEVR_OPENXR_SDK / MEVR_GAME_BIN environment variables. This script contains no paths.

param([switch]$Install, [switch]$Package)

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

    # ---- -Package: stage the release zip ----
    #
    # Everything a user needs and nothing else. The two rules this enforces, because both
    # failures are silent and both reach the user before anyone notices:
    #
    #   1. A zip must correspond to a COMMIT. Otherwise the version in the log names a tree
    #      nobody can check out, and the first question a bug report has to answer - "which
    #      build is this" - has no answer.
    #   2. The shipped mevr.ini must actually say Debug=off. The compiled default is ON, so a
    #      failed substitution does not produce a broken build; it produces a working build
    #      with a diagnostic overlay across the player's face.
    if ($Package) {
        if (-not $needsOpenXr) { throw "packaging a build with no OpenXR is not a release" }

        # ---- the version, read from the source ----
        # Same principle as $needsOpenXr above: the DLL logs this string, so parsing it here
        # means the zip cannot be named after a version the binary does not report.
        $verLine = Select-String -LiteralPath (Join-Path $here "d3d9.cpp") `
                                 -Pattern '^\s*#define\s+MEVR_VERSION\s+"([^"]+)"'
        if (-not $verLine) { throw "no MEVR_VERSION #define found in d3d9.cpp" }
        $version = $verLine.Matches[0].Groups[1].Value

        # ---- refuse to package a dirty or unknown tree ----
        $dirty = git -C $root status --porcelain
        if ($LASTEXITCODE -ne 0) { throw "not a git repository - cannot identify this build" }
        if ($dirty) {
            $dirty | ForEach-Object { Write-Host "  $_" }
            throw "working tree is dirty. Commit or stash before packaging - a release zip " +
                  "that does not correspond to a commit cannot be reproduced or bisected."
        }
        $sha = (git -C $root rev-parse --short HEAD).Trim()

        & (Join-Path $root "tools\check-clean.ps1")
        if ($LASTEXITCODE -ne 0) { throw "check-clean failed - not packaging" }

        # ---- the DLL must be 32-bit ----
        # The game is a 32-bit process, so an x64 d3d9.dll is not loaded at all - which looks
        # exactly like the mod doing nothing. Read the PE machine field rather than trusting
        # that vcvarsall was invoked with x86 twenty lines up.
        $dllPath = Join-Path $here "d3d9.dll"
        $fs = [System.IO.File]::OpenRead($dllPath)
        try {
            $br = New-Object System.IO.BinaryReader($fs)
            $fs.Position = 0x3C
            $fs.Position = $br.ReadInt32() + 4      # e_lfanew -> COFF header, past "PE\0\0"
            $machine = $br.ReadUInt16()
        } finally { $fs.Dispose() }
        if ($machine -ne 0x014C) {
            throw ("d3d9.dll is not x86 (PE machine 0x{0:X4}, expected 0x014C)" -f $machine)
        }

        $dist  = Join-Path $root "dist"
        $stage = Join-Path $dist "mevr-$version"
        if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
        New-Item -ItemType Directory -Force -Path $stage | Out-Null

        Copy-Item $dllPath $stage
        Copy-Item (Join-Path $binDir "openxr_loader.dll") $stage
        Copy-Item (Join-Path $root "LICENSE")                  (Join-Path $stage "LICENSE.txt")
        Copy-Item (Join-Path $root "THIRD-PARTY-NOTICES.txt")   $stage
        Copy-Item (Join-Path $root "packaging\README.txt")      $stage

        # ---- mevr.ini, derived from the example rather than kept as a second copy ----
        #
        # One source of truth for the settings and their documentation, with exactly two
        # deliberate release deltas. Each MUST match, or the build stops: a silently skipped
        # substitution here ships the wrong default, and nothing downstream would catch it.
        $iniText = Get-Content (Join-Path $root "mevr.ini.example") -Raw
        $edits = @(
            @{ What = "the rename instruction";
               Rx   = '(?m)^; Rename this file to  mevr\.ini  and leave it beside d3d9\.dll.*$';
               To   = '; This IS mevr.ini. Keep it beside d3d9.dll in the game''s Binaries folder.' }
            @{ What = "Debug defaulted off for release";
               Rx   = '(?m)^Debug = on\s*$';
               To   = 'Debug = off' }
        )
        foreach ($e in $edits) {
            if ($iniText -notmatch $e.Rx) {
                throw "packaging cannot apply '$($e.What)' - mevr.ini.example no longer matches. " +
                      "Fix the pattern in build.ps1 rather than shipping the file unedited."
            }
            $iniText = [regex]::Replace($iniText, $e.Rx, $e.To)
        }
        # NOT Set-Content -Encoding utf8, which on Windows PowerShell 5.1 writes a BOM. The
        # parser skips spaces and tabs before testing for ';', and a BOM is neither - so the
        # file's own first comment would be reported as a rejected line in every release, and
        # a setting on line 1 would be dropped outright. LoadSettings tolerates a BOM now; this
        # side simply never produces one.
        [System.IO.File]::WriteAllText((Join-Path $stage "mevr.ini"), $iniText,
                                       (New-Object System.Text.UTF8Encoding($false)))

        $zip = Join-Path $dist "mevr-$version.zip"
        if (Test-Path $zip) { Remove-Item $zip -Force }
        Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zip

        # The PDB is a SEPARATE release asset, never inside the zip. It is wanted only to turn
        # a crash address in somebody's log into a line number, and putting it in the zip
        # invites users to copy it into Binaries where it does nothing.
        Copy-Item (Join-Path $here "d3d9.pdb") (Join-Path $dist "d3d9-$version.pdb") -Force

        Write-Host ""
        Write-Host "Packaged $version ($sha)" -ForegroundColor Green
        Write-Host "  $zip"
        Write-Host "  $(Join-Path $dist "d3d9-$version.pdb")  (separate release asset, not in the zip)"
        Write-Host ""
        Write-Host "  git tag -a v$version -m ""pre-alpha"" && git push origin v$version"
    }
} finally {
    Pop-Location
}
