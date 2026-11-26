#pragma once

#include "bridge/TouchTypes.hpp"
#include "config/Profile.hpp"

#include <cstdint>

struct GamepadSnapshot {
    bool connected = false;
    float lx = 0.0f;         // Left stick X [-1..+1] (after deadzone)
    float ly = 0.0f;         // Left stick Y [-1..+1] (+1 = up)
    float rx = 0.0f;         // Right stick X [-1..+1] (after deadzone)
    float ry = 0.0f;         // Right stick Y [-1..+1] (+1 = up)
    float lt = 0.0f;         // Left trigger [0..1]
    float rt = 0.0f;         // Right trigger [0..1]
    uint16_t buttons = 0;    // XINPUT_GAMEPAD_* bitmask
};

struct GamepadKeyDelta {
    int vk;
    bool down;
};

struct GamepadFrameOutput {
    bool connected = false;
    bool analogJoyActive = false;
    bool stickDeflected = false; // Raw left-stick deflection before owner suppression (E3)
    double lookDx = 0.0;     // Virtual mouse delta X for FpsViewStack
    double lookDy = 0.0;     // Virtual mouse delta Y for FpsViewStack
    int keyDeltaCount = 0;
    GamepadKeyDelta keyDeltas[16];
};

// XInput Gamepad-to-Touch Controller (dynamically loads xinput1_4.dll / xinput9_1_0.dll).
// Maps:
//   - Left Stick  -> 360° smooth analog touch joystick around profile.joyX/Y (via TouchFingerPool 0..9!)
//   - Right Stick -> 360° quadratic camera look fed directly into FpsViewStack
//   - RT / LT     -> Fire (LMB) / Scope (RMB)
//   - A/B/X/Y/LB/RB/D-Pad/Start/Back -> standard game button virtual keys
class GamepadController {
public:
    GamepadController();
    ~GamepadController();

    void setTouchSink(TouchEmitFn fn, void* ctx);
    void setEnabled(bool enabled) { enabled_ = enabled; }
    bool isEnabled() const { return enabled_; }
    bool isConnected() const { return connected_; }
    bool isJoyDown() const { return joyDown_; }

    // Polls Windows XInput (slots 0..3) and processes analog sticks + buttons for one engine tick.
    void pollAndProcess(const Profile& profile, uint32_t screenW, uint32_t screenH,
                        double dtMs, uint32_t& packetsCounter, GamepadFrameOutput& out,
                        bool suppressLeftStick = false);

    // Pure deterministic processor (used by both pollAndProcess and headless unit tests!).
    void processSnapshot(const GamepadSnapshot& snap, const Profile& profile,
                         uint32_t screenW, uint32_t screenH, double dtMs,
                         uint32_t& packetsCounter, GamepadFrameOutput& out,
                         bool suppressLeftStick = false);

    // Releases active analog joystick touch only (used during E3 keyboard/gamepad handoff).
    void releaseJoystickOnly(uint32_t& packetsCounter);

    // Suspends gamepad on focus loss or edit mode: lifts joystick, emits key-ups, and latches
    // held buttons so they must return to neutral before firing again (E4).
    void suspendForFocusLoss(uint32_t& packetsCounter, GamepadFrameOutput& out);

    // Releases active analog joystick touch & resets button edge-tracking state cleanly.
    void releaseAll(uint32_t& packetsCounter);

    // Normalizes raw int16 stick axis with circular deadzone
    static void normalizeStick(int16_t rawX, int16_t rawY, float deadzone, float& outX, float& outY);

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
};
