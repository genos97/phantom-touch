#pragma once

#include <string>

// Persists desktop emulator shell layout & scrcpy mirror stream preferences
// in <profilesDir>\.shell_settings.cfg (independent from per-game keymap profiles).
struct AppSettings {
    int winX = -1;            // -1 = auto center on primary monitor
    int winY = -1;
    int winW = 1360;
    int winH = 780;
    bool maximized = false;
    bool alwaysOnTop = false;

    // Mirror stream parameters (Uncapped default per LATENCY_PLAN V10; set 60/90/120 on Wi-Fi)
    int mirrorFps = 0;        // 0 = Uncapped, 60 / 90 / 120
    int mirrorBitrateMbps = 16; // 8 / 12 / 16 / 24
    int mirrorMaxSize = 0;    // 0 = Native, 1600, 1280
    bool mirrorAudio = false; // Forward device audio (scrcpy 5.0 supports native audio)
    bool turnScreenOff = false;
    bool stayAwake = true;
    bool autoSwitchProfiles = true; // Auto-switch game/desktop profile by foreground Android app
    bool rightClickBack = true;     // Right-click inside phone in Cursor Mode -> Android Back
    bool useNativeVideo = true;     // Stage 2: single-connection Media Foundation + D3D11 video
    bool virtualDisplayMode = false; // Stage 3: "Second Phone" virtual screen (new_display)
    std::string virtualDisplaySpec = "1920x1080/320"; // WxH/DPI or "auto"
    bool gamepadEnabled = true;     // Stage 3: XInput gamepad-to-touch controller
    std::string lastWirelessAddr = "192.168.1.100:5555";
    std::string lastGameProfile;    // Remembers preferred game profile across Desktop auto-switch & restarts

    std::string buildScrcpyArgs() const;
};

class AppSettingsStore {
public:
    static bool load(const std::string& path, AppSettings& outSettings);
    static bool save(const std::string& path, const AppSettings& settings);
};
