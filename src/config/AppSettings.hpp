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

    // Mirror stream parameters (defaults match the smooth 90 FPS 16M setup)
    int mirrorFps = 90;       // 60 / 90 / 120
    int mirrorBitrateMbps = 16; // 8 / 12 / 16 / 24
    int mirrorMaxSize = 0;    // 0 = Native, 1600, 1280
    bool mirrorAudio = false; // Forward device audio (scrcpy 5.0 supports native audio)
    bool turnScreenOff = false;
    bool stayAwake = true;
    std::string lastWirelessAddr = "192.168.1.100:5555";

    std::string buildScrcpyArgs() const;
};

class AppSettingsStore {
public:
    static bool load(const std::string& path, AppSettings& outSettings);
    static bool save(const std::string& path, const AppSettings& settings);
};
