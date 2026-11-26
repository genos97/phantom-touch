#pragma once

#include "bridge/ScrcpyBridge.hpp"
#include "bridge/TouchFingerPool.hpp"
#include "config/Profile.hpp"
#include "engine/ActionDispatcher.hpp"
#include "engine/FpsViewController.hpp"
#include "engine/GamepadController.hpp"
#include "engine/JoystickController.hpp"

#include <cstdint>
#include <vector>

struct EngineTelemetry {
    bool aimMode = false;
    bool aimFingerDown = false;
    bool fireHeld = false;
    bool scopeActive = false;
    bool holdViewActive = false;
    bool joyDown = false;
    bool gamepadConnected = false;
    float aimFingerNormX = 0.62f;
    float aimFingerNormY = 0.70f;
    float joyFingerNormX = 0.16f;
    float joyFingerNormY = 0.75f;
    uint32_t screenW = 1920;
    uint32_t screenH = 1080;
    uint32_t touchRateHz = 0;
};

// High-precision multi-touch orchestrator:
//   - Non-blocking hook queue (<5us lock, zero TCP send on Windows hook threads)
//   - QPC fixed-cadence polling loop (125 / 250 / 500 / MAX 1000 Hz)
//   - Coordinates FpsViewStack (360° camera), JoystickController (WASD),
//     ActionDispatcher (combos & 10 button modes), and Fire Modes (DRAG/CLAW/DUAL).
class InputEngine {
public:
    explicit InputEngine(ScrcpyBridge* bridge);
    ~InputEngine();

    void start();
    void stop();

    void setProfile(const Profile& profile);
    Profile getProfileCopy() const;

    void setAimMode(bool enabled);
    bool isAimMode() const { return aimMode_ != 0; }
    void setGamepadEnabled(bool enabled);

    // Non-blocking raw mouse & hook entry points (safe from WM_INPUT / WH_*_LL)
    void onRawMouseDelta(int dx, int dy);
    bool onKeyEvent(int vk, bool down, bool& outRequestToggleAim);
    bool isKeyMapped(int vk, bool aimModeActive) const;
    void onDirectCursorTouch(TouchAction action, float normX, float normY);
    void onDirectCursorScroll(float normX, float normY, int wheelNotches);
    void onDirectCursorPinch(float normX, float normY, int wheelNotches);

    void releaseAllTouches();
    void resetAimView();

    // Optional downstream sink override (for unit tests verifying 0..9 finger pool output)
    void setTouchSinkForTest(TouchEmitFn fn, void* ctx);

    EngineTelemetry getTelemetry() const;

private:
    struct QueuedKeyEvent {
        int vk;
        bool down;
        double timeMs;
    };

    struct QueuedCursorTouch {
        TouchAction action;
        float normX;
        float normY;
    };

    struct QueuedCursorScroll {
        float normX;
        float normY;
        int notches;
        bool isPinch;
    };

    struct KeyRouteEntry {
        int comboVk;
        int vk;
        KeyActionType action;
    };

    static DWORD WINAPI tickThreadProc(LPVOID param);
    static void emitTouchToBridge(void* ctx, TouchAction action, uint64_t pointerId,
                                  int32_t px, int32_t py, bool clampToScreen);
    static void emitTouchThroughPool(void* ctx, TouchAction action, uint64_t logicalId,
                                     int32_t px, int32_t py, bool clampToScreen);

    void runLoop();
    void processTick(double nowMs);
    void rebuildKeyRouteCacheLocked();
    void syncFpsViewConfigLocked(uint32_t w, uint32_t h);
    bool classifyKeyInputLocked(int vk, bool down, bool isAim, bool& outRequestToggleAim);
    void processKeyEventLocked(int vk, bool down, double nowMs);
    void processDirectCursorLocked(TouchAction action, float normX, float normY);
    void processDirectCursorScrollLocked(float normX, float normY, int wheelNotches, double nowMs);
    void releaseCursorScrollLocked();
    void processDirectCursorPinchLocked(float normX, float normY, int wheelNotches, double nowMs);
    void releaseCursorPinchLocked();

    void pressFireLocked(double nowMs);
    void liftFireFingerLocked();
    void releaseMouseTouchesLocked();
    void releaseAllLocked();

    ScrcpyBridge* bridge_;

    mutable CRITICAL_SECTION lock_;      // Worker thread state & touch dispatch
    mutable CRITICAL_SECTION inputLock_; // Microsecond lock for non-blocking hook queues
    Profile rawProfile_;                 // Authoring/stored reference profile
    Profile profile_;                    // Active screen-resolved profile (edge-anchored when enabled)

    HANDLE thread_ = NULL;
    HANDLE wakeEvent_ = NULL;
    volatile LONG stopFlag_ = 0;
    volatile LONG aimMode_ = 0;

    // Non-blocking hook input queues & route cache (protected by inputLock_)
    std::vector<QueuedKeyEvent> pendingKeys_;
    std::vector<QueuedCursorTouch> pendingCursor_;
    std::vector<QueuedCursorScroll> pendingScroll_;
    bool hookHeldKeys_[512];
    bool routeKeymapEnabled_ = true;
    int routeVkUp_ = 'W', routeVkDown_ = 'S', routeVkLeft_ = 'A', routeVkRight_ = 'D';
    int routeVkSprint_ = 0xA0, routeVkWalk_ = 0xA2, routeFireVk_ = 0x01;
    std::vector<KeyRouteEntry> routeButtons_;
    double accumRawDx_ = 0.0;
    double accumRawDy_ = 0.0;

    // Worker-thread state (protected by lock_)
    bool heldKeys_[512];
    double smoothDx_ = 0.0;
    double smoothDy_ = 0.0;
    bool scopeSensActive_ = false;
    bool directCursorDown_ = false;
    int32_t directCursorLastX_ = 0;
    int32_t directCursorLastY_ = 0;

    // Unlocked cursor scroll-wheel virtual touch drag (auto-releases after 80ms idle)
    bool cursorScrollDown_ = false;
    double cursorScrollCurX_ = 0.0;
    double cursorScrollCurY_ = 0.0;
    double cursorScrollReleaseAtMs_ = 0.0;

    // Unlocked cursor Ctrl+Wheel 2-finger pinch-zoom (auto-releases after 140ms idle)
    bool cursorPinchDown_ = false;
    double pinchCenterX_ = 0.0;
    double pinchCenterY_ = 0.0;
    double pinchRadius_ = 0.0;
    double pinchReleaseAtMs_ = 0.0;

    // Modular sub-controllers & bounded 0..9 Android virtual finger pool
    TouchFingerPool fingerPool_;
    FpsViewStack viewStack_;
    GamepadController gamepad_;
    double lastTickMs_ = 0.0;
    JoystickController joystick_;
    ActionDispatcher actions_;
    uint32_t lastConfiguredW_ = 0;
    uint32_t lastConfiguredH_ = 0;

    // Fire finger state (PointerId::FIRE)
    bool fireHeld_ = false;
    bool fireDown_ = false;
    bool pendingFireDown_ = false;
    double pendingFireAtMs_ = 0.0;
    double quickTapFireReleaseAtMs_ = 0.0;
    double aimCooldownUntilMs_ = 0.0;
    double fireCurX_ = 0.0;
    double fireCurY_ = 0.0;
    int32_t fireStartX_ = 0;
    int32_t fireStartY_ = 0;
    int32_t fireLastSentX_ = -1;
    int32_t fireLastSentY_ = -1;

    // Telemetry counters
    uint32_t packetsThisSec_ = 0;
    uint32_t lastPacketsPerSec_ = 0;
    double lastStatMs_ = 0.0;
};
