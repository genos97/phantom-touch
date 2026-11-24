#include "profile.hpp"

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

std::string vkToName(int vk) {
    if (vk >= 'A' && vk <= 'Z') return std::string(1, static_cast<char>(vk));
    if (vk >= '0' && vk <= '9') return std::string(1, static_cast<char>(vk));
    switch (vk) {
        case 0:    return "NONE";
        case 0x01: return "LMB";
        case 0x02: return "RMB";
        case 0x04: return "MMB";
        case 0x05: return "X1";
        case 0x06: return "X2";
        case VK_CUSTOM_MWHEEL_UP:   return "MWHEELUP";
        case VK_CUSTOM_MWHEEL_DOWN: return "MWHEELDOWN";
        case 0x20: return "SPACE";
        case 0x09: return "TAB";
        case 0x0D: return "ENTER";
        case 0x1B: return "ESC";
        case 0x14: return "CAPS";
        case 0x10: return "LSHIFT";
        case 0x11: return "LCTRL";
        case 0x12: return "LALT";
        case 0xA0: return "LSHIFT";
        case 0xA1: return "RSHIFT";
        case 0xA2: return "LCTRL";
        case 0xA3: return "RCTRL";
        case 0xA4: return "LALT";
        case 0xA5: return "RALT";
        case 0xC0: return "TILDE";
        case 0x70: return "F1";
        case 0x71: return "F2";
        case 0x72: return "F3";
        case 0x73: return "F4";
        case 0x74: return "F5";
        case 0x75: return "F6";
        case 0x76: return "F7";
        case 0x77: return "F8";
        case 0x78: return "F9";
        case 0x79: return "F10";
        case 0x7A: return "F11";
        case 0x7B: return "F12";
        case 0x25: return "LEFT";
        case 0x26: return "UP";
        case 0x27: return "RIGHT";
        case 0x28: return "DOWN";
        default: {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "0x%02X", vk);
            return std::string(buf);
        }
    }
}

int nameToVk(const std::string& raw) {
    std::string s = toUpper(trim(raw));
    if (s.empty() || s == "NONE" || s == "0") return 0;
    if (s.size() == 1) {
        char c = s[0];
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            return static_cast<int>(c);
        }
        if (c == '~' || c == '`') return 0xC0;
    }
    if (s == "LMB" || s == "MOUSE1") return 0x01;
    if (s == "RMB" || s == "MOUSE2") return 0x02;
    if (s == "MMB" || s == "MOUSE3") return 0x04;
    if (s == "X1"  || s == "MOUSE4") return 0x05;
    if (s == "X2"  || s == "MOUSE5") return 0x06;
    if (s == "MWHEELUP" || s == "SCROLLUP" || s == "SCROLL_UP" || s == "WHEELUP" || s == "SCRL+") return VK_CUSTOM_MWHEEL_UP;
    if (s == "MWHEELDOWN" || s == "SCROLLDOWN" || s == "SCROLL_DOWN" || s == "WHEELDOWN" || s == "SCRL-") return VK_CUSTOM_MWHEEL_DOWN;
    if (s == "SPACE")  return 0x20;
    if (s == "TAB")    return 0x09;
    if (s == "ENTER")  return 0x0D;
    if (s == "ESC")    return 0x1B;
    if (s == "CAPS" || s == "CAPSLOCK") return 0x14;
    if (s == "SHIFT" || s == "LSHIFT") return 0xA0;
    if (s == "RSHIFT") return 0xA1;
    if (s == "CTRL"  || s == "LCTRL")  return 0xA2;
    if (s == "RCTRL")  return 0xA3;
    if (s == "ALT"   || s == "LALT")   return 0xA4;
    if (s == "RALT")   return 0xA5;
    if (s == "TILDE" || s == "GRAVE")  return 0xC0;
    if (s == "LEFT")   return 0x25;
    if (s == "UP")     return 0x26;
    if (s == "RIGHT")  return 0x27;
    if (s == "DOWN")   return 0x28;
    if (s.size() >= 2 && s[0] == 'F') {
        int fn = std::atoi(s.c_str() + 1);
        if (fn >= 1 && fn <= 12) return 0x70 + (fn - 1);
    }
    if (s.size() > 2 && s[0] == '0' && s[1] == 'X') {
        int v = static_cast<int>(std::strtol(s.c_str(), NULL, 16));
        if (v == 0x10) return 0xA0; // Heal legacy generic VK_SHIFT -> VK_LSHIFT
        if (v == 0x11) return 0xA2; // Heal legacy generic VK_CONTROL -> VK_LCONTROL
        if (v == 0x12) return 0xA4; // Heal legacy generic VK_MENU -> VK_LMENU
        return v;
    }
    return 0;
}

std::string comboToName(int comboVk, int vk) {
    if (comboVk != 0) {
        return vkToName(comboVk) + "+" + vkToName(vk);
    }
    return vkToName(vk);
}

void parseComboName(const std::string& spec, int& outComboVk, int& outVk) {
    outComboVk = 0;
    outVk = 0;
    size_t plus = spec.find('+');
    if (plus != std::string::npos) {
        outComboVk = nameToVk(spec.substr(0, plus));
        outVk = nameToVk(spec.substr(plus + 1));
    } else {
        outVk = nameToVk(spec);
    }
}

std::string actionTypeToName(KeyActionType a) {
    switch (a) {
        case NODE_PRESS:          return "PRESS";
        case NODE_CLICK:          return "CLICK";
        case NODE_DOWN_UP:        return "DOWN_UP";
        case NODE_UP_TRIGGER:     return "UP_TRIGGER";
        case NODE_HOLD_VIEW:      return "HOLD_VIEW";
        case NODE_SWIPE:          return "SWIPE";
        case NODE_CURSOR_TOGGLE:  return "CURSOR_TOGGLE";
        case NODE_RESET_VIEW:     return "RESET_VIEW";
        case NODE_SWITCH_SENS:    return "SWITCH_SENS";
        case NODE_REACTIVATE_JOY: return "REACTIVATE_JOY";
        default:                  return "PRESS";
    }
}

KeyActionType nameToActionType(const std::string& raw) {
    std::string s = toUpper(trim(raw));
    if (s == "CLICK" || s == "TAP")          return NODE_CLICK;
    if (s == "DOWN_UP" || s == "HOLD_ADS")   return NODE_DOWN_UP;
    if (s == "UP_TRIGGER" || s == "UP")      return NODE_UP_TRIGGER;
    if (s == "HOLD_VIEW" || s == "FREELOOK") return NODE_HOLD_VIEW;
    if (s == "SWIPE")                        return NODE_SWIPE;
    if (s == "CURSOR_TOGGLE" || s == "CURSOR" || s == "1" || s == "TRUE") return NODE_CURSOR_TOGGLE;
    if (s == "RESET_VIEW")                   return NODE_RESET_VIEW;
    if (s == "SWITCH_SENS")                  return NODE_SWITCH_SENS;
    if (s == "REACTIVATE_JOY")               return NODE_REACTIVATE_JOY;
    return NODE_PRESS;
}

Profile createDefaultFreeFireProfile() {
    Profile p;
    p.name = "Free Fire MAX (S21 FE)";
    p.scrcpyArgs = "--max-fps=90 --video-bit-rate=16M";
    p.controlMaxSize = 0;
    p.pollHz = 250;

    p.toggleAimVk = 0xC0; // ~
    p.holdUnlockVk = 0;
    p.resetTouchVk = 0x74; // F5
    p.showHudInPlay = true;

    p.aimX = 0.620f;
    p.aimY = 0.700f;
    p.aimHalfW = 0.160f;
    p.aimHalfH = 0.640f;

    p.unboundedAim = true;
    p.invertY = false;
    p.ffCounterAccel = 7.0f;
    p.sensX = 0.95f;
    p.sensY = 1.15f;
    p.fireSensX = 0.80f;
    p.fireSensY = 1.25f;
    p.scopeSensX = 0.65f;
    p.scopeSensY = 0.75f;
    p.accelPower = 1.00f;
    p.accelRefSpeed = 14.0f;
    p.accelCap = 2.00f;
    p.smoothing = 0.10f;
    p.idleResetMs = 700;
    p.touchDownDelayMs = 15;
    p.touchUpDelayMs = 0;
    p.tapJitterPx = 4;

    p.fireMode = FIRE_MODE_CLAW_LEFT;
    p.fireVk = 0x01; // LMB
    p.fireX = 0.851f;
    p.fireY = 0.728f;
    p.fireBoxHalfW = 0.350f;
    p.fireBoxHalfH = 0.720f;
    p.leftFireX = 0.236f;
    p.leftFireY = 0.428f;

    p.joyX = 0.162f;
    p.joyY = 0.750f;
    p.joyRadius = 0.085f;
    p.joySprintRadius = 0.160f;
    p.joyWalkRadius = 0.042f;
    p.joySteppedGlide = true;
    p.vkUp = 'W';
    p.vkDown = 'S';
    p.vkLeft = 'A';
    p.vkRight = 'D';
    p.vkSprint = 0xA0; // LSHIFT
    p.vkWalk = 0xA2;   // LCTRL

    auto addNode = [&](const char* label, int comboVk, int vk, KeyActionType act,
                       float x, float y, bool scopeSens = false, int durMs = 35) {
        KeyNode n;
        n.label = label;
        n.comboVk = comboVk;
        n.vk = vk;
        n.action = act;
        n.x = x;
        n.y = y;
        n.x2 = x;
        n.y2 = y;
        n.durationMs = durMs;
        n.radius = 0.030f;
        n.switchScopeSens = scopeSens;
        p.buttons.push_back(n);
    };

    // Calibrated to Samsung S21 FE 2340x1080 Free Fire MAX HUD
    addNode("SCOPE",    0, 0x02, NODE_PRESS,         0.931f, 0.495f, true);  // RMB (also toggles scope sens!)
    addNode("JUMP",     0, 0x20, NODE_PRESS,         0.941f, 0.658f);        // SPACE
    addNode("CROUCH",   0, 'C',  NODE_PRESS,         0.883f, 0.892f);
    addNode("PRONE",    0, 'Z',  NODE_PRESS,         0.793f, 0.892f);
    addNode("RELOAD",   0, 'R',  NODE_CLICK,         0.880f, 0.085f);
    addNode("GLOO",     0, 'E',  NODE_PRESS,         0.270f, 0.855f);
    addNode("MEDKIT",   0, '4',  NODE_CLICK,         0.081f, 0.648f);
    addNode("WPN1",     0, '1',  NODE_CLICK,         0.880f, 0.085f);
    addNode("WPN2",     0, '2',  NODE_CLICK,         0.840f, 0.215f);
    addNode("MELEE",    0, '3',  NODE_CLICK,         0.932f, 0.215f);
    addNode("SWITCH",   0, 'Q',  NODE_CLICK,         0.770f, 0.085f);
    addNode("LOOT",     0, 'F',  NODE_CLICK,         0.720f, 0.340f);
    addNode("SPRINT",   0, 0x14, NODE_CLICK,         0.128f, 0.422f);        // CAPS
    addNode("USE",      0, 'G',  NODE_PRESS,         0.650f, 0.670f);
    addNode("FREELOOK", 0, 0xA4, NODE_HOLD_VIEW,     0.775f, 0.275f);        // LALT -> temporary Free Look Eye drag!
    addNode("BAG",      0, 0x09, NODE_CURSOR_TOGGLE, 0.081f, 0.542f);        // TAB -> Bag + Cursor Mode
    addNode("MAP",      0, 'M',  NODE_CURSOR_TOGGLE, 0.095f, 0.140f);        // M   -> Map + Cursor Mode
    addNode("RSTVIEW",  0, 'V',  NODE_RESET_VIEW,    0.500f, 0.150f);        // V   -> Clean View Reset

    return p;
}

Profile createBlankProfile(const std::string& name) {
    Profile p = createDefaultFreeFireProfile();
    p.name = name.empty() ? "Custom Keymap" : name;
    p.buttons.clear(); // Starts with just WASD, AIM, FIRE so user builds their own layout freely!
    return p;
}

std::vector<std::string> listProfileFiles(const std::string& profilesDir) {
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

std::string sanitizeProfileFilename(const std::string& name) {
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

std::string loadLastActiveProfilePath(const std::string& profilesDir) {
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

void saveLastActiveProfilePath(const std::string& profilesDir, const std::string& fullPath) {
    std::ofstream out((profilesDir + "\\.active_profile").c_str());
    if (out.is_open()) {
        out << fullPath << "\n";
    }
}

bool loadProfile(const std::string& path, Profile& outProfile) {
    std::ifstream in(path.c_str());
    if (!in.is_open()) return false;

    Profile p = createDefaultFreeFireProfile();
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
        else if (key == "toggle_aim_key") { int vk = nameToVk(val); if (vk) p.toggleAimVk = vk; }
        else if (key == "hold_unlock_key") { p.holdUnlockVk = nameToVk(val); }
        else if (key == "reset_touch_key") { int vk = nameToVk(val); if (vk) p.resetTouchVk = vk; }
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
        else if (key == "fire_key") { int vk = nameToVk(val); if (vk) p.fireVk = vk; }
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
        else if (key == "joy_up") { int vk = nameToVk(val); if (vk) p.vkUp = vk; }
        else if (key == "joy_down") { int vk = nameToVk(val); if (vk) p.vkDown = vk; }
        else if (key == "joy_left") { int vk = nameToVk(val); if (vk) p.vkLeft = vk; }
        else if (key == "joy_right") { int vk = nameToVk(val); if (vk) p.vkRight = vk; }
        else if (key == "joy_sprint") { int vk = nameToVk(val); if (vk) p.vkSprint = vk; }
        else if (key == "joy_walk") { p.vkWalk = nameToVk(val); }

        else if (key == "button") {
            // Supports both legacy:
            //   button = LABEL, KEY, X, Y, 0|1
            // and extended v3 format:
            //   button = LABEL, KEY[or K1+K2], X, Y, ACTION[, SCOPE_SENS(0/1), DUR_MS, X2, Y2]
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
                parseComboName(parts[1], n.comboVk, n.vk);
                n.x = clampf(static_cast<float>(std::atof(parts[2].c_str())), 0.01f, 0.99f);
                n.y = clampf(static_cast<float>(std::atof(parts[3].c_str())), 0.01f, 0.99f);
                n.x2 = n.x;
                n.y2 = n.y;
                n.radius = 0.030f;
                if (parts.size() >= 5) {
                    n.action = nameToActionType(parts[4]);
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

bool saveProfile(const std::string& path, const Profile& p) {
    std::ofstream out(path.c_str());
    if (!out.is_open()) return false;

    char buf[384];
    out << "# Scrcpy Keymapper Pro v3 Profile\n";
    out << "# All coordinates are normalized (0.0 = top/left, 1.0 = bottom/right)\n";
    out << "# Press F2 in-app to drag & drop nodes, T to cycle node Action Type, Ctrl+S to save.\n\n";

    out << "name = " << p.name << "\n";
    out << "scrcpy_args = " << p.scrcpyArgs << "\n";
    out << "control_max_size = " << p.controlMaxSize << "\n";
    out << "poll_hz = " << p.pollHz << "\n";
    out << "toggle_aim_key = " << vkToName(p.toggleAimVk) << "\n";
    out << "hold_unlock_key = " << vkToName(p.holdUnlockVk) << "\n";
    out << "reset_touch_key = " << vkToName(p.resetTouchVk) << "\n";
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
    out << "fire_key = " << vkToName(p.fireVk) << "\n";
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
    out << "joy_up = " << vkToName(p.vkUp) << "\n";
    out << "joy_down = " << vkToName(p.vkDown) << "\n";
    out << "joy_left = " << vkToName(p.vkLeft) << "\n";
    out << "joy_right = " << vkToName(p.vkRight) << "\n";
    out << "joy_sprint = " << vkToName(p.vkSprint) << "\n";
    out << "joy_walk = " << vkToName(p.vkWalk) << "\n\n";

    out << "# --- Action Nodes: LABEL, KEY[or K1+K2], X, Y, ACTION, SCOPE_SENS(0/1), DUR_MS[, X2, Y2] ---\n";
    out << "# Actions: PRESS, CLICK, DOWN_UP, UP_TRIGGER, HOLD_VIEW, SWIPE, CURSOR_TOGGLE, RESET_VIEW, SWITCH_SENS, REACTIVATE_JOY\n";
    for (size_t i = 0; i < p.buttons.size(); ++i) {
        const KeyNode& b = p.buttons[i];
        std::string keySpec = comboToName(b.comboVk, b.vk);
        if (b.action == NODE_SWIPE) {
            std::snprintf(buf, sizeof(buf), "button = %s, %s, %.4f, %.4f, %s, %d, %d, %.4f, %.4f\n",
                          b.label.c_str(), keySpec.c_str(), b.x, b.y,
                          actionTypeToName(b.action).c_str(),
                          b.switchScopeSens ? 1 : 0, b.durationMs, b.x2, b.y2);
        } else {
            std::snprintf(buf, sizeof(buf), "button = %s, %s, %.4f, %.4f, %s, %d, %d\n",
                          b.label.c_str(), keySpec.c_str(), b.x, b.y,
                          actionTypeToName(b.action).c_str(),
                          b.switchScopeSens ? 1 : 0, b.durationMs);
        }
        out << buf;
    }

    return out.good();
}
