#include <windows.h>
#include <iostream>
#include <string>
#include <vector>

extern "C" {
    #include "lua.h"
    #include "lualib.h"
    #include "lauxlib.h"
}

// Define a custom function to expose to Lua (e.g., print or game:HttpGet)
static int custom_httpget(lua_State* L) {
    const char* url = luaL_checkstring(L, 1);
    std::string response = "Downloaded data from: " + std::string(url); // Replace with actual WinHTTP code
    
    lua_pushstring(L, response.c_str());
    return 1; // Number of return values
}

// Define a custom function to expose the game global
static int get_game(lua_State* L) {
    // In a real executor, this would return the actual Roblox DataModel userdata
    lua_newtable(L); // Fake game table for demonstration
    lua_pushstring(L, "Fake Roblox Game Instance");
    lua_setfield(L, -2, "Name");
    return 1;
}

// Registry to inject functions into the Lua environment
void RegisterAPI(lua_State* L) {
    // Register a global function: HttpGet("url")
    lua_pushcfunction(L, custom_httpget, "HttpGet");
    lua_setglobal(L, "HttpGet");

    // Register a global property: game
    lua_pushcfunction(L, get_game, "game");
    lua_setglobal(L, "game");
}

// The main execution function
void ExecuteScript(lua_State* L, const std::string& script) {
    // 1. Load the script into the VM
    int status = luaL_loadbuffer(L, script.c_str(), script.length(), "ExecutorScript");
    if (status != LUA_OK) {
        std::cout << "[Lua Load Error] " << lua_tostring(L, -1) << std::endl;
        lua_pop(L, 1);
        return;
    }

    // 2. Register the custom API functions before running
    RegisterAPI(L);

    // 3. Execute the script via pcall (protected call)
    status = lua_pcall(L, 0, LUA_MULTRET, 0);
    if (status != LUA_OK) {
        std::cout << "[Lua Runtime Error] " << lua_tostring(L, -1) << std::endl;
        lua_pop(L, 1);
    }
}

// Console allocation for debugging
void SetupConsole() {
    AllocConsole();
    FILE* f;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONIN$", "r", stdin);
}

// DLL Entry Point
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        
        // Run the executor in a separate thread so we don't block Roblox
        CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
            SetupConsole();
            std::cout << "[+] Wesoda Internal Executor Initialized.\n";

            // 1. Initialize Luau State
            lua_State* L = luaL_newstate();
            if (!L) {
                std::cout << "[-] Failed to create Luau state.\n";
                return 0;
            }
            
            // Open standard libraries (math, string, table, etc.)
            luaL_openlibs(L);

            // 2. Example: Execute a script automatically on inject
            std::string testScript = 
                "print('Hello from internal C++ executor!')\n"
                "local data = HttpGet('https://api.github.com')\n"
                "print(data)\n"
                "print('Game Name: ' .. game.Name)\n";
            
            ExecuteScript(L, testScript);
            
            return 0;
        }, nullptr, 0, nullptr);
        break;
    }
    return TRUE;
}
