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

} // namespace

std::string AppSettings::buildScrcpyArgs() const {
    // Only video/audio flags valid with scrcpy --no-control!
    // (stay_awake and turn_screen_off are handled via our control=true server bridge!)
    char buf[160];
    std::snprintf(buf, sizeof(buf), "--max-fps=%d --video-bit-rate=%dM",
                  clampInt(mirrorFps, 30, 144),
                  clampInt(mirrorBitrateMbps, 2, 64));
    std::string args(buf);
    if (mirrorMaxSize > 0) {
        char sz[48];
        std::snprintf(sz, sizeof(sz), " --max-size=%d", clampInt(mirrorMaxSize, 640, 4096));
        args += sz;
    }
    if (!mirrorAudio) {
        args += " --no-audio";
    }
    return args;
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
        else if (k == "mirror_fps") s.mirrorFps = clampInt(std::atoi(v.c_str()), 30, 144);
        else if (k == "mirror_bitrate_mbps") s.mirrorBitrateMbps = clampInt(std::atoi(v.c_str()), 2, 64);
        else if (k == "mirror_max_size") s.mirrorMaxSize = clampInt(std::atoi(v.c_str()), 0, 4096);
        else if (k == "mirror_audio") s.mirrorAudio = (v == "1");
        else if (k == "turn_screen_off") s.turnScreenOff = (v == "1");
        else if (k == "stay_awake") s.stayAwake = (v == "1");
        else if (k == "last_wireless_addr" && !v.empty()) s.lastWirelessAddr = v;
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
        out << "mirror_audio = " << (s.mirrorAudio ? 1 : 0) << "\n";
        out << "turn_screen_off = " << (s.turnScreenOff ? 1 : 0) << "\n";
        out << "stay_awake = " << (s.stayAwake ? 1 : 0) << "\n";
        out << "last_wireless_addr = " << s.lastWirelessAddr << "\n";
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
