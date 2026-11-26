#pragma once

#include "bridge/TouchTypes.hpp"
#include "config/Profile.hpp"

#include <cstdint>
#include <cstring>

enum GamepadShellNavBit : uint8_t {
    PAD_NAV_UP     = 0x01,
    PAD_NAV_DOWN   = 0x02,
    PAD_NAV_LEFT   = 0x04,
    PAD_NAV_RIGHT  = 0x08,
    PAD_NAV_SELECT = 0x10,
    PAD_NAV_BACK   = 0x20,
    PAD_NAV_GUIDE  = 0x40
};

struct GamepadSnapshot {
    bool connected = false;
    float lx = 0.0f;         // Left stick X [-1..+1] (after deadzone)
    float ly = 0.0f;         // Left stick Y [-1..+1] (+1 = up)
    float rx = 0.0f;         // Right stick X [-1..+1] (after deadzone)
    float ry = 0.0f;         // Right stick Y [-1..+1] (+1 = up)
    float lt = 0.0f;         // Left trigger [0..1]
    float rt = 0.0f;         // Right trigger [0..1]
    uint16_t buttons = 0;    // XINPUT_GAMEPAD_* bitmask
    int16_t rawLx = 0;
    int16_t rawLy = 0;
    int16_t rawRx = 0;
    int16_t rawRy = 0;
    uint8_t rawLt = 0;
    uint8_t rawRt = 0;
    bool guidePressed = false;
    bool hasRawAxes = false;
};

struct GamepadKeyDelta {
    int vk;
    bool down;
};

struct GamepadFrameOutput {
    bool connected;
    bool analogJoyActive;
    bool stickDeflected; // Raw left-stick deflection before owner suppression (E3)
    double lookDx;       // Virtual mouse delta X for FpsViewStack
    double lookDy;       // Virtual mouse delta Y for FpsViewStack
    int keyDeltaCount;
    GamepadKeyDelta keyDeltas[16];
    bool uhidActive;
    bool uhidNeedCreate;
    bool uhidNeedDestroy;
    bool uhidReportChanged;
    uint8_t uhidReport[15];
    uint8_t shellNavMask;

    GamepadFrameOutput()
        : connected(false), analogJoyActive(false), stickDeflected(false),
          lookDx(0.0), lookDy(0.0), keyDeltaCount(0),
          uhidActive(false), uhidNeedCreate(false), uhidNeedDestroy(false),
          uhidReportChanged(false), shellNavMask(0) {
        std::memset(keyDeltas, 0, sizeof(keyDeltas));
        std::memset(uhidReport, 0, sizeof(uhidReport));
    }
};

// XInput Gamepad v2 (dynamically loads xinput1_4.dll / xinput1_3.dll / xinput9_1_0.dll).
// Supports both Per-Profile Touch Mapping (E2-E4, E11) and scrcpy UHID Native Gamepad passthrough (F6).
// HID gamepad report descriptor & 15-byte packing adapted from Genymobile/scrcpy (Apache-2.0).
class GamepadController {
public:
    GamepadController();
    ~GamepadController();

    void setTouchSink(TouchEmitFn fn, void* ctx);
    void setEnabled(bool enabled) { enabled_ = enabled; }
    bool isEnabled() const { return enabled_; }
    bool isConnected() const { return connected_; }
    bool isJoyDown() const { return joyDown_; }
    bool isUhidCreated() const { return uhidCreated_; }
    void markUhidDestroyed() { uhidCreated_ = false; hasLastHidReport_ = false; }

    // Polls Windows XInput (slots 0..3) and processes analog sticks + buttons for one engine tick.
    void pollAndProcess(const Profile& profile, uint32_t screenW, uint32_t screenH,
                        double dtMs, uint32_t& packetsCounter, GamepadFrameOutput& out,
                        bool suppressLeftStick = false, bool shellMenuOpen = false);

    // Pure deterministic processor (used by both pollAndProcess and headless unit tests!).
    void processSnapshot(const GamepadSnapshot& snap, const Profile& profile,
                         uint32_t screenW, uint32_t screenH, double dtMs,
                         uint32_t& packetsCounter, GamepadFrameOutput& out,
                         bool suppressLeftStick = false, bool shellMenuOpen = false);

    // Releases active analog joystick touch only (used during E3 keyboard/gamepad handoff).
    void releaseJoystickOnly(uint32_t& packetsCounter);

    // Suspends gamepad on focus loss / edit mode / disable: lifts joystick, emits key-ups,
    // resets UHID to neutral, and latches held buttons (E4, E11).
    void suspendForFocusLoss(uint32_t& packetsCounter, GamepadFrameOutput& out);

    // Releases active analog joystick touch & resets button edge-tracking state cleanly.
    void releaseAll(uint32_t& packetsCounter);

    // Normalizes raw int16 stick axis with circular deadzone
    static void normalizeStick(int16_t rawX, int16_t rawY, float deadzone, float& outX, float& outY);

    // Pure UHID & per-profile pad mapping helpers
    static const uint8_t* hidReportDescriptor(uint16_t& outSize);
    static void packHidReport(int16_t rawLx, int16_t rawLy, int16_t rawRx, int16_t rawRy,
                              uint8_t rawLt, uint8_t rawRt, uint16_t xinputButtons,
                              bool guidePressed, uint8_t outReport15[15]);
    static int resolvePadButtonVk(int padBtnId, const Profile& profile);

private:
    void ensureXInputLoaded();
    void emit(TouchAction action, int32_t px, int32_t py, uint32_t& packetsCounter);

    TouchEmitFn sink_ = nullptr;
    void* sinkCtx_ = nullptr;

    void* xinputDll_ = nullptr;
    void* xinputGetStateFn_ = nullptr;
    bool loadAttempted_ = false;
    bool enabled_ = true;
    bool connected_ = false;
    int activeSlot_ = 0;
    double nextScanAtMs_ = 0.0;
    double elapsedMs_ = 0.0;

    bool joyDown_ = false;
    int32_t joyLastX_ = 0;
    int32_t joyLastY_ = 0;

    uint16_t prevButtons_ = 0;
    bool prevLt_ = false;
    bool prevRt_ = false;
    uint16_t suppressedButtons_ = 0;
    bool suppressedLt_ = false;
    bool suppressedRt_ = false;
    int lastUsedVk_[PAD_BUTTON_COUNT];

    uint16_t prevNavButtons_ = 0;
    bool prevNavGuide_ = false;

    bool uhidCreated_ = false;
    bool hasLastHidReport_ = false;
    uint8_t lastHidReport_[15];
};
