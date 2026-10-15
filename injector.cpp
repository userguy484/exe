// injector.cpp — attaches to RobloxPlayerBeta.exe and injects a DLL
// build: g++ -O2 -static -o injector.exe injector.cpp -lkernel32 -luser32
// usage: injector.exe path\to\executor.dll

#define _CRT_SECURE_NO_WARNINGS

#include <windows.h>
#include <tlhelp32.h>
#include <string>
#include <cstdio>
#include <cstring>
#include <iostream>

// ----------------------------------------------------------------
// find process id by exe name (case-insensitive)
// ----------------------------------------------------------------
static DWORD find_pid(const wchar_t* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    DWORD pid = 0;

    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, name) == 0) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

// ----------------------------------------------------------------
// enable SeDebugPrivilege — needed to open roblox with full access
// ----------------------------------------------------------------
static bool enable_debug_priv() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok))
        return false;

    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    LookupPrivilegeValueW(nullptr, L"SeDebugPrivilege", &tp.Privileges[0].Luid);
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    BOOL ok = AdjustTokenPrivileges(tok, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    CloseHandle(tok);
    return ok && GetLastError() == ERROR_SUCCESS;
}

// ----------------------------------------------------------------
// inject DLL into pid via CreateRemoteThread + LoadLibraryW
// ----------------------------------------------------------------
static bool inject_dll(DWORD pid, const std::wstring& dll_path) {
    HANDLE proc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!proc) {
        printf("[!] OpenProcess failed (err=%lu) — run as admin\n", GetLastError());
        return false;
    }

    SIZE_T path_bytes = (dll_path.size() + 1) * sizeof(wchar_t);
    LPVOID remote_mem = VirtualAllocEx(proc, nullptr, path_bytes,
                                       MEM_COMMIT | MEM_RESERVE,
                                       PAGE_READWRITE);
    if (!remote_mem) {
        printf("[!] VirtualAllocEx failed (err=%lu)\n", GetLastError());
        CloseHandle(proc);
        return false;
    }

    if (!WriteProcessMemory(proc, remote_mem, dll_path.c_str(),
                            path_bytes, nullptr)) {
        printf("[!] WriteProcessMemory failed (err=%lu)\n", GetLastError());
        VirtualFreeEx(proc, remote_mem, 0, MEM_RELEASE);
        CloseHandle(proc);
        return false;
    }

    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (!k32) {
        printf("[!] GetModuleHandleW(kernel32) failed\n");
        VirtualFreeEx(proc, remote_mem, 0, MEM_RELEASE);
        CloseHandle(proc);
        return false;
    }
    FARPROC load_lib = GetProcAddress(k32, "LoadLibraryW");
    if (!load_lib) {
        printf("[!] GetProcAddress(LoadLibraryW) failed\n");
        VirtualFreeEx(proc, remote_mem, 0, MEM_RELEASE);
        CloseHandle(proc);
        return false;
    }

    HANDLE thread = CreateRemoteThread(proc, nullptr, 0,
                                       (LPTHREAD_START_ROUTINE)load_lib,
                                       remote_mem, 0, nullptr);
    if (!thread) {
        printf("[!] CreateRemoteThread failed (err=%lu)\n", GetLastError());
        VirtualFreeEx(proc, remote_mem, 0, MEM_RELEASE);
        CloseHandle(proc);
        return false;
    }

    DWORD wait = WaitForSingleObject(thread, 15000);
    if (wait == WAIT_TIMEOUT) {
        printf("[!] injection thread timed out\n");
        CloseHandle(thread);
        VirtualFreeEx(proc, remote_mem, 0, MEM_RELEASE);
        CloseHandle(proc);
        return false;
    }

    DWORD module_handle = 0;
    GetExitCodeThread(thread, &module_handle);

    CloseHandle(thread);
    VirtualFreeEx(proc, remote_mem, 0, MEM_RELEASE);
    CloseHandle(proc);

    if (module_handle == 0) {
        printf("[!] LoadLibraryW returned NULL inside roblox\n");
        printf("    the dll was rejected — wrong architecture, missing deps,\n");
        printf("    or byfron blocked the load\n");
        return false;
    }

    printf("[+] module handle in target: 0x%08lX\n", module_handle);
    return true;
}

// ----------------------------------------------------------------
// main
// ----------------------------------------------------------------
int main(int argc, char** argv) {
    printf("=== dll injector ===\n");

    if (argc < 2) {
        printf("usage: %s <path-to-dll>\n", argv[0]);
        printf("press enter to exit.\n");
        std::cin.get();
        return 1;
    }

    char full[MAX_PATH] = {0};
    if (!GetFullPathNameA(argv[1], MAX_PATH, full, nullptr)) {
        printf("[!] could not resolve path: %s\n", argv[1]);
        std::cin.get();
        return 1;
    }

    DWORD attrs = GetFileAttributesA(full);
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        printf("[!] file not found: %s\n", full);
        std::cin.get();
        return 1;
    }

    wchar_t wide[MAX_PATH] = {0};
    MultiByteToWideChar(CP_UTF8, 0, full, -1, wide, MAX_PATH);
    std::wstring dll_path(wide);

    printf("[*] dll: %ls\n", dll_path.c_str());

    if (!enable_debug_priv()) {
        printf("[!] could not enable SeDebugPrivilege — run as admin\n");
        std::cin.get();
        return 1;
    }

    printf("[*] waiting for RobloxPlayerBeta.exe...\n");
    DWORD pid = 0;
    int waited = 0;
    while (!pid && waited < 60000) {
        pid = find_pid(L"RobloxPlayerBeta.exe");
        if (!pid) {
            Sleep(500);
            waited += 500;
        }
    }

    if (!pid) {
        printf("[!] roblox not running after 60s — launch the game first\n");
        std::cin.get();
        return 1;
    }

    printf("[+] roblox found (pid %lu)\n", pid);
    printf("[*] waiting 3s for the client to settle...\n");
    Sleep(3000);

    printf("[*] injecting...\n");
    if (inject_dll(pid, dll_path)) {
        printf("[+] done. check for a console window from the dll.\n");
    } else {
        printf("[!] injection failed.\n");
    }

    printf("press enter to close.\n");
    std::cin.get();
    return 0;
}
