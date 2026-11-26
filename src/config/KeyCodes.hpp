#pragma once

#include <cstdint>
#include <string>

// Custom virtual key codes for mouse wheel actions (beyond standard 0x00..0xFF Win32 VK range)
static const int VK_CUSTOM_MWHEEL_UP   = 0x101;
static const int VK_CUSTOM_MWHEEL_DOWN = 0x102;

class KeyCodes {
public:
    static std::string toName(int vk);
    static int fromName(const std::string& raw);

    static std::string comboToName(int comboVk, int vk);
    static void parseCombo(const std::string& spec, int& outComboVk, int& outVk);

    // Matches generic Shift/Ctrl/Alt (0x10/0x11/0x12) against left/right side VKs (0xA0..0xA5)
    static bool matches(int eventVk, int targetVk);

    // Checks whether targetVk (or its matching generic/side modifier) is held in heldKeys[512]
    static bool isKeyHeld(const bool heldKeys[512], int targetVk);

    // Normalizes WM_KEYDOWN generic Shift/Ctrl/Alt to the exact physical side VK_L*/VK_R*
    static int normalizeModifier(int vk);

    // Returns true for LMB, RMB, MMB, X1, X2, MWHEELUP, MWHEELDOWN
    static bool isMouseKey(int vk);

    // Converts a Win32 VK into an Android AKEYCODE_* (0 if unmapped) for desktop typing & shortcuts
    static uint32_t vkToAndroidKeycode(int vk);

    // E1 (AltGr vs Ctrl): returns true only for pure Ctrl shortcuts (Ctrl held, Alt NOT held)
    static bool isPureCtrlShortcut(bool ctrlDown, bool altDown);
};
