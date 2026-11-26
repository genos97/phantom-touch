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

std::string escapeCfgField(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '\\' || c == ',' || c == '#') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    return out;
}

std::string unescapeCfgField(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char nxt = s[i + 1];
            if (nxt == '\\' || nxt == ',' || nxt == '#') {
                out.push_back(nxt);
                ++i;
                continue;
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

std::string stripUnescapedComment(const std::string& line) {
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '\\' && i + 1 < line.size()) {
            ++i;
            continue;
        }
        if (line[i] == '#') {
            return line.substr(0, i);
        }
    }
    return line;
}

std::vector<std::string> splitUnescapedComma(const std::string& val) {
    std::vector<std::string> parts;
    std::string cur;
    for (size_t i = 0; i < val.size(); ++i) {
        if (val[i] == '\\' && i + 1 < val.size()) {
            cur.push_back(val[i]);
            cur.push_back(val[i + 1]);
            ++i;
            continue;
        }
        if (val[i] == ',') {
            parts.push_back(trim(cur));
            cur.clear();
            continue;
        }
        cur.push_back(val[i]);
    }
    parts.push_back(trim(cur));
    return parts;
}

std::string baseNameNoExt(const std::string& path) {
    size_t sl = path.find_last_of("\\/");
    std::string fname = (sl == std::string::npos) ? path : path.substr(sl + 1);
    size_t dot = fname.find_last_of('.');
    return (dot == std::string::npos) ? fname : fname.substr(0, dot);
}

std::wstring utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return std::wstring();
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), NULL, 0);
    if (wlen <= 0) {
        std::wstring fb;
        for (size_t i = 0; i < utf8.size(); ++i) fb.push_back(static_cast<wchar_t>(static_cast<unsigned char>(utf8[i])));
        return fb;
    }
    std::wstring ws(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), &ws[0], wlen);
    return ws;
}

bool readFileUtf8(const std::string& utf8Path, std::string& outText, std::string* outErr) {
    outText.clear();
    std::wstring wp = utf8ToWide(utf8Path);
    if (wp.empty()) {
        if (outErr) *outErr = "Empty file path.";
        return false;
    }
    HANDLE hf = CreateFileW(wp.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) {
        if (outErr) *outErr = "Could not open file: " + utf8Path;
        return false;
    }
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(hf, &sz) || sz.QuadPart <= 0 || sz.QuadPart > (2 * 1024 * 1024)) {
        CloseHandle(hf);
        if (outErr) *outErr = "Profile file is empty or exceeds 2 MB.";
        return false;
    }
    outText.resize(static_cast<size_t>(sz.QuadPart));
    DWORD totalRead = 0;
    while (totalRead < static_cast<DWORD>(outText.size())) {
        DWORD chunk = 0;
        if (!ReadFile(hf, &outText[totalRead], static_cast<DWORD>(outText.size()) - totalRead, &chunk, NULL) || chunk == 0) {
            break;
        }
        totalRead += chunk;
    }
    CloseHandle(hf);
    outText.resize(totalRead);
    return !outText.empty();
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
                if (!fname.empty() && fname[0] != '_' && fname[0] != '.') {
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
    std::string targetPath = profilesDir + "\\.active_profile";
    std::string tmpPath = targetPath + ".tmp";
    {
        std::ofstream out(tmpPath.c_str(), std::ios::binary | std::ios::trunc);
        if (!out.is_open()) return;
        out << fullPath << "\n";
        out.flush();
        if (!out.good()) return;
    }
    if (!MoveFileExA(tmpPath.c_str(), targetPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        std::ofstream fallback(targetPath.c_str(), std::ios::binary | std::ios::trunc);
        if (fallback.is_open()) {
            fallback << fullPath << "\n";
        }
        DeleteFileA(tmpPath.c_str());
    }
}

bool ProfileStore::parseFromString(const std::string& text, Profile& outProfile, std::string* outError) {
    if (text.empty()) {
        if (outError) *outError = "Profile text is empty.";
        return false;
    }

    Profile p = ProfileDefaults::createFreeFire();
    p.appPackage.clear(); // Only auto-link if explicitly configured in the .cfg file
    bool sawAnyButton = false;
    bool sawKnownKey = false;

    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.find("# phantom-touch-profile v") == 0) sawKnownKey = true;
        line = trim(stripUnescapedComment(line));
        if (line.empty()) continue;

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));

        if (key == "name") { p.name = unescapeCfgField(val); sawKnownKey = true; }
        else if (key == "app_package" || key == "package") { p.appPackage = val; sawKnownKey = true; }
        else if (key == "keymap_enabled") { p.keymapEnabled = (val == "1" || toUpper(val) == "TRUE"); sawKnownKey = true; }
        else if (key == "edge_anchored") { p.edgeAnchored = (val == "1" || toUpper(val) == "TRUE"); sawKnownKey = true; }
        else if (key == "ref_aspect") { p.refAspect = clampf(static_cast<float>(std::atof(val.c_str())), 0.40f, 3.50f); sawKnownKey = true; }
        else if (key == "scrcpy_args") { p.scrcpyArgs = val; sawKnownKey = true; }
        else if (key == "control_max_size") p.controlMaxSize = clampi(std::atoi(val.c_str()), 0, 4096);
        else if (key == "poll_hz") { p.pollHz = clampi(std::atoi(val.c_str()), 0, 1000); sawKnownKey = true; }
        else if (key == "toggle_aim_key") { int vk = KeyCodes::fromName(val); if (vk) p.toggleAimVk = vk; sawKnownKey = true; }
        else if (key == "hold_unlock_key") { p.holdUnlockVk = KeyCodes::fromName(val); }
        else if (key == "reset_touch_key") { int vk = KeyCodes::fromName(val); if (vk) p.resetTouchVk = vk; }
        else if (key == "show_hud_in_play") p.showHudInPlay = (val == "1" || toUpper(val) == "TRUE");
        else if (key == "hud_opacity") p.hudOpacity = clampf(static_cast<float>(std::atof(val.c_str())), 0.15f, 1.00f);
        else if (key == "float_btn_x") p.floatBtnX = clampf(static_cast<float>(std::atof(val.c_str())), 0.02f, 0.98f);
        else if (key == "float_btn_y") p.floatBtnY = clampf(static_cast<float>(std::atof(val.c_str())), 0.05f, 0.95f);
        else if (key == "panel_x") p.panelX = clampf(static_cast<float>(std::atof(val.c_str())), 0.01f, 0.85f);
        else if (key == "panel_y") p.panelY = clampf(static_cast<float>(std::atof(val.c_str())), 0.02f, 0.80f);
        else if (key == "cursor_remember_pos") p.rememberCursorPos = (val == "1" || toUpper(val) == "TRUE");
        else if (key == "cursor_scroll_step_px") p.cursorScrollStepPx = clampi(std::atoi(val.c_str()), 20, 400);
        else if (key == "cast_center_x") { p.castCenterX = clampf(static_cast<float>(std::atof(val.c_str())), 0.05f, 0.95f); sawKnownKey = true; }
        else if (key == "cast_center_y") { p.castCenterY = clampf(static_cast<float>(std::atof(val.c_str())), 0.05f, 0.95f); sawKnownKey = true; }
        else if (key == "cast_cancel_key") { p.castCancelVk = KeyCodes::fromName(val); sawKnownKey = true; }

        else if (key == "pad_mode") { p.padMode = (toUpper(val) == "NATIVE" || val == "1") ? PAD_MODE_NATIVE : PAD_MODE_TOUCH; sawKnownKey = true; }
        else if (key == "pad_deadzone_l") { p.padDeadzoneL = clampf(static_cast<float>(std::atof(val.c_str())), 0.05f, 0.55f); sawKnownKey = true; }
        else if (key == "pad_deadzone_r") { p.padDeadzoneR = clampf(static_cast<float>(std::atof(val.c_str())), 0.05f, 0.55f); sawKnownKey = true; }
        else if (key == "pad_look_sens") { p.padLookSens = clampf(static_cast<float>(std::atof(val.c_str())), 0.10f, 4.00f); sawKnownKey = true; }

        else if (key == "aim_x") { p.aimX = clampf(static_cast<float>(std::atof(val.c_str())), 0.05f, 0.95f); sawKnownKey = true; }
        else if (key == "aim_y") p.aimY = clampf(static_cast<float>(std::atof(val.c_str())), 0.05f, 0.95f);
        else if (key == "aim_half_w") p.aimHalfW = clampf(static_cast<float>(std::atof(val.c_str())), 0.04f, 0.48f);
        else if (key == "aim_half_h") p.aimHalfH = clampf(static_cast<float>(std::atof(val.c_str())), 0.04f, 0.85f);

        else if (key == "unbounded_aim") p.unboundedAim = (val == "1" || toUpper(val) == "TRUE");
        else if (key == "invert_y") p.invertY = (val == "1" || toUpper(val) == "TRUE");
        else if (key == "ff_counter_accel") p.ffCounterAccel = clampf(static_cast<float>(std::atof(val.c_str())), 0.0f, 25.0f);
        else if (key == "sens_x") { p.sensX = clampf(static_cast<float>(std::atof(val.c_str())), 0.05f, 20.0f); sawKnownKey = true; }
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

        else if (key == "fire_mode") { p.fireMode = clampi(std::atoi(val.c_str()), 0, 2); sawKnownKey = true; }
        else if (key == "fire_drag_mode") p.fireMode = (val == "1" || toUpper(val) == "TRUE") ? FIRE_MODE_DRAG_RIGHT : FIRE_MODE_CLAW_LEFT;
        else if (key == "fire_key") { int vk = KeyCodes::fromName(val); if (vk) p.fireVk = vk; }
        else if (key == "fire_x") p.fireX = clampf(static_cast<float>(std::atof(val.c_str())), 0.02f, 0.98f);
        else if (key == "fire_y") p.fireY = clampf(static_cast<float>(std::atof(val.c_str())), 0.02f, 0.98f);
        else if (key == "fire_box_half_w") p.fireBoxHalfW = clampf(static_cast<float>(std::atof(val.c_str())), 0.05f, 0.85f);
        else if (key == "fire_box_half_h") p.fireBoxHalfH = clampf(static_cast<float>(std::atof(val.c_str())), 0.05f, 0.95f);
        else if (key == "left_fire_x") p.leftFireX = clampf(static_cast<float>(std::atof(val.c_str())), 0.02f, 0.98f);
        else if (key == "left_fire_y") p.leftFireY = clampf(static_cast<float>(std::atof(val.c_str())), 0.02f, 0.98f);

        else if (key == "joy_x") { p.joyX = clampf(static_cast<float>(std::atof(val.c_str())), 0.05f, 0.95f); sawKnownKey = true; }
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
        else if (key == "buttons_explicit") {
            sawKnownKey = true;
            if (val == "1" || toUpper(val) == "TRUE") {
                p.buttons.clear();
                sawAnyButton = true;
            }
        }
        else if (key == "button") {
            sawKnownKey = true;
            if (!sawAnyButton) {
                p.buttons.clear();
                sawAnyButton = true;
            }
            std::vector<std::string> parts = splitUnescapedComma(val);
            if (parts.size() >= 4) {
                KeyNode n;
                n.label = unescapeCfgField(parts[0]);
                KeyCodes::parseCombo(parts[1], n.comboVk, n.vk);
                n.x = clampf(static_cast<float>(std::atof(parts[2].c_str())), 0.01f, 0.99f);
                n.y = clampf(static_cast<float>(std::atof(parts[3].c_str())), 0.01f, 0.99f);
                n.x2 = n.x;
                n.y2 = n.y;
                n.radius = 0.030f;
                if (parts.size() >= 5) {
                    n.action = ProfileDefaults::nameToActionType(parts[4]);
                }
                if (n.action == NODE_SKILL_CAST) n.radius = 0.110f;
                if (parts.size() >= 6) {
                    std::string sf = toUpper(parts[5]);
                    n.switchScopeSens = (sf == "1" || sf == "TRUE" || sf.find("SCOPE") != std::string::npos);
                    n.toggleMode      = (sf.find("TOGGLE") != std::string::npos);
                    n.resetView       = (sf.find("RSTVIEW") != std::string::npos);
                    n.reactivateJoy   = (sf.find("REJOY") != std::string::npos);
                    n.quickCast       = (sf.find("QUICKCAST") != std::string::npos);
                }
                if (parts.size() >= 7) {
                    n.durationMs = clampi(std::atoi(parts[6].c_str()), 10, 2000);
                }
                if (parts.size() >= 9) {
                    n.x2 = clampf(static_cast<float>(std::atof(parts[7].c_str())), 0.01f, 0.99f);
                    n.y2 = clampf(static_cast<float>(std::atof(parts[8].c_str())), 0.01f, 0.99f);
                }
                if (parts.size() >= 10) {
                    n.radius = clampf(static_cast<float>(std::atof(parts[9].c_str())), 0.015f, 0.35f);
                }
                if (n.vk != 0) {
                    p.buttons.push_back(n);
                }
            }
        }
        else {
            for (int pi = 0; pi < PAD_BUTTON_COUNT; ++pi) {
                if (key == ProfileDefaults::padButtonConfigKey(pi)) {
                    p.padMap[pi] = unescapeCfgField(val);
                    sawKnownKey = true;
                    break;
                }
            }
        }
    }

    if (!sawKnownKey) {
        if (outError) *outError = "Not a valid Phantom Touch profile file.";
        return false;
    }
    outProfile = p;
    return true;
}

std::string ProfileStore::serializeToString(const Profile& p) {
    std::ostringstream out;
    char buf[384];
    out << "# phantom-touch-profile v1\n";
    out << "# Reference aspect for edge anchoring: " << p.refAspect << "\n";
    if (!p.appPackage.empty()) {
        out << "# package = " << p.appPackage << "\n";
    }
    out << "# All coordinates are normalized (0.0 = top/left, 1.0 = bottom/right)\n\n";

    out << "name = " << escapeCfgField(p.name) << "\n";
    out << "app_package = " << p.appPackage << "\n";
    out << "keymap_enabled = " << (p.keymapEnabled ? 1 : 0) << "\n";
    out << "edge_anchored = " << (p.edgeAnchored ? 1 : 0) << "\n";
    std::snprintf(buf, sizeof(buf), "ref_aspect = %.4f\n", p.refAspect);
    out << buf;
    out << "scrcpy_args = " << p.scrcpyArgs << "\n";
    out << "control_max_size = " << p.controlMaxSize << "\n";
    out << "poll_hz = " << p.pollHz << "\n";
    out << "toggle_aim_key = " << KeyCodes::toName(p.toggleAimVk) << "\n";
    out << "hold_unlock_key = " << KeyCodes::toName(p.holdUnlockVk) << "\n";
    out << "reset_touch_key = " << KeyCodes::toName(p.resetTouchVk) << "\n";
    out << "show_hud_in_play = " << (p.showHudInPlay ? 1 : 0) << "\n";
    std::snprintf(buf, sizeof(buf), "hud_opacity = %.2f\nfloat_btn_x = %.4f\nfloat_btn_y = %.4f\npanel_x = %.4f\npanel_y = %.4f\n",
                  p.hudOpacity, p.floatBtnX, p.floatBtnY, p.panelX, p.panelY);
    out << buf;
    out << "cursor_remember_pos = " << (p.rememberCursorPos ? 1 : 0) << "\n";
    out << "cursor_scroll_step_px = " << p.cursorScrollStepPx << "\n";
    std::snprintf(buf, sizeof(buf), "cast_center_x = %.4f\ncast_center_y = %.4f\n",
                  p.castCenterX, p.castCenterY);
    out << buf;
    out << "cast_cancel_key = " << KeyCodes::toName(p.castCancelVk) << "\n\n";

    out << "# --- Aim, View Stack & Counter-Acceleration Physics ---\n";
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

    out << "# --- Fire Button ---\n";
    out << "fire_mode = " << p.fireMode << "\n";
    out << "fire_key = " << KeyCodes::toName(p.fireVk) << "\n";
    std::snprintf(buf, sizeof(buf), "fire_x = %.4f\nfire_y = %.4f\nfire_box_half_w = %.4f\nfire_box_half_h = %.4f\n",
                  p.fireX, p.fireY, p.fireBoxHalfW, p.fireBoxHalfH);
    out << buf;
    std::snprintf(buf, sizeof(buf), "left_fire_x = %.4f\nleft_fire_y = %.4f\n\n",
                  p.leftFireX, p.leftFireY);
    out << buf;

    out << "# --- WASD Virtual Joystick ---\n";
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

    out << "# --- Gamepad v2 ---\n";
    out << "pad_mode = " << (p.padMode == PAD_MODE_NATIVE ? "native" : "touch") << "\n";
    std::snprintf(buf, sizeof(buf), "pad_deadzone_l = %.3f\npad_deadzone_r = %.3f\npad_look_sens = %.3f\n",
                  p.padDeadzoneL, p.padDeadzoneR, p.padLookSens);
    out << buf;
    for (int pi = 0; pi < PAD_BUTTON_COUNT; ++pi) {
        if (!p.padMap[pi].empty()) {
            out << ProfileDefaults::padButtonConfigKey(pi) << " = " << escapeCfgField(p.padMap[pi]) << "\n";
        }
    }
    out << "\n";

    out << "# --- Action Nodes ---\n";
    out << "buttons_explicit = 1\n";
    for (size_t i = 0; i < p.buttons.size(); ++i) {
        const KeyNode& b = p.buttons[i];
        std::string escLabel = escapeCfgField(b.label);
        std::string keySpec = KeyCodes::comboToName(b.comboVk, b.vk);
        std::string flagsStr;
        if (b.toggleMode || b.resetView || b.reactivateJoy || b.quickCast) {
            if (b.switchScopeSens) flagsStr += (flagsStr.empty() ? "" : "|") + std::string("SCOPE");
            if (b.toggleMode)      flagsStr += (flagsStr.empty() ? "" : "|") + std::string("TOGGLE");
            if (b.resetView)       flagsStr += (flagsStr.empty() ? "" : "|") + std::string("RSTVIEW");
            if (b.reactivateJoy)   flagsStr += (flagsStr.empty() ? "" : "|") + std::string("REJOY");
            if (b.quickCast)       flagsStr += (flagsStr.empty() ? "" : "|") + std::string("QUICKCAST");
        } else {
            flagsStr = b.switchScopeSens ? "1" : "0";
        }
        if (b.action == NODE_SKILL_CAST) {
            std::snprintf(buf, sizeof(buf), "button = %s, %s, %.4f, %.4f, %s, %s, %d, %.4f, %.4f, %.4f\n",
                          escLabel.c_str(), keySpec.c_str(), b.x, b.y,
                          ProfileDefaults::actionTypeToName(b.action).c_str(),
                          flagsStr.c_str(), b.durationMs, b.x2, b.y2, b.radius);
        } else if (b.action == NODE_SWIPE) {
            std::snprintf(buf, sizeof(buf), "button = %s, %s, %.4f, %.4f, %s, %s, %d, %.4f, %.4f\n",
                          escLabel.c_str(), keySpec.c_str(), b.x, b.y,
                          ProfileDefaults::actionTypeToName(b.action).c_str(),
                          flagsStr.c_str(), b.durationMs, b.x2, b.y2);
        } else {
            std::snprintf(buf, sizeof(buf), "button = %s, %s, %.4f, %.4f, %s, %s, %d\n",
                          escLabel.c_str(), keySpec.c_str(), b.x, b.y,
                          ProfileDefaults::actionTypeToName(b.action).c_str(),
                          flagsStr.c_str(), b.durationMs);
        }
        out << buf;
    }
    return out.str();
}

bool ProfileStore::load(const std::string& path, Profile& outProfile) {
    std::string raw;
    if (!readFileUtf8(path, raw, nullptr)) return false;
    return parseFromString(raw, outProfile, nullptr);
}

bool ProfileStore::save(const std::string& path, const Profile& p) {
    std::string text = serializeToString(p);
    std::wstring wpath = utf8ToWide(path);
    std::wstring wtmp = utf8ToWide(path + ".tmp");
    if (wpath.empty()) return false;

    auto writeAll = [&](const std::wstring& target) -> bool {
        HANDLE hf = CreateFileW(target.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hf == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        BOOL ok = WriteFile(hf, text.data(), static_cast<DWORD>(text.size()), &written, NULL);
        CloseHandle(hf);
        return (ok && written == static_cast<DWORD>(text.size()));
    };

    if (!wtmp.empty() && writeAll(wtmp)) {
        if (MoveFileExW(wtmp.c_str(), wpath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            return true;
        }
        bool copied = (CopyFileW(wtmp.c_str(), wpath.c_str(), FALSE) != 0);
        DeleteFileW(wtmp.c_str());
        if (copied) return true;
    }
    return writeAll(wpath);
}

bool ProfileStore::importProfileText(const std::string& cfgText, const std::string& profilesDir,
                                     std::string& outSavedPath, std::string& outError) {
    outSavedPath.clear();
    outError.clear();
    Profile parsed;
    if (!parseFromString(cfgText, parsed, &outError)) {
        return false;
    }
    std::string base = sanitizeFilename(parsed.name.empty() ? "imported" : parsed.name);
    std::string fname = base;
    std::string target = profilesDir + "\\" + fname + ".cfg";
    int suffix = 2;
    while (GetFileAttributesW(utf8ToWide(target).c_str()) != INVALID_FILE_ATTRIBUTES && suffix < 100) {
        char sbuf[16];
        std::snprintf(sbuf, sizeof(sbuf), "_%d", suffix++);
        fname = base + sbuf;
        target = profilesDir + "\\" + fname + ".cfg";
    }
    if (suffix > 2 && !parsed.name.empty()) {
        char sbuf[16];
        std::snprintf(sbuf, sizeof(sbuf), " (%d)", suffix - 1);
        parsed.name += sbuf;
    }
    if (!save(target, parsed)) {
        outError = "Failed saving imported profile to " + target;
        return false;
    }
    outSavedPath = target;
    return true;
}

bool ProfileStore::importProfileFile(const std::string& srcUtf8Path, const std::string& profilesDir,
                                     std::string& outSavedPath, std::string& outError) {
    std::string raw;
    if (!readFileUtf8(srcUtf8Path, raw, &outError)) {
        return false;
    }
    return importProfileText(raw, profilesDir, outSavedPath, outError);
}

bool ProfileStore::moveToTrash(const std::string& profilePath, const std::string& profilesDir,
                               std::string& outTrashPath) {
    outTrashPath.clear();
    std::string trashDir = profilesDir + "\\.trash";
    CreateDirectoryW(utf8ToWide(trashDir).c_str(), NULL);
    std::string stem = sanitizeFilename(baseNameNoExt(profilePath));
    char tbuf[32];
    std::snprintf(tbuf, sizeof(tbuf), "_%lu", static_cast<unsigned long>(GetTickCount()));
    outTrashPath = trashDir + "\\" + stem + tbuf + ".cfg";
    std::wstring wSrc = utf8ToWide(profilePath);
    std::wstring wDst = utf8ToWide(outTrashPath);
    if (MoveFileExW(wSrc.c_str(), wDst.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        return true;
    }
    if (CopyFileW(wSrc.c_str(), wDst.c_str(), FALSE)) {
        DeleteFileW(wSrc.c_str());
        return true;
    }
    outTrashPath.clear();
    return false;
}

bool ProfileStore::restoreFromTrash(const std::string& trashPath, const std::string& originalPath) {
    if (trashPath.empty() || originalPath.empty()) return false;
    std::wstring wSrc = utf8ToWide(trashPath);
    std::wstring wDst = utf8ToWide(originalPath);
    if (MoveFileExW(wSrc.c_str(), wDst.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        return true;
    }
    if (CopyFileW(wSrc.c_str(), wDst.c_str(), FALSE)) {
        DeleteFileW(wSrc.c_str());
        return true;
    }
    return false;
}
