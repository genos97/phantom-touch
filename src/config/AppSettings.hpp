#pragma once

#include <map>
#include <string>

enum StreamPresetId {
    PRESET_CUSTOM = 0,
    PRESET_COMPETITIVE = 1,
    PRESET_BALANCED = 2,
    PRESET_QUALITY = 3,
    PRESET_WIFI = 4
};

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
    int mirrorMaxSize = 0;    // 0 = Native, 1920, 1600, 1280
    std::string videoCodecOptions; // LATENCY 2.2: comma-separated MediaCodec keys (e.g. bitrate-mode=2,max-bframes=0)
    std::map<std::string, std::string> deviceVideoEncoders; // LATENCY 2.3: per-serial HW H.264 encoder
    bool mirrorAudio = false; // Forward device audio (scrcpy 5.0 supports native audio)
    bool turnScreenOff = false;
    bool stayAwake = true;
    bool autoSwitchProfiles = true; // Auto-switch game/desktop profile by foreground Android app
    bool rightClickBack = true;     // Right-click inside phone in Cursor Mode -> Android Back
    bool useNativeVideo = true;     // Stage 2: single-connection Media Foundation + D3D11 video
    bool vsyncEnabled = false;      // LATENCY 1.4: Waitable-swapchain VSync (no tearing) vs immediate Flip+Tearing
    int audioBufferMs = 25;         // LATENCY 1.6: scrcpy audio relay buffer (15..100 ms)
    bool audioRawCodec = false;     // LATENCY 1.6: uncompressed PCM audio over USB
    bool virtualDisplayMode = false; // Stage 3: "Second Phone" virtual screen (new_display)
    std::string virtualDisplaySpec = "1920x1080/320"; // WxH/DPI or "auto"
    bool gamepadEnabled = true;     // Stage 3: XInput gamepad-to-touch controller
    bool onboardingDone = false;    // F1: First-run onboarding wizard completed
    int lastPhoneW = 1920;          // N27: Last connected phone aspect/size for Offline Studio
    int lastPhoneH = 1080;
    std::string lastWirelessAddr = "192.168.1.100:5555";
    std::string lastGameProfile;    // Remembers preferred game profile across Desktop auto-switch & restarts

    std::string buildScrcpyArgs() const;
    void applyStreamPreset(StreamPresetId id);
    StreamPresetId detectStreamPreset() const;
    static const char* streamPresetName(StreamPresetId id);
    std::string encoderForDevice(const std::string& serial) const;
    void setEncoderForDevice(const std::string& serial, const std::string& encoderName);
    static int countPendingStreamChanges(const AppSettings& live, const AppSettings& applied,
                                         const std::string& serial);
    static bool hasPendingStreamChanges(const AppSettings& live, const AppSettings& applied,
                                        const std::string& serial);
};

class AppSettingsStore {
public:
    static bool load(const std::string& path, AppSettings& outSettings);
    static bool save(const std::string& path, const AppSettings& settings);
};
