// External x86 dump capture. Never injected into the game; dumps stay local.
#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
#include <cwchar>

int wmain(int argc,wchar_t** argv) {
    if(argc==2 && !wcscmp(argv[1],L"--test-target")){Sleep(30000);return 0;}
    if(argc!=3){fwprintf(stderr,L"Usage: capture-hang PID output.dmp\n");return 2;}
    wchar_t* end=nullptr;const unsigned long id=wcstoul(argv[1],&end,10);
    if(!id || !end || *end)return 2;
    HANDLE process=OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ|PROCESS_DUP_HANDLE,FALSE,id);
    if(!process){fwprintf(stderr,L"OpenProcess failed: %lu\n",GetLastError());return 3;}
    wchar_t exe[32768]{};DWORD length=_countof(exe);
    if(!QueryFullProcessImageNameW(process,0,exe,&length)){CloseHandle(process);return 4;}
    const wchar_t* leaf=wcsrchr(exe,L'\\');leaf=leaf?leaf+1:exe;
    // Limit collection to the game and this helper's explicit self-test process.
    if(_wcsicmp(leaf,L"MirrorsEdge.exe") && _wcsicmp(leaf,L"capture-hang.exe")){
        fwprintf(stderr,L"Refusing unrelated process: %s\n",leaf);CloseHandle(process);return 5;
    }
    HANDLE file=CreateFileW(argv[2],GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE){fwprintf(stderr,L"CreateFile failed: %lu\n",GetLastError());CloseHandle(process);return 6;}
    const auto flags=static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo|MiniDumpWithUnloadedModules|
        MiniDumpWithIndirectlyReferencedMemory|MiniDumpWithFullMemoryInfo|
        MiniDumpWithProcessThreadData|MiniDumpWithHandleData);
    const BOOL ok=MiniDumpWriteDump(process,id,file,flags,nullptr,nullptr,nullptr);
    const DWORD error=ok?0:GetLastError();CloseHandle(file);CloseHandle(process);
    if(!ok){fwprintf(stderr,L"MiniDumpWriteDump failed: %lu\n",error);return 7;}
    wprintf(L"Captured PID %lu to %s\n",id,argv[2]);return 0;
}
