$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $root '.analysis/combat-tests'
New-Item -ItemType Directory -Force $out | Out-Null
$source = Get-Content (Join-Path $root 'src/d3d9.cpp') -Raw
# Compile the production quaternion conversion, not a parallel implementation.
$functions = foreach ($name in @('RotateByQuaternion', 'MultiplyQuaternion', 'GripQuaternionToUERotator', 'LoadGunCalibrationAngle', 'LoadGunHandValue', 'LoadGunCalibration')) {
    $match = [regex]::Match($source, "(?ms)^static [^\r\n]+ $name\([^;{]*\)\s*\{.*?^\}")
    if (-not $match.Success) { throw "Missing production function $name" }
    $match.Value
}
$prefix = @'
#include <cmath>
#include <cstdint>
#include <cassert>
#include <cstdio>
#include <cwchar>
#include <Windows.h>
#include <initializer_list>
#undef near
struct XrQuaternionf { float x,y,z,w; };
struct MEVR_Vec3 { float x,y,z; };
static int g_gunWristDownDeg[2]{};
static int g_gunWristRightDeg[2]{};
static int g_gunPositionMm[2][3]{};
static wchar_t g_gunCalibrationPath[MAX_PATH]{};
static void Log(const char*, ...) {}
static bool armed = false;
static int holdingHand = 1;
static int CombatHoldingHand() { return holdingHand; }
static bool PistolCalibrationActive() { return armed; }
'@
$tests = @'
static bool near(float a, float b) { return fabsf(a-b)<0.0001f; }
int wmain(int argc, wchar_t** argv) {
    assert(argc==3);
    wcscpy_s(g_gunCalibrationPath,argv[2]);
    if (wcscmp(argv[1],L"read")==0) {
        LoadGunCalibration();
        assert(g_gunWristDownDeg[0]==25 && g_gunWristDownDeg[1]==40);
        assert(g_gunWristRightDeg[0]==5 && g_gunWristRightDeg[1]==-15);
        assert(g_gunPositionMm[0][1]==35 && g_gunPositionMm[1][1]==0);
        for(const auto* invalid:{L"201",L"-201",L"junk",L"10oops",L"9999999999999"}) {
            assert(WritePrivateProfileStringW(L"PistolLeft",L"RightMm",invalid,g_gunCalibrationPath));
            assert(LoadGunHandValue(0,L"RightMm",0,200)==0);
        }
        puts("gun calibration: separate-process per-hand reload and invalid values passed");
        return 0;
    }
    int cal[2][3]{}; int32_t r[3]{}; XrQuaternionf q{};
    g_gunWristDownDeg[0]=g_gunWristDownDeg[1]=30; armed=true;
    assert(GripQuaternionToUERotator({0,0,0,1},false,cal,r,&q));
    auto f=RotateByQuaternion(q,{1,0,0});
    assert(near(f.x,sqrtf(0.75f)) && near(f.y,0) && near(f.z,-0.5f));
    assert(r[0]<0); // positive DOWN produces downward pitch
    // Roll the controller 90 degrees: pinky/down follows the hand, not world gravity.
    const float s=sqrtf(0.5f);
    assert(GripQuaternionToUERotator({s,0,0,s},false,cal,r,&q));
    f=RotateByQuaternion(q,{1,0,0});
    assert(near(f.x,sqrtf(0.75f)) && near(f.y,0.5f) && near(f.z,0));
    // Combined yaw/down, both horizontal directions and controller-local rotation.
    for (int yaw : {-30,30}) {
        g_gunWristRightDeg[0]=g_gunWristRightDeg[1]=yaw;
        assert(GripQuaternionToUERotator({0,0,0,1},false,cal,r,&q));
        f=RotateByQuaternion(q,{1,0,0});
        const float y=(yaw>0 ? 1.0f : -1.0f)*sqrtf(0.75f)*0.5f;
        assert(near(f.x,0.75f) && near(f.y,y) && near(f.z,-0.5f));
        assert(GripQuaternionToUERotator({s,0,0,s},false,cal,r,&q));
        f=RotateByQuaternion(q,{1,0,0});
        assert(near(f.x,0.75f) && near(f.y,0.5f) && near(f.z,y));
    }
    // Moving the pistol to the left moves its trim too; the free right hand stays untrimmed.
    holdingHand=0;
    assert(GripQuaternionToUERotator({0,0,0,1},true,cal,r,&q));
    f=RotateByQuaternion(q,{1,0,0});
    assert(near(f.x,0.75f) && near(f.y,sqrtf(0.75f)*0.5f) && near(f.z,-0.5f));
    assert(GripQuaternionToUERotator({0,0,0,1},false,cal,r,&q));
    f=RotateByQuaternion(q,{1,0,0});assert(near(f.x,1)&&near(f.y,0)&&near(f.z,0));
    holdingHand=1;
    // Free left hand and unarmed right hand retain the original calibrated rest frame.
    cal[0][0]=-30; cal[1][0]=150;
    assert(GripQuaternionToUERotator({0,0,0,1},true,cal,r,&q));
    f=RotateByQuaternion(q,{1,0,0}); assert(near(f.z,-0.5f));
    armed=false;
    assert(GripQuaternionToUERotator({0,0,0,1},false,cal,r,&q));
    f=RotateByQuaternion(q,{1,0,0}); assert(near(f.z,0.5f));
    assert(!GripQuaternionToUERotator({0,0,0,0},false,cal,r,&q));
    // Legacy defaults migrate to both hands without changing the old INI section.
    assert(WritePrivateProfileStringW(L"PistolLeft",nullptr,nullptr,g_gunCalibrationPath));
    assert(WritePrivateProfileStringW(L"PistolRight",nullptr,nullptr,g_gunCalibrationPath));
    assert(WritePrivateProfileStringW(L"Pistol",L"WristDownDegrees",L"40",g_gunCalibrationPath));
    assert(WritePrivateProfileStringW(L"Pistol",L"WristRightDegrees",L"-10",g_gunCalibrationPath));
    LoadGunCalibration();
    assert(g_gunWristDownDeg[0]==40 && g_gunWristDownDeg[1]==40);
    assert(g_gunWristRightDeg[0]==-10 && g_gunWristRightDeg[1]==-10);
    g_gunWristDownDeg[0]=25;g_gunWristRightDeg[0]=5;g_gunPositionMm[0][1]=35;
    g_gunWristRightDeg[1]=-15;
    // Legacy fixture: new saves and their error paths are tested against the actual
    // mevr.ini writer by test-vr-menu.cpp. Keep testing migration from both old sections.
    assert(WritePrivateProfileStringW(L"PistolLeft",L"WristDownDegrees",L"25",g_gunCalibrationPath));
    assert(WritePrivateProfileStringW(L"PistolLeft",L"WristRightDegrees",L"5",g_gunCalibrationPath));
    assert(WritePrivateProfileStringW(L"PistolLeft",L"RightMm",L"35",g_gunCalibrationPath));
    assert(WritePrivateProfileStringW(L"PistolRight",L"WristRightDegrees",L"-15",g_gunCalibrationPath));
    assert(LoadGunHandValue(0,L"RightMm",0,200)==35);
    assert(LoadGunHandValue(1,L"RightMm",0,200)==0);
    assert(LoadGunCalibrationAngle(L"WristRightDegrees")==-10);
    puts("gun calibration: local down/yaw, rolled hand, isolation and legacy migration passed");
}
'@
$cpp = Join-Path $out 'gun-calibration.cpp'
($prefix + "`n" + ($functions -join "`n") + "`n" + $tests) | Set-Content $cpp
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vc = Join-Path $vs 'VC/Auxiliary/Build/vcvarsall.bat'
$exe = Join-Path $out 'gun-calibration.exe'
$obj = Join-Path $out 'gun-calibration.obj'
cmd /c ('"'+$vc+'" x86 >nul && cl /nologo /EHsc /W4 /std:c++17 /Fe:"'+$exe+'" /Fo:"'+$obj+'" "'+$cpp+'"')
if ($LASTEXITCODE -ne 0) { throw 'gun calibration compile failed' }
$ini = Join-Path $out 'gun-calibration-test.ini'
& $exe write $ini
if ($LASTEXITCODE -ne 0) { throw 'gun calibration tests failed' }
& $exe read $ini
if ($LASTEXITCODE -ne 0) { throw 'gun calibration persistence tests failed' }
