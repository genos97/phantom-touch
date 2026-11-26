#include "config/AppSettings.hpp"

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace {

std::string trimLine(const std::string& s) {
    size_t a = 0;
    while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    size_t b = s.size();
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

int clampInt(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

std::string sanitizeSafeToken(const std::string& raw, bool allowCommaEq) {
    std::string out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size() && out.size() < 256; ++i) {
        unsigned char uc = static_cast<unsigned char>(raw[i]);
        bool ok = std::isalnum(uc) || uc == '_' || uc == '-' || uc == '.' || uc == ':';
        if (allowCommaEq && (uc == ',' || uc == '=')) ok = true;
        if (ok) out.push_back(static_cast<char>(uc));
    }
    return out;
}

} // namespace

std::string AppSettings::buildScrcpyArgs() const {
    // Only video/audio flags valid with scrcpy --no-control!
    // (stay_awake and turn_screen_off are handled via our control=true server bridge!)
    char buf[160];
    if (mirrorFps > 0) {
        std::snprintf(buf, sizeof(buf), "--max-fps=%d --video-bit-rate=%dM",
                      clampInt(mirrorFps, 30, 144),
                      clampInt(mirrorBitrateMbps, 2, 64));
    } else {
        std::snprintf(buf, sizeof(buf), "--video-bit-rate=%dM",
                      clampInt(mirrorBitrateMbps, 2, 64));
    }
    std::string args(buf);
    if (mirrorMaxSize > 0) {
        char sz[48];
        std::snprintf(sz, sizeof(sz), " --max-size=%d", clampInt(mirrorMaxSize, 640, 4096));
        args += sz;
    }
    std::string safeOpts = sanitizeSafeToken(videoCodecOptions, true);
    if (!safeOpts.empty()) {
        args += " --video-codec-options=" + safeOpts;
    }
    if (!mirrorAudio) {
        args += " --no-audio";
    }
    return args;
}

void AppSettings::applyStreamPreset(StreamPresetId id) {
    if (id == PRESET_COMPETITIVE) {
        mirrorMaxSize = 1280;
        mirrorFps = 0;
        mirrorBitrateMbps = 16;
        videoCodecOptions = "bitrate-mode=2,max-bframes=0";
        vsyncEnabled = false;
        audioBufferMs = 25;
        audioRawCodec = true;
    } else if (id == PRESET_BALANCED) {
        mirrorMaxSize = 1600;
        mirrorFps = 0;
        mirrorBitrateMbps = 20;
        videoCodecOptions = "max-bframes=0";
        vsyncEnabled = false;
        audioBufferMs = 25;
        audioRawCodec = false;
    } else if (id == PRESET_QUALITY) {
        mirrorMaxSize = 0;
        mirrorFps = 0;
        mirrorBitrateMbps = 24;
        videoCodecOptions = "max-bframes=0";
        vsyncEnabled = false;
        audioBufferMs = 30;
        audioRawCodec = false;
    } else if (id == PRESET_WIFI) {
        mirrorMaxSize = 1280;
        mirrorFps = 60;
        mirrorBitrateMbps = 8;
        videoCodecOptions = "intra-refresh-period=60,bitrate-mode=2,max-bframes=0";
        vsyncEnabled = false;
        audioBufferMs = 30;
        audioRawCodec = false;
    }
}

StreamPresetId AppSettings::detectStreamPreset() const {
    if (mirrorMaxSize == 1280 && mirrorFps == 0 && mirrorBitrateMbps == 16 &&
        videoCodecOptions == "bitrate-mode=2,max-bframes=0" && !vsyncEnabled && audioRawCodec) {
        return PRESET_COMPETITIVE;
    }
    if (mirrorMaxSize == 1600 && mirrorFps == 0 && mirrorBitrateMbps == 20 &&
        videoCodecOptions == "max-bframes=0" && !vsyncEnabled && !audioRawCodec) {
        return PRESET_BALANCED;
    }
    if (mirrorMaxSize == 0 && mirrorFps == 0 && mirrorBitrateMbps == 24 &&
        videoCodecOptions == "max-bframes=0" && !vsyncEnabled && !audioRawCodec) {
        return PRESET_QUALITY;
    }
    if (mirrorMaxSize == 1280 && mirrorFps == 60 && mirrorBitrateMbps == 8 &&
        videoCodecOptions == "intra-refresh-period=60,bitrate-mode=2,max-bframes=0" && !vsyncEnabled) {
        return PRESET_WIFI;
    }
    return PRESET_CUSTOM;
}

const char* AppSettings::streamPresetName(StreamPresetId id) {
    switch (id) {
        case PRESET_COMPETITIVE: return "Competitive";
        case PRESET_BALANCED:    return "Balanced";
        case PRESET_QUALITY:     return "Quality";
        case PRESET_WIFI:        return "Wi-Fi";
        default:                 return "Custom";
    }
}

std::string AppSettings::encoderForDevice(const std::string& serial) const {
    if (serial.empty()) return "";
    std::string key = sanitizeSafeToken(serial, false);
    std::map<std::string, std::string>::const_iterator it = deviceVideoEncoders.find(key);
    return (it != deviceVideoEncoders.end()) ? it->second : std::string();
}

void AppSettings::setEncoderForDevice(const std::string& serial, const std::string& encoderName) {
    std::string key = sanitizeSafeToken(serial, false);
    if (key.empty()) return;
    std::string val = sanitizeSafeToken(encoderName, false);
    if (val.empty()) {
        deviceVideoEncoders.erase(key);
    } else {
        deviceVideoEncoders[key] = val;
    }
}

int AppSettings::countPendingStreamChanges(const AppSettings& live, const AppSettings& applied,
                                           const std::string& serial) {
    int count = 0;
    if (live.mirrorFps != applied.mirrorFps) ++count;
    if (live.mirrorBitrateMbps != applied.mirrorBitrateMbps) ++count;
    if (live.mirrorMaxSize != applied.mirrorMaxSize) ++count;
    if (live.videoCodecOptions != applied.videoCodecOptions) ++count;
    if (live.encoderForDevice(serial) != applied.encoderForDevice(serial)) ++count;
    if (live.useNativeVideo != applied.useNativeVideo) ++count;
    if (live.virtualDisplayMode != applied.virtualDisplayMode) ++count;
    if (live.virtualDisplayMode && live.virtualDisplaySpec != applied.virtualDisplaySpec) ++count;
    if (live.mirrorAudio != applied.mirrorAudio) ++count;
    if (live.mirrorAudio && live.audioBufferMs != applied.audioBufferMs) ++count;
    if (live.mirrorAudio && live.audioRawCodec != applied.audioRawCodec) ++count;
    if (live.stayAwake != applied.stayAwake) ++count;
    return count;
}

bool AppSettings::hasPendingStreamChanges(const AppSettings& live, const AppSettings& applied,
                                          const std::string& serial) {
    return countPendingStreamChanges(live, applied, serial) > 0;
}

bool AppSettingsStore::load(const std::string& path, AppSettings& outSettings) {
    std::ifstream in(path.c_str());
    if (!in.is_open()) return false;

    AppSettings s;
    std::string line;
    while (std::getline(in, line)) {
        size_t hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        line = trimLine(line);
        if (line.empty()) continue;

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trimLine(line.substr(0, eq));
        std::string v = trimLine(line.substr(eq + 1));

        if (k == "win_x") s.winX = std::atoi(v.c_str());
        else if (k == "win_y") s.winY = std::atoi(v.c_str());
        else if (k == "win_w") s.winW = clampInt(std::atoi(v.c_str()), 900, 3840);
        else if (k == "win_h") s.winH = clampInt(std::atoi(v.c_str()), 560, 2160);
        else if (k == "maximized") s.maximized = (v == "1");
        else if (k == "always_on_top") s.alwaysOnTop = (v == "1");
        else if (k == "mirror_fps") {
            int f = std::atoi(v.c_str());
            s.mirrorFps = (f <= 0) ? 0 : clampInt(f, 30, 144);
        }
        else if (k == "mirror_bitrate_mbps") s.mirrorBitrateMbps = clampInt(std::atoi(v.c_str()), 2, 64);
        else if (k == "mirror_max_size") s.mirrorMaxSize = clampInt(std::atoi(v.c_str()), 0, 4096);
        else if (k == "video_codec_options") s.videoCodecOptions = sanitizeSafeToken(v, true);
        else if (k.find("device_encoder.") == 0) {
            std::string ser = sanitizeSafeToken(k.substr(15), false);
            std::string enc = sanitizeSafeToken(v, false);
            if (!ser.empty() && !enc.empty()) s.deviceVideoEncoders[ser] = enc;
        }
        else if (k == "mirror_audio") s.mirrorAudio = (v == "1");
        else if (k == "turn_screen_off") s.turnScreenOff = (v == "1");
        else if (k == "stay_awake") s.stayAwake = (v == "1");
        else if (k == "auto_switch_profiles") s.autoSwitchProfiles = (v == "1");
        else if (k == "right_click_back") s.rightClickBack = (v == "1");
        else if (k == "use_native_video") s.useNativeVideo = (v == "1");
        else if (k == "vsync") s.vsyncEnabled = (v == "1");
        else if (k == "audio_buffer_ms") s.audioBufferMs = clampInt(std::atoi(v.c_str()), 15, 100);
        else if (k == "audio_raw_codec") s.audioRawCodec = (v == "1");
        else if (k == "virtual_display_mode") s.virtualDisplayMode = (v == "1");
        else if (k == "virtual_display_spec" && !v.empty()) s.virtualDisplaySpec = v;
        else if (k == "gamepad_enabled") s.gamepadEnabled = (v == "1");
        else if (k == "onboarding_done") s.onboardingDone = (v == "1");
        else if (k == "last_phone_w") s.lastPhoneW = clampInt(std::atoi(v.c_str()), 320, 4096);
        else if (k == "last_phone_h") s.lastPhoneH = clampInt(std::atoi(v.c_str()), 320, 4096);
        else if (k == "last_wireless_addr" && !v.empty()) s.lastWirelessAddr = v;
        else if (k == "last_game_profile" && !v.empty()) s.lastGameProfile = v;
    }

    outSettings = s;
    return true;
}

bool AppSettingsStore::save(const std::string& path, const AppSettings& s) {
    std::string tmpPath = path + ".tmp";
    {
        std::ofstream out(tmpPath.c_str(), std::ios::binary | std::ios::trunc);
        if (!out.is_open()) return false;
        out << "# Phantom Touch Emulator Shell Settings\n";
        out << "win_x = " << s.winX << "\n";
        out << "win_y = " << s.winY << "\n";
        out << "win_w = " << s.winW << "\n";
        out << "win_h = " << s.winH << "\n";
        out << "maximized = " << (s.maximized ? 1 : 0) << "\n";
        out << "always_on_top = " << (s.alwaysOnTop ? 1 : 0) << "\n";
        out << "mirror_fps = " << s.mirrorFps << "\n";
        out << "mirror_bitrate_mbps = " << s.mirrorBitrateMbps << "\n";
        out << "mirror_max_size = " << s.mirrorMaxSize << "\n";
        if (!s.videoCodecOptions.empty()) {
            out << "video_codec_options = " << sanitizeSafeToken(s.videoCodecOptions, true) << "\n";
        }
        for (std::map<std::string, std::string>::const_iterator it = s.deviceVideoEncoders.begin();
             it != s.deviceVideoEncoders.end(); ++it) {
            if (!it->first.empty() && !it->second.empty()) {
                out << "device_encoder." << it->first << " = " << it->second << "\n";
            }
        }
        out << "mirror_audio = " << (s.mirrorAudio ? 1 : 0) << "\n";
        out << "turn_screen_off = " << (s.turnScreenOff ? 1 : 0) << "\n";
        out << "stay_awake = " << (s.stayAwake ? 1 : 0) << "\n";
        out << "auto_switch_profiles = " << (s.autoSwitchProfiles ? 1 : 0) << "\n";
        out << "right_click_back = " << (s.rightClickBack ? 1 : 0) << "\n";
        out << "use_native_video = " << (s.useNativeVideo ? 1 : 0) << "\n";
        out << "vsync = " << (s.vsyncEnabled ? 1 : 0) << "\n";
        out << "audio_buffer_ms = " << clampInt(s.audioBufferMs, 15, 100) << "\n";
        out << "audio_raw_codec = " << (s.audioRawCodec ? 1 : 0) << "\n";
        out << "virtual_display_mode = " << (s.virtualDisplayMode ? 1 : 0) << "\n";
        out << "virtual_display_spec = " << s.virtualDisplaySpec << "\n";
        out << "gamepad_enabled = " << (s.gamepadEnabled ? 1 : 0) << "\n";
        out << "onboarding_done = " << (s.onboardingDone ? 1 : 0) << "\n";
        out << "last_phone_w = " << clampInt(s.lastPhoneW, 320, 4096) << "\n";
        out << "last_phone_h = " << clampInt(s.lastPhoneH, 320, 4096) << "\n";
        out << "last_wireless_addr = " << s.lastWirelessAddr << "\n";
        if (!s.lastGameProfile.empty()) {
            out << "last_game_profile = " << s.lastGameProfile << "\n";
        }
        out.flush();
        if (!out.good()) return false;
    }
    if (!MoveFileExA(tmpPath.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        bool ok = (CopyFileA(tmpPath.c_str(), path.c_str(), FALSE) != 0);
        DeleteFileA(tmpPath.c_str());
        return ok;
    }
    return true;
}
