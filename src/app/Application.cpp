#include "app/Application.hpp"

#include "bridge/ScrcpyBridge.hpp"
#include "config/Profile.hpp"
#include "config/ProfileStore.hpp"
#include "engine/InputEngine.hpp"
#include "ui/GdiPlusDraw.hpp"
#include "ui/ShellWindow.hpp"

#include <cstdio>
#include <string>

std::string Application::getExeDir() {
    wchar_t wbuf[MAX_PATH] = {0};
    GetModuleFileNameW(NULL, wbuf, MAX_PATH);
    std::wstring ws(wbuf);
    size_t pos = ws.find_last_of(L"\\/");
    std::wstring wdir = (pos == std::wstring::npos) ? L"." : ws.substr(0, pos);
    int bytes = WideCharToMultiByte(CP_UTF8, 0, wdir.c_str(), static_cast<int>(wdir.size()), NULL, 0, NULL, NULL);
    if (bytes <= 0) return ".";
    std::string out(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wdir.c_str(), static_cast<int>(wdir.size()), &out[0], bytes, NULL, NULL);
    return out;
}

void Application::enableDpiAwareness() {
    HMODULE hUser32 = GetModuleHandleA("user32.dll");
    if (!hUser32) return;
    // Prefer Per-Monitor V2 DPI Awareness on Windows 10/11, fallback to system DPI aware
    typedef BOOL (WINAPI *SetProcessDpiAwarenessContextFn)(HANDLE);
    SetProcessDpiAwarenessContextFn fnV2 = reinterpret_cast<SetProcessDpiAwarenessContextFn>(
        reinterpret_cast<void*>(GetProcAddress(hUser32, "SetProcessDpiAwarenessContext")));
    if (fnV2 && fnV2(reinterpret_cast<HANDLE>(static_cast<LONG_PTR>(-4)))) {
        return;
    }
    typedef BOOL (WINAPI *SetProcessDPIAwareFn)(void);
    SetProcessDPIAwareFn fn = reinterpret_cast<SetProcessDPIAwareFn>(
        reinterpret_cast<void*>(GetProcAddress(hUser32, "SetProcessDPIAware")));
    if (fn) fn();
}

void Application::printUsage() {
    std::printf(
        "Phantom Touch v2.1 — Production Emulator Shell & Multi-Touch Keymapper\n"
        "======================================================================\n"
        "Usage:\n"
        "  PhantomTouch.exe [options]\n\n"
        "Options:\n"
        "  --profile <file>   Load a specific profile (default: last active or profiles\\desktop.cfg)\n"
        "  --demo             Launch directly into Offline Keymap Studio (no phone required)\n"
        "  --no-mirror        Connect control bridge without spawning embedded display\n"
        "  --console          Attach a debug console window for live stdout logs\n"
        "  --help             Show this help message\n"
    );
}

int Application::run(int argc, char** argv) {
    enableDpiAwareness();

    bool demoMode = false;
    bool noMirror = false;
    bool showConsole = false;
    bool explicitProfile = false;
    std::string profilePath;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--demo") {
            demoMode = true;
        } else if (arg == "--no-mirror") {
            noMirror = true;
        } else if (arg == "--console") {
            showConsole = true;
        } else if (arg == "--help" || arg == "-h") {
            showConsole = true;
            AllocConsole();
            std::freopen("CONOUT$", "w", stdout);
            printUsage();
            return 0;
        } else if (arg == "--profile" && i + 1 < argc) {
            profilePath = argv[++i];
            explicitProfile = true;
        } else if (!arg.empty() && arg[0] != '-') {
            profilePath = arg;
            explicitProfile = true;
        }
    }

    if (showConsole) {
        AllocConsole();
        std::freopen("CONOUT$", "w", stdout);
        setvbuf(stdout, NULL, _IONBF, 0);
    }

    // Single-instance guard: bring existing Phantom Touch shell to front instead of double-launching
    HANDLE hMutex = CreateMutexA(NULL, TRUE, "Local\\PhantomTouchEmulatorShellMutex");
    if (hMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(L"PhantomTouchShellClass", NULL);
        if (existing) {
            if (IsIconic(existing)) ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        CloseHandle(hMutex);
        return 0;
    }

    GdiPlusSession gdiplus;

    std::string exeDir = getExeDir();
    std::string profilesDir = exeDir + "\\profiles";
    CreateDirectoryA(profilesDir.c_str(), NULL);
    std::string settingsPath = profilesDir + "\\.shell_settings.cfg";

    bool firstRunDesktop = false;
    if (!explicitProfile) {
        std::string lastPath = ProfileStore::loadLastActive(profilesDir);
        if (lastPath.empty()) {
            profilePath = profilesDir + "\\desktop.cfg";
            firstRunDesktop = true;
        } else {
            profilePath = lastPath;
        }
    }

    Profile profile = firstRunDesktop
        ? ProfileDefaults::createDesktop("Desktop (Typing)")
        : ProfileDefaults::createFreeFire();
    if (!ProfileStore::load(profilePath, profile)) {
        ProfileStore::save(profilePath, profile);
    }
    ProfileStore::saveLastActive(profilesDir, profilePath);

    ScrcpyBridge bridge;
    bridge.appendLog("[Profile] Active: " + profile.name);

    InputEngine engine(&bridge);
    engine.setProfile(profile);
    engine.start();

    ShellWindow shell(&bridge, &engine, exeDir, profilesDir, profilePath, settingsPath, demoMode, noMirror);
    int exitCode = 1;
    if (shell.create(GetModuleHandleA(NULL))) {
        exitCode = shell.runMessageLoop();
    } else {
        MessageBoxA(NULL, "Failed to initialize Phantom Touch Shell window.", "Phantom Touch", MB_OK | MB_ICONERROR);
    }

    engine.stop();
    bridge.disconnect();

    if (hMutex) {
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
    }
    return exitCode;
}
