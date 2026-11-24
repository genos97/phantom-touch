#include "aim_engine.hpp"
#include "overlay.hpp"
#include "profile.hpp"
#include "protocol.hpp"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

std::string getExeDir() {
    char buf[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, buf, MAX_PATH);
    std::string s(buf);
    size_t pos = s.find_last_of("\\/");
    return (pos == std::string::npos) ? "." : s.substr(0, pos);
}

void printUsage() {
    std::printf(
        "Scrcpy Keymapper Pro (100%% PC-Side C++ Wrapper)\n"
        "================================================\n"
        "Usage:\n"
        "  scrcpy_keymapper.exe [options]\n\n"
        "Options:\n"
        "  --profile <file>   Load/save a specific profile (default: profiles\\freefire.cfg)\n"
        "  --demo             Launch Offline Demo & HUD Editor canvas (no phone required)\n"
        "  --no-mirror        Connect scrcpy control bridge without spawning mirror window\n"
        "  --help             Show this help message\n\n"
        "Hotkeys:\n"
        "  ~ or F1            Toggle Aim Mode (360° Raw Mouse) <-> Cursor Mode\n"
        "  F2                 Toggle Visual Drag-and-Drop HUD & X/Y Editor\n"
        "  F3                 Toggle Fire Mode (Drag-on-Fire vs Separate Left Fire)\n"
        "  F4                 Show / Hide Key Badges while playing\n"
        "  F5                 Quick Reset all touches (anti-stuck failsafe)\n"
        "  [ / ]              Decrease / Increase X Sensitivity live\n"
        "  - / =              Decrease / Increase Y Sensitivity live\n"
        "  Ctrl+S (in F2)     Save profile to disk\n"
    );
}

} // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);

    // Critical on laptops with 125%/150% Windows scaling so overlay & clicks align 1:1
    typedef BOOL (WINAPI *SetProcessDPIAwareFn)(void);
    HMODULE hUser32 = GetModuleHandleA("user32.dll");
    if (hUser32) {
        SetProcessDPIAwareFn fn = reinterpret_cast<SetProcessDPIAwareFn>(
            GetProcAddress(hUser32, "SetProcessDPIAware"));
        if (fn) fn();
    }

    std::string exeDir = getExeDir();
    std::string profilesDir = exeDir + "\\profiles";
    CreateDirectoryA(profilesDir.c_str(), NULL);

    std::string profilePath;
    bool explicitProfile = false;
    bool demoMode = false;
    bool noMirror = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--demo") {
            demoMode = true;
        } else if (arg == "--no-mirror") {
            noMirror = true;
        } else if (arg == "--help" || arg == "-h") {
            printUsage();
            return 0;
        } else if (arg == "--profile" && i + 1 < argc) {
            profilePath = argv[++i];
            explicitProfile = true;
        } else if (arg[0] != '-') {
            profilePath = arg;
            explicitProfile = true;
        }
    }

    if (!explicitProfile) {
        std::string lastPath = loadLastActiveProfilePath(profilesDir);
        profilePath = lastPath.empty() ? (profilesDir + "\\freefire.cfg") : lastPath;
    }

    // Load or generate default Free Fire profile
    Profile profile = createDefaultFreeFireProfile();
    if (loadProfile(profilePath, profile)) {
        std::printf("[Profile] Loaded '%s' from %s\n", profile.name.c_str(), profilePath.c_str());
    } else {
        std::printf("[Profile] Creating default profile at %s\n", profilePath.c_str());
        saveProfile(profilePath, profile);
    }

    const std::string mirrorTitle = "ScrcpyKeymapper-Display";
    ScrcpyBridge bridge;

    if (!demoMode) {
        ToolPaths tools;
        std::string err;
        if (!bridge.locateTools(tools, err)) {
            std::printf("\n[Setup Required] %s\n", err.c_str());
            std::string boxMsg = err + "\n\nOpen Offline Demo / HUD Editor mode instead?";
            int res = MessageBoxA(NULL, boxMsg.c_str(), "Scrcpy Keymapper Pro", MB_YESNO | MB_ICONINFORMATION);
            if (res == IDYES) {
                demoMode = true;
            } else {
                return 1;
            }
        } else {
            std::printf("[Tools] scrcpy %s | adb: %s\n", tools.scrcpyVersion.c_str(), tools.adbExe.c_str());
            if (!bridge.connectControl(tools, profile.controlMaxSize, err)) {
                std::printf("\n[Device Error] %s\n", err.c_str());
                std::string boxMsg = err + "\n\nOpen Offline Demo / HUD Editor mode instead?";
                int res = MessageBoxA(NULL, boxMsg.c_str(), "Scrcpy Keymapper Pro", MB_YESNO | MB_ICONWARNING);
                if (res == IDYES) {
                    demoMode = true;
                } else {
                    return 1;
                }
            } else if (!noMirror) {
                if (!bridge.launchMirrorWindow(tools, profile.scrcpyArgs, mirrorTitle, err)) {
                    std::printf("[Mirror Warning] %s\n", err.c_str());
                } else {
                    std::printf("[Mirror] Launched scrcpy display window '%s'\n", mirrorTitle.c_str());
                }
            }
        }
    }

    AimEngine engine(&bridge);
    engine.setProfile(profile);
    engine.start();

    OverlayApp overlay(&bridge, &engine, profilesDir, profilePath, mirrorTitle, demoMode);
    if (!overlay.create(GetModuleHandleA(NULL))) {
        std::printf("[Error] Failed to create overlay window.\n");
        engine.stop();
        bridge.disconnect();
        return 1;
    }

    std::printf("[Ready] Press ~ or F1 to toggle Aim Mode | F2 for visual HUD Editor | Close window to exit.\n");
    int exitCode = overlay.runMessageLoop();

    engine.stop();
    bridge.disconnect();
    return exitCode;
}
