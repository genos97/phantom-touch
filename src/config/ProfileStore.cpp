#include "config/ProfileStore.hpp"

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace {

std::string trim(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) ++start;
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
    return s.substr(start, end - start);
}

std::string toUpper(std::string s) {
    for (size_t i = 0; i < s.size(); ++i) {
        s[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[i])));
    }
    return s;
}

float clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

int clampi(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

} // namespace

std::vector<std::string> ProfileStore::listProfiles(const std::string& profilesDir) {
    std::vector<std::string> result;
    WIN32_FIND_DATAA fd;
    std::string pattern = profilesDir + "\\*.cfg";
    HANDLE hFind = FindFirstFileA(pattern.c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                std::string fname = fd.cFileName;
                if (!fname.empty() && fname[0] != '_') {
                    result.push_back(profilesDir + "\\" + fname);
                }
            }
        } while (FindNextFileA(hFind, &fd));
        FindClose(hFind);
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::string ProfileStore::sanitizeFilename(const std::string& name) {
    std::string out;
    for (size_t i = 0; i < name.size(); ++i) {
        char c = name[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_') {
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        } else if (c == ' ') {
            out.push_back('_');
        }
    }
    if (out.empty()) out = "custom";
    return out;
}

std::string ProfileStore::loadLastActive(const std::string& profilesDir) {
    std::ifstream in((profilesDir + "\\.active_profile").c_str());
    std::string line;
    if (in.is_open() && std::getline(in, line)) {
        line = trim(line);
        DWORD attr = GetFileAttributesA(line.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
            return line;
        }
    }
    return "";
}

void ProfileStore::saveLastActive(const std::string& profilesDir, const std::string& fullPath) {
    std::ofstream out((profilesDir + "\\.active_profile").c_str());
    if (out.is_open()) {
        out << fullPath << "\n";
    }
}

bool ProfileStore::load(const std::string& path, Profile& outProfile) {
    std::ifstream in(path.c_str());
    if (!in.is_open()) return false;

    Profile p = ProfileDefaults::createFreeFire();
    bool sawAnyButton = false;

    std::string line;
    while (std::getline(in, line)) {
        size_t hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        line = trim(line);
        if (line.empty()) continue;

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));

        if (key == "name") p.name = val;
        else if (key == "scrcpy_args") p.scrcpyArgs = val;
        else if (key == "control_max_size") p.controlMaxSize = clampi(std::atoi(val.c_str()), 0, 4096);
        else if (key == "poll_hz") p.pollHz = clampi(std::atoi(val.c_str()), 0, 1000);
        else if (key == "toggle_aim_key") { int vk = KeyCodes::fromName(val); if (vk) p.toggleAimVk = vk; }
        else if (key == "hold_unlock_key") { p.holdUnlockVk = KeyCodes::fromName(val); }
        else if (key == "reset_touch_key") { int vk = KeyCodes::fromName(val); if (vk) p.resetTouchVk = vk; }
        else if (key == "show_hud_in_play") p.showHudInPlay = (val == "1" || toUpper(val) == "TRUE");
        else if (key == "hud_opacity") p.hudOpacity = clampf(static_cast<float>(std::atof(val.c_str())), 0.15f, 1.00f);
        else if (key == "float_btn_x") p.floatBtnX = clampf(static_cast<float>(std::atof(val.c_str())), 0.02f, 0.98f);
        else if (key == "float_btn_y") p.floatBtnY = clampf(static_cast<float>(std::atof(val.c_str())), 0.05f, 0.95f);
        else if (key == "panel_x") p.panelX = clampf(static_cast<float>(std::atof(val.c_str())), 0.01f, 0.85f);
        else if (key == "panel_y") p.panelY = clampf(static_cast<float>(std::atof(val.c_str())), 0.02f, 0.80f);

        else if (key == "aim_x") p.aimX = clampf(static_cast<float>(std::atof(val.c_str())), 0.05f, 0.95f);
        else if (key == "aim_y") p.aimY = clampf(static_cast<float>(std::atof(val.c_str())), 0.05f, 0.95f);
        else if (key == "aim_half_w") p.aimHalfW = clampf(static_cast<float>(std::atof(val.c_str())), 0.04f, 0.48f);
        else if (key == "aim_half_h") p.aimHalfH = clampf(static_cast<float>(std::atof(val.c_str())), 0.04f, 0.85f);

        else if (key == "unbounded_aim") p.unboundedAim = (val == "1" || toUpper(val) == "TRUE");
        else if (key == "invert_y") p.invertY = (val == "1" || toUpper(val) == "TRUE");
        else if (key == "ff_counter_accel") p.ffCounterAccel = clampf(static_cast<float>(std::atof(val.c_str())), 0.0f, 25.0f);
        else if (key == "sens_x") p.sensX = clampf(static_cast<float>(std::atof(val.c_str())), 0.05f, 20.0f);
        else if (key == "sens_y") p.sensY = clampf(static_cast<float>(std::atof(val.c_str())), 0.05f, 20.0f);
        else if (key == "fire_sens_x") p.fireSensX = clampf(static_cast<float>(std::atof(val.c_str())), 0.10f, 5.0f);
        else if (key == "fire_sens_y") p.fireSensY = clampf(static_cast<float>(std::atof(val.c_str())), 0.10f, 5.0f);
        else if (key == "scope_sens_x") p.scopeSensX = clampf(static_cast<float>(std::atof(val.c_str())), 0.10f, 5.0f);
        else if (key == "scope_sens_y") p.scopeSensY = clampf(static_cast<float>(std::atof(val.c_str())), 0.10f, 5.0f);
        else if (key == "accel_power") p.accelPower = clampf(static_cast<float>(std::atof(val.c_str())), 1.00f, 2.50f);
        else if (key == "accel_ref_speed") p.accelRefSpeed = clampf(static_cast<float>(std::atof(val.c_str())), 1.0f, 200.0f);
        else if (key == "accel_cap") p.accelCap = clampf(static_cast<float>(std::atof(val.c_str())), 1.0f, 5.0f);
        else if (key == "smoothing") p.smoothing = clampf(static_cast<float>(std::atof(val.c_str())), 0.0f, 0.85f);
        else if (key == "idle_reset_ms") p.idleResetMs = clampi(std::atoi(val.c_str()), 0, 5000);
        else if (key == "touch_down_delay_ms") p.touchDownDelayMs = clampi(std::atoi(val.c_str()), 0, 100);
        else if (key == "touch_up_delay_ms") p.touchUpDelayMs = clampi(std::atoi(val.c_str()), 0, 100);
        else if (key == "tap_jitter_px") p.tapJitterPx = clampi(std::atoi(val.c_str()), 0, 20);

        else if (key == "fire_mode") p.fireMode = clampi(std::atoi(val.c_str()), 0, 2);
        else if (key == "fire_drag_mode") p.fireMode = (val == "1" || toUpper(val) == "TRUE") ? FIRE_MODE_DRAG_RIGHT : FIRE_MODE_CLAW_LEFT;
        else if (key == "fire_key") { int vk = KeyCodes::fromName(val); if (vk) p.fireVk = vk; }
        else if (key == "fire_x") p.fireX = clampf(static_cast<float>(std::atof(val.c_str())), 0.02f, 0.98f);
        else if (key == "fire_y") p.fireY = clampf(static_cast<float>(std::atof(val.c_str())), 0.02f, 0.98f);
        else if (key == "fire_box_half_w") p.fireBoxHalfW = clampf(static_cast<float>(std::atof(val.c_str())), 0.05f, 0.85f);
        else if (key == "fire_box_half_h") p.fireBoxHalfH = clampf(static_cast<float>(std::atof(val.c_str())), 0.05f, 0.95f);
        else if (key == "left_fire_x") p.leftFireX = clampf(static_cast<float>(std::atof(val.c_str())), 0.02f, 0.98f);
        else if (key == "left_fire_y") p.leftFireY = clampf(static_cast<float>(std::atof(val.c_str())), 0.02f, 0.98f);

        else if (key == "joy_x") p.joyX = clampf(static_cast<float>(std::atof(val.c_str())), 0.05f, 0.95f);
        else if (key == "joy_y") p.joyY = clampf(static_cast<float>(std::atof(val.c_str())), 0.05f, 0.95f);
        else if (key == "joy_radius") p.joyRadius = clampf(static_cast<float>(std::atof(val.c_str())), 0.02f, 0.30f);
        else if (key == "joy_sprint_radius") p.joySprintRadius = clampf(static_cast<float>(std::atof(val.c_str())), 0.03f, 0.40f);
        else if (key == "joy_walk_radius") p.joyWalkRadius = clampf(static_cast<float>(std::atof(val.c_str())), 0.015f, 0.15f);
        else if (key == "joy_stepped_glide") p.joySteppedGlide = (val == "1" || toUpper(val) == "TRUE");
        else if (key == "joy_up") { int vk = KeyCodes::fromName(val); if (vk) p.vkUp = vk; }
        else if (key == "joy_down") { int vk = KeyCodes::fromName(val); if (vk) p.vkDown = vk; }
        else if (key == "joy_left") { int vk = KeyCodes::fromName(val); if (vk) p.vkLeft = vk; }
        else if (key == "joy_right") { int vk = KeyCodes::fromName(val); if (vk) p.vkRight = vk; }
        else if (key == "joy_sprint") { int vk = KeyCodes::fromName(val); if (vk) p.vkSprint = vk; }
        else if (key == "joy_walk") { p.vkWalk = KeyCodes::fromName(val); }

        else if (key == "button") {
            if (!sawAnyButton) {
                p.buttons.clear();
                sawAnyButton = true;
            }
            std::vector<std::string> parts;
            std::stringstream ss(val);
            std::string item;
            while (std::getline(ss, item, ',')) {
                parts.push_back(trim(item));
            }
            if (parts.size() >= 4) {
                KeyNode n;
                n.label = parts[0];
                KeyCodes::parseCombo(parts[1], n.comboVk, n.vk);
                n.x = clampf(static_cast<float>(std::atof(parts[2].c_str())), 0.01f, 0.99f);
                n.y = clampf(static_cast<float>(std::atof(parts[3].c_str())), 0.01f, 0.99f);
                n.x2 = n.x;
                n.y2 = n.y;
                n.radius = 0.030f;
                if (parts.size() >= 5) {
                    n.action = ProfileDefaults::nameToActionType(parts[4]);
                }
                if (parts.size() >= 6) {
                    std::string sf = toUpper(parts[5]);
                    n.switchScopeSens = (sf == "1" || sf == "TRUE" || sf == "SCOPE");
                    n.toggleMode = (sf == "TOGGLE");
                }
                if (parts.size() >= 7) {
                    n.durationMs = clampi(std::atoi(parts[6].c_str()), 10, 2000);
                }
                if (parts.size() >= 9) {
                    n.x2 = clampf(static_cast<float>(std::atof(parts[7].c_str())), 0.01f, 0.99f);
                    n.y2 = clampf(static_cast<float>(std::atof(parts[8].c_str())), 0.01f, 0.99f);
                }
                if (n.vk != 0) {
                    p.buttons.push_back(n);
                }
            }
        }
    }

    outProfile = p;
    return true;
}

bool ProfileStore::save(const std::string& path, const Profile& p) {
    std::ofstream out(path.c_str());
    if (!out.is_open()) return false;

    char buf[384];
    out << "# Phantom Touch v3 Profile\n";
    out << "# All coordinates are normalized (0.0 = top/left, 1.0 = bottom/right)\n";
    out << "# Press F2 in-app to drag & drop nodes, T to cycle node Action Type, Ctrl+S to save.\n\n";

    out << "name = " << p.name << "\n";
    out << "scrcpy_args = " << p.scrcpyArgs << "\n";
    out << "control_max_size = " << p.controlMaxSize << "\n";
    out << "poll_hz = " << p.pollHz << "\n";
    out << "toggle_aim_key = " << KeyCodes::toName(p.toggleAimVk) << "\n";
    out << "hold_unlock_key = " << KeyCodes::toName(p.holdUnlockVk) << "\n";
    out << "reset_touch_key = " << KeyCodes::toName(p.resetTouchVk) << "\n";
    out << "show_hud_in_play = " << (p.showHudInPlay ? 1 : 0) << "\n";
    std::snprintf(buf, sizeof(buf), "hud_opacity = %.2f\nfloat_btn_x = %.4f\nfloat_btn_y = %.4f\npanel_x = %.4f\npanel_y = %.4f\n\n",
                  p.hudOpacity, p.floatBtnX, p.floatBtnY, p.panelX, p.panelY);
    out << buf;

    out << "# --- Aim, View Stack & Free Fire Counter-Acceleration Physics ---\n";
    std::snprintf(buf, sizeof(buf), "aim_x = %.4f\naim_y = %.4f\naim_half_w = %.4f\naim_half_h = %.4f\n",
                  p.aimX, p.aimY, p.aimHalfW, p.aimHalfH);
    out << buf;
    out << "unbounded_aim = " << (p.unboundedAim ? 1 : 0) << "\n";
    out << "invert_y = " << (p.invertY ? 1 : 0) << "\n";
    std::snprintf(buf, sizeof(buf), "ff_counter_accel = %.2f\n", p.ffCounterAccel);
    out << buf;
    std::snprintf(buf, sizeof(buf), "sens_x = %.3f\nsens_y = %.3f\nfire_sens_x = %.3f\nfire_sens_y = %.3f\nscope_sens_x = %.3f\nscope_sens_y = %.3f\n",
                  p.sensX, p.sensY, p.fireSensX, p.fireSensY, p.scopeSensX, p.scopeSensY);
    out << buf;
    std::snprintf(buf, sizeof(buf), "accel_power = %.3f\naccel_ref_speed = %.2f\naccel_cap = %.2f\nsmoothing = %.3f\n",
                  p.accelPower, p.accelRefSpeed, p.accelCap, p.smoothing);
    out << buf;
    std::snprintf(buf, sizeof(buf), "idle_reset_ms = %d\ntouch_down_delay_ms = %d\ntouch_up_delay_ms = %d\ntap_jitter_px = %d\n\n",
                  p.idleResetMs, p.touchDownDelayMs, p.touchUpDelayMs, p.tapJitterPx);
    out << buf;

    out << "# --- Fire Button (0 = DRAG-FIRE Right, 1 = CLAW-LEFT Separate, 2 = DUAL-DRAG) ---\n";
    out << "fire_mode = " << p.fireMode << "\n";
    out << "fire_key = " << KeyCodes::toName(p.fireVk) << "\n";
    std::snprintf(buf, sizeof(buf), "fire_x = %.4f\nfire_y = %.4f\nfire_box_half_w = %.4f\nfire_box_half_h = %.4f\n",
                  p.fireX, p.fireY, p.fireBoxHalfW, p.fireBoxHalfH);
    out << buf;
    std::snprintf(buf, sizeof(buf), "left_fire_x = %.4f\nleft_fire_y = %.4f\n\n",
                  p.leftFireX, p.leftFireY);
    out << buf;

    out << "# --- WASD Virtual Joystick (with Free Fire 3-Stage Stepped Glide + Sprint/Walk Radius) ---\n";
    std::snprintf(buf, sizeof(buf), "joy_x = %.4f\njoy_y = %.4f\njoy_radius = %.4f\njoy_sprint_radius = %.4f\njoy_walk_radius = %.4f\n",
                  p.joyX, p.joyY, p.joyRadius, p.joySprintRadius, p.joyWalkRadius);
    out << buf;
    out << "joy_stepped_glide = " << (p.joySteppedGlide ? 1 : 0) << "\n";
    out << "joy_up = " << KeyCodes::toName(p.vkUp) << "\n";
    out << "joy_down = " << KeyCodes::toName(p.vkDown) << "\n";
    out << "joy_left = " << KeyCodes::toName(p.vkLeft) << "\n";
    out << "joy_right = " << KeyCodes::toName(p.vkRight) << "\n";
    out << "joy_sprint = " << KeyCodes::toName(p.vkSprint) << "\n";
    out << "joy_walk = " << KeyCodes::toName(p.vkWalk) << "\n\n";

    out << "# --- Action Nodes: LABEL, KEY[or K1+K2], X, Y, ACTION, SCOPE_SENS(0/1), DUR_MS[, X2, Y2] ---\n";
    out << "# Actions: PRESS, CLICK, DOWN_UP, UP_TRIGGER, HOLD_VIEW, SWIPE, CURSOR_TOGGLE, RESET_VIEW, SWITCH_SENS, REACTIVATE_JOY\n";
    for (size_t i = 0; i < p.buttons.size(); ++i) {
        const KeyNode& b = p.buttons[i];
        std::string keySpec = KeyCodes::comboToName(b.comboVk, b.vk);
        if (b.action == NODE_SWIPE) {
            std::snprintf(buf, sizeof(buf), "button = %s, %s, %.4f, %.4f, %s, %d, %d, %.4f, %.4f\n",
                          b.label.c_str(), keySpec.c_str(), b.x, b.y,
                          ProfileDefaults::actionTypeToName(b.action).c_str(),
                          b.switchScopeSens ? 1 : 0, b.durationMs, b.x2, b.y2);
        } else {
            std::snprintf(buf, sizeof(buf), "button = %s, %s, %.4f, %.4f, %s, %d, %d\n",
                          b.label.c_str(), keySpec.c_str(), b.x, b.y,
                          ProfileDefaults::actionTypeToName(b.action).c_str(),
                          b.switchScopeSens ? 1 : 0, b.durationMs);
        }
        out << buf;
    }

    return out.good();
}
