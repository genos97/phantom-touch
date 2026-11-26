#include "engine/ThreadScheduling.hpp"

#ifndef LOAD_LIBRARY_SEARCH_SYSTEM32
#define LOAD_LIBRARY_SEARCH_SYSTEM32 0x00000800
#endif

namespace {

typedef HANDLE (WINAPI *PFN_AvSetMmThreadCharacteristicsW)(LPCWSTR, LPDWORD);
typedef BOOL   (WINAPI *PFN_AvRevertMmThreadCharacteristics)(HANDLE);

struct AvrtApi {
    HMODULE mod = NULL;
    PFN_AvSetMmThreadCharacteristicsW setChars = nullptr;
    PFN_AvRevertMmThreadCharacteristics revert = nullptr;
    bool resolved = false;
};

AvrtApi& getAvrtApi() {
    static AvrtApi api;
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    BOOL pending = FALSE;
    if (InitOnceBeginInitialize(&once, 0, &pending, NULL) && pending) {
        api.mod = LoadLibraryExW(L"avrt.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!api.mod) {
            // Fallback on unpatched Win7 without KB2533623
            api.mod = LoadLibraryW(L"avrt.dll");
        }
        if (api.mod) {
            api.setChars = reinterpret_cast<PFN_AvSetMmThreadCharacteristicsW>(
                reinterpret_cast<void*>(GetProcAddress(api.mod, "AvSetMmThreadCharacteristicsW")));
            api.revert = reinterpret_cast<PFN_AvRevertMmThreadCharacteristics>(
                reinterpret_cast<void*>(GetProcAddress(api.mod, "AvRevertMmThreadCharacteristics")));
        }
        api.resolved = true;
        InitOnceComplete(&once, 0, NULL);
    }
    return api;
}

struct PowerThrottlingStateCompat {
    ULONG Version;
    ULONG ControlMask;
    ULONG StateMask;
};

typedef BOOL (WINAPI *PFN_SetProcessInformation)(
    HANDLE hProcess, int ProcessInformationClass, LPVOID ProcessInformation, DWORD ProcessInformationSize);

} // namespace

namespace ThreadScheduling {

HANDLE enableMmcssGames(DWORD& outTaskIdx) {
    outTaskIdx = 0;
    AvrtApi& api = getAvrtApi();
    if (!api.setChars) return NULL;
    return api.setChars(L"Games", &outTaskIdx);
}

void disableMmcss(HANDLE avrtHandle) {
    if (!avrtHandle) return;
    AvrtApi& api = getAvrtApi();
    if (api.revert) {
        api.revert(avrtHandle);
    }
}

bool disableProcessEcoQoS() {
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (!k32) return false;
    PFN_SetProcessInformation fnSetProcInfo = reinterpret_cast<PFN_SetProcessInformation>(
        reinterpret_cast<void*>(GetProcAddress(k32, "SetProcessInformation")));
    if (!fnSetProcInfo) return false;

    // ProcessPowerThrottling = 4, PROCESS_POWER_THROTTLING_EXECUTION_SPEED = 0x1, StateMask = 0 -> opt out
    PowerThrottlingStateCompat state;
    state.Version = 1;
    state.ControlMask = 0x1u;
    state.StateMask = 0u;
    return fnSetProcInfo(GetCurrentProcess(), 4, &state, sizeof(state)) != FALSE;
}

} // namespace ThreadScheduling
