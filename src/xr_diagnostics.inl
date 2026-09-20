// Startup-only, read-only OpenXR diagnostics. Never called under DllMain's loader lock.
#include <winver.h>
#include <openxr/openxr_reflection.h>
#pragma comment(lib, "version.lib")

static bool XrDetailedLogging();

static const char* XrResultName(XrResult result)
{
    // xrResultToString needs a valid instance, which is exactly what may have failed.
#define XR_DIAG_RESULT(name, value) case value: return #name;
    switch (result) {
        XR_LIST_ENUM_XrResult(XR_DIAG_RESULT)
        default: return "XR_RESULT_UNKNOWN";
    }
#undef XR_DIAG_RESULT
}

static bool XrLogResult(const char* operation, XrResult result)
{
    Log("[xr] %s -> %s (%d)", operation, XrResultName(result), (int)result);
    return XR_SUCCEEDED(result);
}

static std::string XrUtf8(const std::wstring& value)
{
    if (value.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), (int)value.size(), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return "<invalid Unicode>";
    std::string result((size_t)size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), (int)value.size(), &result[0], size, nullptr, nullptr);
    return result;
}

static std::wstring XrWide(const std::string& value)
{
    if (value.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), (int)value.size(), nullptr, 0);
    if (size <= 0) return {};
    std::wstring result((size_t)size, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), (int)value.size(), &result[0], size);
    return result;
}

// Log() has a 1 KiB line buffer. Split long paths/messages and escape control characters
// so a diagnostic supplied by another component cannot forge our own log prefixes.
static void XrLogText(const char* label, const std::string& value)
{
    std::string clean;
    for (unsigned char c : value) {
        if (clean.size() >= 8192) { clean += "<truncated>"; break; }
        if (c < 32 || c == 127) {
            char escaped[5]; sprintf_s(escaped, "\\x%02X", (unsigned)c); clean += escaped;
        } else clean += (char)c;
    }
    if (clean.empty()) { Log("[xr-diag] %s: <empty>", label); return; }
    for (size_t i = 0; i < clean.size(); i += 700)
        Log("[xr-diag] %s%s: %s", label, i ? " (continued)" : "", clean.substr(i, 700).c_str());
}

static std::wstring XrEnvironment(const wchar_t* name)
{
    std::vector<wchar_t> value(32768);
    DWORD n = GetEnvironmentVariableW(name, value.data(), (DWORD)value.size());
    return n && n < value.size() ? std::wstring(value.data(), n) : std::wstring();
}

static void XrFileEvidence(const wchar_t* path, bool binary)
{
    XrLogText(binary ? "DLL" : "manifest", XrUtf8(path));
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        Log("[xr-diag] file open failed: Win32=%lu", GetLastError()); return;
    }
    if (binary) {
        IMAGE_DOS_HEADER dos{}; DWORD got = 0;
        DWORD signature = 0; IMAGE_FILE_HEADER pe{};
        LARGE_INTEGER size{}, offset{};
        bool valid = GetFileSizeEx(file, &size) && ReadFile(file, &dos, sizeof(dos), &got, nullptr) &&
            got == sizeof(dos) && dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew >= (LONG)sizeof(dos);
        offset.QuadPart = dos.e_lfanew;
        valid = valid && offset.QuadPart + sizeof(signature) + sizeof(pe) <= size.QuadPart &&
            SetFilePointerEx(file, offset, nullptr, FILE_BEGIN) &&
            ReadFile(file, &signature, sizeof(signature), &got, nullptr) && got == sizeof(signature) &&
            signature == IMAGE_NT_SIGNATURE && ReadFile(file, &pe, sizeof(pe), &got, nullptr) && got == sizeof(pe);
        if (valid) Log("[xr-diag] readable PE machine=0x%04X (%s)", (unsigned)pe.Machine,
            pe.Machine == IMAGE_FILE_MACHINE_I386 ? "x86" :
            pe.Machine == IMAGE_FILE_MACHINE_AMD64 ? "x64 - incompatible with this x86 process" : "not x86");
        else Log("[xr-diag] readable file; invalid or incomplete PE header");
    } else Log("[xr-diag] manifest is readable");
    CloseHandle(file);
}

// A bounded JSON reader for manifest string fields. It validates structure, escapes and
// scope before using paths; nested/duplicate keys must not impersonate api_layer/runtime.
struct XrManifestFields {
    std::string library, name, enable, disable;
};
class XrManifestReader {
    const char* p; const char* end; const char* section;
    XrManifestFields& fields;
    void Space() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p; }
    bool Hex(unsigned& out) {
        out = 0;
        for (int i = 0; i < 4; ++i) {
            if (p == end) return false;
            char c = *p++; unsigned v = c >= '0' && c <= '9' ? c-'0' :
                c >= 'a' && c <= 'f' ? c-'a'+10 : c >= 'A' && c <= 'F' ? c-'A'+10 : 16;
            if (v == 16) return false;
            out = out * 16 + v;
        }
        return true;
    }
    bool String(std::string& out) {
        if (p == end || *p++ != '"') return false;
        while (p < end) {
            unsigned char c = (unsigned char)*p++;
            if (c == '"') return out.find('\0') == std::string::npos;
            if (c < 32) return false;
            if (c != '\\') { out += (char)c; continue; }
            if (p == end) return false;
            switch (*p++) {
                case '"': out += '"'; break; case '\\': out += '\\'; break; case '/': out += '/'; break;
                case 'b': out += '\b'; break; case 'f': out += '\f'; break;
                case 'n': out += '\n'; break; case 'r': out += '\r'; break; case 't': out += '\t'; break;
                case 'u': {
                    unsigned hi = 0, lo = 0; if (!Hex(hi)) return false;
                    std::wstring text(1, (wchar_t)hi);
                    if (hi >= 0xD800 && hi <= 0xDBFF) {
                        if (end-p < 2 || *p++ != '\\' || *p++ != 'u' || !Hex(lo) || lo < 0xDC00 || lo > 0xDFFF) return false;
                        text += (wchar_t)lo;
                    } else if (hi >= 0xDC00 && hi <= 0xDFFF) return false;
                    out += XrUtf8(text); break;
                }
                default: return false;
            }
        }
        return false;
    }
    bool Value(unsigned depth, bool capture = false) {
        Space(); if (p == end || depth > 16) return false;
        if (*p == '{') {
            ++p; Space(); std::vector<std::string> keys;
            if (p < end && *p == '}') { ++p; return true; }
            for (;;) {
                Space(); std::string key;
                if (!String(key) || std::find(keys.begin(), keys.end(), key) != keys.end()) return false;
                keys.push_back(key); Space(); if (p == end || *p++ != ':') return false; Space();
                std::string* dest = !capture ? nullptr : key == "library_path" ? &fields.library :
                    key == "name" ? &fields.name : key == "enable_environment" ? &fields.enable :
                    key == "disable_environment" ? &fields.disable : nullptr;
                if (dest) { if (!String(*dest)) return false; }
                else {
                    bool target = depth == 0 && key == section;
                    if (target && (p == end || *p != '{')) return false;
                    if (!Value(depth+1, target)) return false;
                }
                Space(); if (p == end) return false;
                if (*p == '}') { ++p; return true; }
                if (*p++ != ',') return false;
            }
        }
        if (*p == '[') {
            ++p; Space(); if (p < end && *p == ']') { ++p; return true; }
            for (;;) {
                if (!Value(depth+1)) return false;
                Space(); if (p == end) return false;
                if (*p == ']') { ++p; return true; }
                if (*p++ != ',') return false;
            }
        }
        if (*p == '"') { std::string ignored; return String(ignored); }
        for (const char* word : {"true", "false", "null"}) {
            size_t n = strlen(word);
            if ((size_t)(end-p) >= n && !memcmp(p, word, n)) { p += n; return true; }
        }
        if (*p == '-') ++p;
        if (p == end || *p < '0' || *p > '9') return false;
        if (*p == '0') ++p; else while (p < end && *p >= '0' && *p <= '9') ++p;
        if (p < end && *p == '.') {
            ++p; const char* start = p;
            while (p < end && *p >= '0' && *p <= '9') ++p;
            if (p == start) return false;
        }
        if (p < end && (*p == 'e' || *p == 'E')) {
            ++p; if (p < end && (*p == '+' || *p == '-')) ++p;
            const char* start = p;
            while (p < end && *p >= '0' && *p <= '9') ++p;
            if (p == start) return false;
        }
        return true;
    }
public:
    XrManifestReader(const std::string& text, const char* key, XrManifestFields& out)
        : p(text.data()), end(p+text.size()), section(key), fields(out) {}
    bool Read() { Space(); if (p == end || *p != '{') return false;
        return Value(0) && (Space(), p == end) && !fields.library.empty(); }
};

static void XrInspectManifest(const std::wstring& path, bool layer)
{
    XrFileEvidence(path.c_str(), false);
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size{}; DWORD got = 0;
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 1 || size.QuadPart > 65536) {
        Log("[xr-diag] manifest inspection skipped: invalid size or exceeds 64 KiB"); CloseHandle(file); return;
    }
    std::string text((size_t)size.QuadPart, '\0');
    bool read = ReadFile(file, &text[0], (DWORD)text.size(), &got, nullptr) && got == text.size();
    CloseHandle(file);
    XrManifestFields fields;
    if (!read || !XrManifestReader(text, layer ? "api_layer" : "runtime", fields).Read()) {
        Log("[xr-diag] manifest inspection failed: malformed/unsupported JSON or missing library_path"); return;
    }
    if (layer) {
        XrLogText("layer name (registered, not proof of loading)", fields.name);
        for (const auto& pair : {std::make_pair("enable_environment", fields.enable),
                                 std::make_pair("disable_environment", fields.disable)}) {
            if (pair.second.empty()) continue;
            XrLogText(pair.first, pair.second);
            // Log presence only; arbitrary third-party environment values may be private.
            Log("[xr-diag] environment gate: %s", XrEnvironment(XrWide(pair.second).c_str()).empty() ? "unset/empty" : "set");
        }
    }
    std::wstring library = XrWide(fields.library);
    if (library.empty()) { Log("[xr-diag] library_path is not valid UTF-8"); return; }
    if (library.find_first_of(L"\\/") == std::wstring::npos) {
        XrLogText("library uses DLL search path; resolution not assumed", fields.library); return;
    }
    if (!(library.size() > 1 && library[1] == ':') && library[0] != '\\' && library[0] != '/') {
        size_t slash = path.find_last_of(L"\\/");
        library = (slash == std::wstring::npos ? L"" : path.substr(0, slash+1)) + library;
    }
    XrFileEvidence(library.c_str(), true);
}

static void XrLayerRegistry(HKEY hive, const char* hiveName, const wchar_t* kind)
{
    std::wstring key = L"SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\"; key += kind;
    HKEY opened = nullptr;
    LSTATUS status = RegOpenKeyExW(hive, key.c_str(), 0, KEY_QUERY_VALUE | KEY_WOW64_32KEY, &opened);
    Log("[xr-diag] %s 32-bit %ls layers: registry status=%ld", hiveName, kind, status);
    if (status != ERROR_SUCCESS) return;
    for (DWORD i = 0; i < 128; ++i) {
        std::vector<wchar_t> path(32768); DWORD chars = (DWORD)path.size(), type = 0, value = 0, bytes = sizeof(value);
        status = RegEnumValueW(opened, i, path.data(), &chars, nullptr, &type, (BYTE*)&value, &bytes);
        if (status == ERROR_NO_MORE_ITEMS) break;
        if (status != ERROR_SUCCESS) { Log("[xr-diag] layer enumeration status=%ld", status); continue; }
        Log("[xr-diag] registered layer entry %lu: %s (type=%lu value=%lu)", i,
            type == REG_DWORD && bytes == sizeof(value) ? (value ? "registry-disabled" : "registry-enabled") : "invalid value",
            type, value);
        XrInspectManifest(std::wstring(path.data(), chars), true);
        if (i == 127) Log("[xr-diag] layer inventory truncated at 128 entries");
    }
    RegCloseKey(opened);
}

static void XrStartupInventory()
{
    Log("[xr-diag] startup inventory; process=x86; registry/file checks do not prove DLL loadability");
    HMODULE loader = GetModuleHandleW(L"openxr_loader.dll");
    std::vector<wchar_t> path(32768);
    DWORD n = loader ? GetModuleFileNameW(loader, path.data(), (DWORD)path.size()) : 0;
    if (n && n < path.size()) {
        XrLogText("loaded OpenXR loader", XrUtf8(path.data()));
        DWORD unused = 0, size = GetFileVersionInfoSizeW(path.data(), &unused);
        if (size && size <= 1048576) {
            std::vector<BYTE> version(size); VS_FIXEDFILEINFO* info = nullptr; UINT bytes = 0;
            if (GetFileVersionInfoW(path.data(), 0, size, version.data()) &&
                VerQueryValueW(version.data(), L"\\", (void**)&info, &bytes) && bytes >= sizeof(*info))
                Log("[xr-diag] loader file version=%u.%u.%u.%u", (unsigned)HIWORD(info->dwFileVersionMS),
                    (unsigned)LOWORD(info->dwFileVersionMS), (unsigned)HIWORD(info->dwFileVersionLS), (unsigned)LOWORD(info->dwFileVersionLS));
            else Log("[xr-diag] loader file version unavailable");
        } else Log("[xr-diag] loader file version unavailable");
    } else Log("[xr-diag] loaded loader path unavailable: Win32=%lu", GetLastError());
    for (const wchar_t* key : {L"XR_RUNTIME_JSON", L"XR_API_LAYER_PATH", L"XR_ENABLE_API_LAYERS", L"XR_LOADER_DEBUG"})
        XrLogText(XrUtf8(key).c_str(), XrUtf8(XrEnvironment(key)));
    HKEY runtime = nullptr;
    LSTATUS status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", 0,
                                  KEY_QUERY_VALUE | KEY_WOW64_32KEY, &runtime);
    std::wstring registered;
    if (status == ERROR_SUCCESS) {
        DWORD size = (DWORD)(path.size()*sizeof(wchar_t)), type = 0;
        status = RegQueryValueExW(runtime, L"ActiveRuntime", nullptr, &type, (BYTE*)path.data(), &size);
        if (status == ERROR_SUCCESS && type == REG_SZ && size >= sizeof(wchar_t) && size <= path.size()*sizeof(wchar_t) &&
            size % sizeof(wchar_t) == 0 && path[size/sizeof(wchar_t)-1] == L'\0') registered = path.data();
        else Log("[xr-diag] ActiveRuntime unreadable/unsupported: status=%ld type=%lu", status, type);
        RegCloseKey(runtime);
    } else Log("[xr-diag] HKLM 32-bit runtime registry status=%ld", status);
    XrLogText("HKLM 32-bit ActiveRuntime", XrUtf8(registered));
    std::wstring overridePath = XrEnvironment(L"XR_RUNTIME_JSON");
    if (!overridePath.empty()) {
        Log("[xr-diag] inspecting XR_RUNTIME_JSON override; loader remains authority for selection");
        XrInspectManifest(overridePath, false);
    } else if (!registered.empty()) XrInspectManifest(registered, false);
    for (const wchar_t* kind : {L"Implicit", L"Explicit"}) {
        XrLayerRegistry(HKEY_LOCAL_MACHINE, "HKLM", kind);
        XrLayerRegistry(HKEY_CURRENT_USER, "HKCU", kind);
    }
    Log("[xr-diag] layer inventory is registration evidence only; environment/elevation can affect activation");
}

// Capture Windows debugger output even when instance creation never reaches a debug-utils
// callback. Forward every message unchanged. Only the initializing thread is recorded,
// and only for this scope; neither standard streams nor loader environment are changed.
static thread_local bool g_xrDiagnosticCapture = false;
static thread_local unsigned g_xrDiagnosticMessages = 0;
static thread_local unsigned g_xrDiagnosticLoads = 0;
static decltype(&OutputDebugStringA) g_xrDebugA = nullptr;
static decltype(&OutputDebugStringW) g_xrDebugW = nullptr;
static decltype(&LoadLibraryW) g_xrLoadW = nullptr;
static decltype(&LoadLibraryExW) g_xrLoadExW = nullptr;

static void XrCaptureMessage(const char* text)
{
    if (g_xrDiagnosticMessages++ < 128) XrLogText("startup debug output", text ? std::string(text, strnlen_s(text, 8192)) : "<null>");
    else if (g_xrDiagnosticMessages == 129) Log("[xr-diag] startup debug capture truncated at 128 messages");
}
static void WINAPI XrOutputDebugA(LPCSTR text)
{
    DWORD error = GetLastError(); bool capture = g_xrDiagnosticCapture;
    g_xrDiagnosticCapture = false;
    try { if (capture) XrCaptureMessage(text); }
    catch (...) { Log("[xr-diag] debug message capture failed; forwarding original call"); }
    SetLastError(error); g_xrDebugA(text);
    g_xrDiagnosticCapture = capture;
}
static void WINAPI XrOutputDebugW(LPCWSTR text)
{
    DWORD error = GetLastError(); bool capture = g_xrDiagnosticCapture;
    g_xrDiagnosticCapture = false;
    try { if (capture) XrCaptureMessage(text ? XrUtf8(std::wstring(text, wcsnlen_s(text, 8192))).c_str() : "<null>"); }
    catch (...) { Log("[xr-diag] debug message capture failed; forwarding original call"); }
    SetLastError(error); g_xrDebugW(text);
    g_xrDiagnosticCapture = capture;
}

static void XrLogLibraryLoad(const char* api, LPCWSTR path, HMODULE module, DWORD error)
{
    if (g_xrDiagnosticLoads++ >= 128) {
        if (g_xrDiagnosticLoads == 129) Log("[xr-diag] DLL load capture truncated at 128 calls");
        return;
    }
    XrLogText(api, path ? XrUtf8(std::wstring(path, wcsnlen_s(path, 8192))) : "<null>");
    if (module) {
        Log("[xr-diag] DLL load succeeded: module=%p (does not establish OpenXR initialization)", (void*)module);
        std::vector<wchar_t> resolved(32768);
        DWORD n = GetModuleFileNameW(module, resolved.data(), (DWORD)resolved.size());
        if (n && n < resolved.size()) XrLogText("loaded DLL path", XrUtf8(std::wstring(resolved.data(), n)));
    }
    else {
        Log("[xr-diag] DLL load FAILED: Win32=%lu", error);
        wchar_t message[512]{};
        if (FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error,
                           0, message, (DWORD)_countof(message), nullptr))
            XrLogText("Windows loader error", XrUtf8(message));
    }
}
static HMODULE WINAPI XrLoadLibraryW(LPCWSTR path)
{
    HMODULE module = g_xrLoadW(path); DWORD error = GetLastError();
    if (g_xrDiagnosticCapture) {
        g_xrDiagnosticCapture = false;
        try { XrLogLibraryLoad("LoadLibraryW", path, module, error); }
        catch (...) { Log("[xr-diag] DLL load evidence unavailable"); }
        g_xrDiagnosticCapture = true;
    }
    SetLastError(error); return module;
}
static HMODULE WINAPI XrLoadLibraryExW(LPCWSTR path, HANDLE file, DWORD flags)
{
    HMODULE module = g_xrLoadExW(path, file, flags); DWORD error = GetLastError();
    if (g_xrDiagnosticCapture) {
        g_xrDiagnosticCapture = false;
        try { XrLogLibraryLoad("LoadLibraryExW", path, module, error); }
        catch (...) { Log("[xr-diag] DLL load evidence unavailable"); }
        g_xrDiagnosticCapture = true;
    }
    SetLastError(error); return module;
}
class XrStartupDiagnostics {
    bool hookedA = false, hookedW = false;
    bool hookedLoadW = false, hookedLoadExW = false;
    bool detailed;
    bool Hook(void* target, void* hook, void** original) {
        MH_STATUS s = MH_CreateHook(target, hook, original);
        if (s != MH_OK) { Log("[xr-diag] debug capture hook unavailable: status=%d", (int)s); return false; }
        s = MH_EnableHook(target);
        if (s != MH_OK) { MH_RemoveHook(target); Log("[xr-diag] debug capture enable failed: status=%d", (int)s); return false; }
        return true;
    }
public:
    XrStartupDiagnostics() : detailed(XrDetailedLogging()) {
        if (!detailed) { Log("[xr-diag] detailed startup inventory/capture off (Detailed Logging)"); return; }
        try { XrStartupInventory(); }
        catch (...) { Log("[xr-diag] startup inventory incomplete; continuing OpenXR initialization"); }
        MH_STATUS s = MH_Initialize();
        if (s == MH_OK || s == MH_ERROR_ALREADY_INITIALIZED) {
            hookedA = Hook((void*)&OutputDebugStringA, (void*)&XrOutputDebugA, (void**)&g_xrDebugA);
            hookedW = Hook((void*)&OutputDebugStringW, (void*)&XrOutputDebugW, (void**)&g_xrDebugW);
            // Khronos uses these two entry points for runtime/API-layer DLL loading.
            // Record the OS error directly, even if its verbose loader message goes only to stdout.
            hookedLoadW = Hook((void*)&LoadLibraryW, (void*)&XrLoadLibraryW, (void**)&g_xrLoadW);
            hookedLoadExW = Hook((void*)&LoadLibraryExW, (void*)&XrLoadLibraryExW, (void**)&g_xrLoadExW);
        } else Log("[xr-diag] debug capture initialization failed: status=%d", (int)s);
        g_xrDiagnosticMessages = 0; g_xrDiagnosticLoads = 0;
        g_xrDiagnosticCapture = hookedA || hookedW || hookedLoadW || hookedLoadExW;
        Log("[xr-diag] startup debug capture A=%s W=%s; messages from other threads/stdout may be absent",
            hookedA ? "on" : "off", hookedW ? "on" : "off");
        Log("[xr-diag] startup DLL load capture W=%s ExW=%s",
            hookedLoadW ? "on" : "off", hookedLoadExW ? "on" : "off");
    }
    ~XrStartupDiagnostics() {
        g_xrDiagnosticCapture = false;
        // Keep trampolines allocated: a concurrently forwarded debug call may still be
        // returning through one. Hooks are disabled, and this startup scope runs only once.
        if (hookedA) MH_DisableHook((void*)&OutputDebugStringA);
        if (hookedW) MH_DisableHook((void*)&OutputDebugStringW);
        if (hookedLoadW) MH_DisableHook((void*)&LoadLibraryW);
        if (hookedLoadExW) MH_DisableHook((void*)&LoadLibraryExW);
        if (detailed) Log("[xr-diag] startup capture ended (%u messages, %u DLL load calls observed)",
            g_xrDiagnosticMessages, g_xrDiagnosticLoads);
    }
};
