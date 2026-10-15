// injector.cpp — remote DLL injection into a target process
// build: cl /std:c++20 /O2 injector.cpp /link /out:injector.exe

#include <windows.h>
#include <tlhelp32.h>
#include <string>
#include <iostream>
#include <filesystem>

DWORD find_pid(const std::wstring& name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    DWORD pid = 0;

    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, name.c_str()) == 0) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

bool inject(DWORD pid, const std::wstring& dll_path) {
    HANDLE proc = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE, pid);

    if (!proc) {
        std::wcerr << L"[!] OpenProcess failed: " << GetLastError() << L"\n";
        return false;
    }

    size_t bytes = (dll_path.size() + 1) * sizeof(wchar_t);
    LPVOID remote = VirtualAllocEx(proc, nullptr, bytes,
                                   MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) {
        std::wcerr << L"[!] VirtualAllocEx failed\n";
        CloseHandle(proc);
        return false;
    }

    if (!WriteProcessMemory(proc, remote, dll_path.c_str(), bytes, nullptr)) {
        std::wcerr << L"[!] WriteProcessMemory failed\n";
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        CloseHandle(proc);
        return false;
    }

    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    LPTHREAD_START_ROUTINE load_lib =
        (LPTHREAD_START_ROUTINE)GetProcAddress(k32, "LoadLibraryW");

    HANDLE th = CreateRemoteThread(proc, nullptr, 0, load_lib, remote, 0, nullptr);
    if (!th) {
        std::wcerr << L"[!] CreateRemoteThread failed: " << GetLastError() << L"\n";
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        CloseHandle(proc);
        return false;
    }

    WaitForSingleObject(th, INFINITE);

    DWORD exit_code = 0;
    GetExitCodeThread(th, &exit_code);

    CloseHandle(th);
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    CloseHandle(proc);

    if (exit_code == 0) {
        std::wcerr << L"[!] LoadLibrary returned NULL inside target\n";
        return false;
    }

    std::wcout << L"[+] Injected. module base: 0x"
               << std::hex << exit_code << L"\n";
    return true;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::wcout << L"usage: injector.exe <path-to-dll>\n";
        return 1;
    }

    std::wstring dll = argv[1];
    if (!std::filesystem::exists(dll)) {
        std::wcerr << L"[!] dll not found: " << dll << L"\n";
        return 1;
    }
    dll = std::filesystem::absolute(dll).wstring();

    DWORD pid = find_pid(L"RobloxPlayerBeta.exe");
    if (!pid) {
        std::wcerr << L"[!] RobloxPlayerBeta.exe not running\n";
        return 1;
    }

    std::wcout << L"[*] roblox pid: " << pid << L"\n";
    return inject(pid, dll) ? 0 : 1;
}
