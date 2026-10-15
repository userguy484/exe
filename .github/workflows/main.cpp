// executor.cpp — roblox luau executor DLL
// build: g++ -O2 -shared -static-libgcc -static-libstdc++ -o executor.dll executor.cpp -lkernel32 -luser32

#define _CRT_SECURE_NO_WARNINGS

#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <thread>
#include <atomic>

// ---- luau typedefs ----
using lua_State      = void;
using lua_CFunction  = int(*)(lua_State*);

// luau function signatures
using luau_load_t     = int(*)(lua_State*, const char*, const char*, size_t, int);
using lua_pcall_t     = int(*)(lua_State*, int, int, int);
using luaL_newstate_t = lua_State*(*)();
using lua_close_t     = void(*)(lua_State*);
using lua_pushcclosure_t = void(*)(lua_State*, lua_CFunction, int);
using lua_setglobal_t = void(*)(lua_State*, const char*);
using lua_getglobal_t = void(*)(lua_State*, const char*);

// globals
static lua_State*          g_L        = nullptr;
static luau_load_t         luau_load  = nullptr;
static lua_pcall_t         lua_pcall  = nullptr;
static lua_getglobal_t     lua_getglobal = nullptr;
static lua_setglobal_t     lua_setglobal = nullptr;
static lua_pushcclosure_t  lua_pushcclosure = nullptr;

static std::atomic<bool>   g_running{true};
static HANDLE              g_console = nullptr;

// ----------------------------------------------------------------
// pattern scanning helpers
// ----------------------------------------------------------------
static uintptr_t scan_pattern(uintptr_t base, size_t size,
                              const char* pattern, const char* mask) {
    size_t plen = strlen(mask);
    for (size_t i = 0; i + plen <= size; ++i) {
        bool hit = true;
        for (size_t j = 0; j < plen; ++j) {
            if (mask[j] == 'x' && *(uint8_t*)(base + i + j) != (uint8_t)pattern[j]) {
                hit = false;
                break;
            }
        }
        if (hit) return base + i;
    }
    return 0;
}

static size_t module_size(HMODULE mod) {
    auto dos = (IMAGE_DOS_HEADER*)mod;
    auto nt  = (IMAGE_NT_HEADERS*)((uintptr_t)mod + dos->e_lfanew);
    return nt->OptionalHeader.SizeOfImage;
}

// ----------------------------------------------------------------
// resolve Luau functions by pattern scanning RobloxPlayerBeta.exe
// ----------------------------------------------------------------
bool resolve_luau() {
    HMODULE base = GetModuleHandleW(nullptr);
    if (!base) return false;

    uintptr_t start = (uintptr_t)base;
    size_t    sz    = module_size(base);

    uintptr_t luau_load_addr = scan_pattern(
        start, sz,
        "\x48\x89\x5C\x24\x00\x48\x89\x6C\x24\x00\x48\x89\x74\x24\x00\x57\x48\x83\xEC\x60",
        "xxxx?xxxx?xxxx?xxxxx");

    uintptr_t pcall_addr = scan_pattern(
        start, sz,
        "\x48\x83\xEC\x28\x48\x8B\x0D\x00\x00\x00\x00\x48\x85\xC9\x74",
        "xxxxxxxx????xxxx");

    uintptr_t setglobal_addr = scan_pattern(
        start, sz,
        "\x48\x89\x5C\x24\x00\x57\x48\x83\xEC\x20\x48\x8B\xD9\x48\x8B\xF9",
        "xxxx?xxxxxxxxx");

    uintptr_t getglobal_addr = scan_pattern(
        start, sz,
        "\x48\x89\x5C\x24\x00\x57\x48\x83\xEC\x20\x48\x8B\xD9\xE8",
        "xxxx?xxxxxxxx");

    uintptr_t pushcclosure_addr = scan_pattern(
        start, sz,
        "\x48\x89\x5C\x24\x00\x48\x89\x74\x24\x00\x57\x48\x83\xEC\x20",
        "xxxx?xxxx?xxxx");

    if (!luau_load_addr || !pcall_addr || !setglobal_addr
        || !getglobal_addr || !pushcclosure_addr) {
        return false;
    }

    luau_load        = (luau_load_t)luau_load_addr;
    lua_pcall        = (lua_pcall_t)pcall_addr;
    lua_setglobal    = (lua_setglobal_t)setglobal_addr;
    lua_getglobal    = (lua_getglobal_t)getglobal_addr;
    lua_pushcclosure = (lua_pushcclosure_t)pushcclosure_addr;

    return true;
}

// ----------------------------------------------------------------
// state retrieval — detour on lua_getglobal to capture L
// ----------------------------------------------------------------
struct Hook {
    void* target = nullptr;
    void* original = nullptr;
    uint8_t saved[16]{};
    void* stub = nullptr;
};

static Hook g_getglobal_hook;

static void capture_state_hook(void* L, const char* name) {
    if (!g_L) g_L = (lua_State*)L;
    using fn_t = void(*)(void*, const char*);
    ((fn_t)g_getglobal_hook.original)(L, name);
}

bool install_state_hook() {
    if (!lua_getglobal) return false;

    void* target = (void*)lua_getglobal;

    uint8_t patch[12] = { 0x48, 0xB8 };
    uint64_t dest = (uint64_t)&capture_state_hook;
    memcpy(patch + 2, &dest, 8);
    patch[10] = 0xFF;
    patch[11] = 0xE0;

    DWORD old;
    VirtualProtect(target, 12, PAGE_EXECUTE_READWRITE, &old);
    memcpy(g_getglobal_hook.saved, target, 12);
    memcpy(target, patch, 12);
    VirtualProtect(target, 12, old, &old);

    g_getglobal_hook.target = target;
    g_getglobal_hook.original = target;

    return true;
}

// ----------------------------------------------------------------
// execute a lua string against the captured state
// ----------------------------------------------------------------
bool execute_script(const std::string& src) {
    if (!g_L) return false;
    if (!luau_load || !lua_pcall) return false;

    const char* chunkname = "@executor";

    int load_res = luau_load(g_L, chunkname, src.data(), src.size(), 0);
    if (load_res != 0) {
        return false;
    }

    int pcall_res = lua_pcall(g_L, 0, 0, 0);
    return pcall_res == 0;
}

// ----------------------------------------------------------------
// console + input loop
// ----------------------------------------------------------------
void console_thread() {
    AllocConsole();

    freopen("CONOUT$", "w", stdout);
    freopen("CONOUT$", "w", stderr);
    freopen("CONIN$",  "r", stdin);

    SetConsoleTitleW(L"executor");

    printf("executor attached\n");
    printf("waiting for luau state...\n");

    for (int i = 0; i < 300 && !g_L; ++i) {
        Sleep(100);
    }

    if (!g_L) {
        printf("[!] failed to capture lua_State\n");
        printf("    patterns may be outdated for this roblox version\n");
        return;
    }

    printf("[+] lua_State captured: %p\n", g_L);
    printf("type lua and hit enter. 'exit' to unload.\n\n");

    std::string line;
    while (g_running) {
        printf("> ");
        std::getline(std::cin, line);
        if (line == "exit" || line == "quit") break;
        if (line.empty()) continue;

        if (execute_script(line)) {
            printf("[ok]\n");
        } else {
            printf("[error] script failed\n");
        }
    }

    g_running = false;
}

// ----------------------------------------------------------------
// DLL entry
// ----------------------------------------------------------------
BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(mod);
        CreateThread(nullptr, 0, (LPTHREAD_START_ROUTINE)[](LPVOID) -> DWORD {
            Sleep(3000);

            if (!resolve_luau()) {
                AllocConsole();
                freopen("CONOUT$", "w", stdout);
                printf("[!] failed to resolve luau functions.\n");
                printf("    update the patterns in resolve_luau().\n");
                return 0;
            }

            install_state_hook();
            console_thread();
            return 0;
        }, nullptr, 0, nullptr);
    }
    return TRUE;
}
