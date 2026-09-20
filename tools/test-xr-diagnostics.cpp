// Exercise production diagnostics without a game/headset or registry changes.
#include <windows.h>
#include <openxr/openxr.h>
#include <cstdio>
#include <cstdarg>
#include <string>
#include <vector>
#include <algorithm>
#include <cassert>
#include <thread>
#include "../third_party/minhook/include/MinHook.h"
static std::string output;
static void Log(_Printf_format_string_ const char* format, ...) {
    char line[1024]; va_list args; va_start(args, format);
    _vsnprintf_s(line, _TRUNCATE, format, args); va_end(args);
    output += line; output += '\n';
}
static bool detailed = true;
#include "../src/xr_diagnostics.inl"
static bool XrDetailedLogging() { return detailed; }
static bool Has(const char* s) { return output.find(s) != std::string::npos; }
static bool Parse(const char* text, XrManifestFields& fields) {
    fields = {}; std::string json(text);
    return XrManifestReader(json, "api_layer", fields).Read();
}
static void Write(const std::wstring& path, const void* data, DWORD size) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    assert(file != INVALID_HANDLE_VALUE); DWORD got = 0;
    assert(WriteFile(file, data, size, &got, nullptr) && got == size); CloseHandle(file);
}
static void EmitDebug(const char* message) {
    // Resolve the real exported entry point, including when testing patched functions.
    auto emit = (decltype(&OutputDebugStringA))GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "OutputDebugStringA");
    emit(message);
}
int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    _set_error_mode(_OUT_TO_STDERR);
    assert(argc == 2); std::wstring dir = argv[1];
    assert(!strcmp(XrResultName((XrResult)-32), "XR_ERROR_FILE_ACCESS_ERROR"));
    assert(!strcmp(XrResultName((XrResult)-51), "XR_ERROR_RUNTIME_UNAVAILABLE"));
    assert(!XrLogResult("fixture", (XrResult)-123456)); assert(Has("XR_RESULT_UNKNOWN (-123456)"));
    XrManifestFields f;
    assert(Parse(R"({"api_layer":{"name":"test","library_path":".\\folder\\layer.dll","disable_environment":"TEST_OFF"}})", f));
    assert(f.library == ".\\folder\\layer.dll" && f.disable == "TEST_OFF");
    assert(Parse("{\"ignored\":[{\"library_path\":\"wrong\"},true,null,-1.25e+2],\"api_layer\":{\"library_path\":\"./\\u6e2c\\u8a66/\\uD83D\\uDE80.dll\"}}", f));
    assert(!XrWide(f.library).empty());
    const char* invalid[] = {
        R"({"nested":{"api_layer":{"library_path":"wrong"}}})",
        R"({"api_layer":{"library_path":"one","library_path":"two"}})",
        R"({"api_layer":{"library_path":"one"},"api_layer":{"library_path":"two"}})",
        R"({"api_layer":{"library_path":"unterminated})",
        R"({"api_layer":{"library_path":"\uD800"}})",
        R"({"api_layer":{"library_path":"\u0000bad"}})",
        R"({"api_layer":{"library_path":42}})",
        R"({"api_layer":{"library_path":"x"},"bad":01})",
        R"({"api_layer":{"library_path":"x"},"bad":1e})",
        R"({"api_layer":{"library_path":"x"},})",
        R"({"api_layer":{"library_path":"x"}} trailing)"
    };
    for (const char* s : invalid) assert(!Parse(s, f));
    std::string deep(20, '['); deep += "null"; deep += std::string(20, ']');
    deep = "{\"api_layer\":{\"library_path\":\"x\"},\"deep\":" + deep + "}";
    assert(!Parse(deep.c_str(), f));
    output.clear(); XrLogText("fixture", "first\n[xr] forged\r\t");
    assert(Has("first\\x0A[xr] forged\\x0D\\x09")); assert(!Has("\n[xr] forged"));
    output.clear(); XrLogText("long", std::string(10000, 'a')); assert(Has("<truncated>"));

    // Inspect raw PE headers without executing any fixture DLL.
    BYTE pe[sizeof(IMAGE_DOS_HEADER)+sizeof(DWORD)+sizeof(IMAGE_FILE_HEADER)]{};
    IMAGE_DOS_HEADER dos{}; dos.e_magic = IMAGE_DOS_SIGNATURE; dos.e_lfanew = sizeof(dos);
    DWORD signature = IMAGE_NT_SIGNATURE; IMAGE_FILE_HEADER header{}; header.Machine = IMAGE_FILE_MACHINE_AMD64;
    memcpy(pe, &dos, sizeof(dos)); memcpy(pe+sizeof(dos), &signature, sizeof(signature));
    memcpy(pe+sizeof(dos)+sizeof(signature), &header, sizeof(header));
    std::wstring dll = dir+L"\\fixture.dll"; Write(dll, pe, sizeof(pe));
    output.clear(); XrFileEvidence(dll.c_str(), true); assert(Has("x64 - incompatible"));
    header.Machine = IMAGE_FILE_MACHINE_I386;
    memcpy(pe+sizeof(dos)+sizeof(signature), &header, sizeof(header)); Write(dll, pe, sizeof(pe));
    output.clear(); XrFileEvidence(dll.c_str(), true); assert(Has("(x86)"));
    std::wstring json = dir+L"\\fixture.json";
    const char* manifest = R"({"api_layer":{"name":"fixture","library_path":"./fixture.dll","disable_environment":"MEVR_TEST_LAYER_OFF"}})";
    Write(json, manifest, (DWORD)strlen(manifest)); SetEnvironmentVariableW(L"MEVR_TEST_LAYER_OFF", L"1");
    output.clear(); XrInspectManifest(json, true); assert(Has("environment gate: set") && Has("(x86)"));
    SetEnvironmentVariableW(L"MEVR_TEST_LAYER_OFF", nullptr);
    assert(DeleteFileW(dll.c_str()));
    output.clear(); XrInspectManifest(json, true); assert(Has("file open failed: Win32=2"));
    Write(dll, "bad", 3); output.clear(); XrFileEvidence(dll.c_str(), true); assert(Has("invalid or incomplete PE"));
    std::string large(65537, ' '); Write(json, large.data(), (DWORD)large.size());
    output.clear(); XrInspectManifest(json, true); assert(Has("exceeds 64 KiB"));
    assert(DeleteFileW(json.c_str())); assert(DeleteFileW(dll.c_str()));

    // Explicit nonexistent runtime fails before loading any installed API layer/runtime.
    // Overrides are child-process-local and never touch the machine's OpenXR registration.
    std::wstring missing = dir+L"\\nonexistent-runtime.json";
    assert(GetFileAttributesW(missing.c_str()) == INVALID_FILE_ATTRIBUTES);
    assert(SetEnvironmentVariableW(L"XR_RUNTIME_JSON", missing.c_str()));
    assert(SetEnvironmentVariableW(L"XR_LOADER_DEBUG", nullptr));
    assert(SetEnvironmentVariableW(L"XR_API_LAYER_PATH", nullptr));
    assert(SetEnvironmentVariableW(L"XR_ENABLE_API_LAYERS", nullptr));
    detailed = false; output.clear();
    { XrStartupDiagnostics scope; EmitDebug("capture-disabled-fixture"); }
    assert(!Has("capture-disabled-fixture") && Has("Detailed Logging"));
    detailed = true; output.clear();
    {
        XrStartupDiagnostics scope;
        assert(Has("capture A=on W=on"));
        EmitDebug("loader-style failure: fixture.dll Win32=193");
        OutputDebugStringW(L"wide-fixture \u6e2c\u8a66");
        std::thread other([] { EmitDebug("other-thread-fixture"); }); other.join();
        assert(Has("fixture.dll Win32=193") && Has("wide-fixture"));
        assert(!Has("other-thread-fixture"));
        assert(Has("DLL load capture W=on ExW=on"));
        HMODULE system = LoadLibraryW(L"kernel32.dll");
        assert(system && Has("DLL load succeeded") && Has("loaded DLL path")); FreeLibrary(system);
        assert(!LoadLibraryW((dir+L"\\missing-test-library.dll").c_str()));
        DWORD loadError = GetLastError(); assert(loadError == ERROR_MOD_NOT_FOUND);
        assert(Has("missing-test-library.dll") && Has("DLL load FAILED: Win32=126"));
        Write(dll, "bad", 3);
        assert(!LoadLibraryExW(dll.c_str(), nullptr, 0));
        loadError = GetLastError(); assert(loadError == ERROR_BAD_EXE_FORMAT);
        assert(Has("DLL load FAILED: Win32=193")); assert(DeleteFileW(dll.c_str()));
        XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
        strcpy_s(info.applicationInfo.applicationName, "MEVR diagnostic regression");
        info.applicationInfo.apiVersion = XR_MAKE_VERSION(1,0,34); XrInstance instance = XR_NULL_HANDLE;
        XrResult result = xrCreateInstance(&info, &instance);
        assert(XR_FAILED(result)); XrLogResult("xrCreateInstance (missing-runtime fixture)", result);
        assert(Has("Failed loading runtime information")); // Actual loader message, before an instance exists.
    }
    size_t before = output.size(); EmitDebug("after-scope-fixture"); OutputDebugStringW(L"after-scope-wide");
    assert(output.size() == before && !g_xrDiagnosticCapture);
    assert(!LoadLibraryW((dir+L"\\after-scope-missing.dll").c_str()));
    assert(output.size() == before);
    assert(Has("startup capture ended"));
    std::wstring log = dir+L"\\diagnostics-test.log"; Write(log, output.data(), (DWORD)output.size());
    puts("OpenXR diagnostics: JSON/Unicode/scope, PE, missing files, logging gate, DLL load results/errors, real loader capture and cleanup passed");
}
