// ============================================================================
// test_dll.cpp - Tiny test DLL that writes a marker file when loaded.
// Used to verify the executor can actually load and run a DLL.
// Build: cl /LD test_dll.cpp user32.lib /Fe:test_dll.dll
// ============================================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <string>
#include <ctime>

extern "C" __declspec(dllexport) void Run() {
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "C:\\Users\\Public\\xeno_test_%lu.txt", GetCurrentProcessId());
    FILE* f = fopen(path, "w");
    if (f) {
        time_t now = time(NULL);
        fprintf(f, "DLL LOADED SUCCESSFULLY\n");
        fprintf(f, "PID: %lu\n", GetCurrentProcessId());
        fprintf(f, "Time: %s", ctime(&now));
        fclose(f);
    }
    // Also write a marker in TEMP for redundancy
    char tempPath[MAX_PATH];
    GetTempPathA(MAX_PATH, tempPath);
    char tempFile[MAX_PATH];
    snprintf(tempFile, sizeof(tempFile), "%sxeno_dll_loaded_%lu.marker", tempPath, GetCurrentProcessId());
    f = fopen(tempFile, "w");
    if (f) {
        fprintf(f, "ok");
        fclose(f);
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        Run();
    }
    return TRUE;
}
