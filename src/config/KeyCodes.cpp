#include "config/KeyCodes.hpp"

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

namespace {

std::string trimStr(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) ++start;
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
    return s.substr(start, end - start);
}

std::string toUpperStr(std::string s) {
    for (size_t i = 0; i < s.size(); ++i) {
        s[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[i])));
    }
    return s;
}

} // namespace

std::string KeyCodes::toName(int vk) {
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

int KeyCodes::fromName(const std::string& raw) {
    std::string s = toUpperStr(trimStr(raw));
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

std::string KeyCodes::comboToName(int comboVk, int vk) {
    if (comboVk != 0) {
        return toName(comboVk) + "+" + toName(vk);
    }
    return toName(vk);
}

void KeyCodes::parseCombo(const std::string& spec, int& outComboVk, int& outVk) {
    outComboVk = 0;
    outVk = 0;
    size_t plus = spec.find('+');
    if (plus != std::string::npos) {
        outComboVk = fromName(spec.substr(0, plus));
        outVk = fromName(spec.substr(plus + 1));
    } else {
        outVk = fromName(spec);
    }
}

bool KeyCodes::matches(int eventVk, int targetVk) {
    if (targetVk == 0) return false;
    if (eventVk == targetVk) return true;
    if (targetVk == VK_SHIFT   && (eventVk == VK_LSHIFT   || eventVk == VK_RSHIFT))   return true;
    if (targetVk == VK_CONTROL && (eventVk == VK_LCONTROL || eventVk == VK_RCONTROL)) return true;
    if (targetVk == VK_MENU    && (eventVk == VK_LMENU    || eventVk == VK_RMENU))    return true;
    if (eventVk  == VK_SHIFT   && (targetVk == VK_LSHIFT  || targetVk == VK_RSHIFT))  return true;
    if (eventVk  == VK_CONTROL && (targetVk == VK_LCONTROL|| targetVk == VK_RCONTROL))return true;
    if (eventVk  == VK_MENU    && (targetVk == VK_LMENU   || targetVk == VK_RMENU))   return true;
    return false;
}

bool KeyCodes::isKeyHeld(const bool heldKeys[512], int targetVk) {
    if (targetVk <= 0 || targetVk >= 512) return false;
    if (heldKeys[targetVk]) return true;
    if (targetVk == VK_SHIFT)   return heldKeys[VK_LSHIFT]   || heldKeys[VK_RSHIFT];
    if (targetVk == VK_CONTROL) return heldKeys[VK_LCONTROL] || heldKeys[VK_RCONTROL];
    if (targetVk == VK_MENU)    return heldKeys[VK_LMENU]    || heldKeys[VK_RMENU];
    if (targetVk == VK_LSHIFT   || targetVk == VK_RSHIFT)   return heldKeys[VK_SHIFT];
    if (targetVk == VK_LCONTROL || targetVk == VK_RCONTROL) return heldKeys[VK_CONTROL];
    if (targetVk == VK_LMENU    || targetVk == VK_RMENU)    return heldKeys[VK_MENU];
    return false;
}

int KeyCodes::normalizeModifier(int vk) {
    if (vk == VK_SHIFT)   return ((GetKeyState(VK_RSHIFT)   & 0x8000) || (GetAsyncKeyState(VK_RSHIFT)   & 0x8000)) ? VK_RSHIFT   : VK_LSHIFT;
    if (vk == VK_CONTROL) return ((GetKeyState(VK_RCONTROL) & 0x8000) || (GetAsyncKeyState(VK_RCONTROL) & 0x8000)) ? VK_RCONTROL : VK_LCONTROL;
    if (vk == VK_MENU)    return ((GetKeyState(VK_RMENU)    & 0x8000) || (GetAsyncKeyState(VK_RMENU)    & 0x8000)) ? VK_RMENU    : VK_LMENU;
    return vk;
}

bool KeyCodes::isMouseKey(int vk) {
    return vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON ||
           vk == VK_XBUTTON1 || vk == VK_XBUTTON2 ||
           vk == VK_CUSTOM_MWHEEL_UP || vk == VK_CUSTOM_MWHEEL_DOWN;
}
