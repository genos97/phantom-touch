#pragma once

#include "profile.hpp"
#include "protocol.hpp"
#include "fps_view.hpp"

#include <cstdint>
#include <vector>

struct EngineTelemetry {
    bool aimMode = false;
    bool aimFingerDown = false;
    bool fireHeld = false;
    bool scopeActive = false;
    bool holdViewActive = false;
    bool joyDown = false;
    float aimFingerNormX = 0.62f;
    float aimFingerNormY = 0.70f;
    float joyFingerNormX = 0.16f;
    float joyFingerNormY = 0.75f;
    uint32_t screenW = 1920;
    uint32_t screenH = 1080;
    uint32_t touchRateHz = 0;
};

class AimEngine {
public:
    AimEngine(ScrcpyBridge* bridge);
    ~AimEngine();

    void start();
    void stop();

    void setProfile(const Profile& profile);
    Profile getProfileCopy() const;

    void setAimMode(bool enabled);
    bool isAimMode() const { return aimMode_ != 0; }

    // Called from Win32 Raw Input / Hook thread (non-blocking, thread-safe)
    void onRawMouseDelta(int dx, int dy);

    // Returns true if key/mouse button was consumed by an active mapping (NON-BLOCKING: <5us, safe in WH_*_LL hooks!)
    // outRequestToggleAim is set to true if a cursor-toggle node (e.g. Tab/Bag) was pressed
    bool onKeyEvent(int vk, bool down, bool& outRequestToggleAim);

    // Fast non-blocking check used by keyboard hook to suppress OS auto-repeat on mapped keys
    bool isKeyMapped(int vk, bool aimModeActive) const;

    // Direct single-finger pass-through when Aim Mode is OFF (NON-BLOCKING queue)
    void onDirectCursorTouch(TouchAction action, float normX, float normY);

    // Instantly releases all active virtual fingers and resets internal state
    void releaseAllTouches();

    // Cleanly resets only the active camera view finger
    void resetAimView();

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

    struct KeyRouteEntry {
        int comboVk;
        int vk;
        KeyActionType action;
    };

    static DWORD WINAPI tickThreadProc(LPVOID param);
    void runLoop();
    void processTick(double nowMs);
    void rebuildKeyRouteCacheLocked();
    bool classifyKeyInputLocked(int vk, bool down, bool isAim, bool& outRequestToggleAim);
    void processKeyEventLocked(int vk, bool down, double nowMs);
    void processDirectCursorLocked(TouchAction action, float normX, float normY);

    void updateJoystickLocked(double nowMs);
    void stepJoystickGlideLocked(double nowMs);
    void reactivateJoystickLocked(double nowMs);

    static void fpsViewEmitBridge(void* ctx, TouchAction action, uint64_t pointerId,
                                  int32_t px, int32_t py, bool clampToScreen);
    void syncFpsViewConfigLocked(uint32_t w, uint32_t h);

    void liftFireFingerLocked();
    void pressFireLocked(double nowMs);
    void releaseMouseTouchesLocked();
    void releaseAllLocked();

    void sendTapWithJitterLocked(TouchAction action, uint64_t ptrId, float nx, float ny, int32_t& ioPx, int32_t& ioPy);
    void triggerButtonStartLocked(size_t idx, double nowMs, bool& outRequestToggleAim);
    void triggerButtonStopLocked(size_t idx, double nowMs);

    ScrcpyBridge* bridge_;

    mutable CRITICAL_SECTION lock_;      // Owned by AimEngine tick thread (touch injection & state)
    mutable CRITICAL_SECTION inputLock_; // Microsecond-only lock for non-blocking hook queues!
    Profile profile_;

    HANDLE thread_ = NULL;
    HANDLE wakeEvent_ = NULL;
    volatile LONG stopFlag_ = 0;
    volatile LONG aimMode_ = 0;

    // Non-blocking queues fed from Win32 Raw Input / WH_KEYBOARD_LL / WH_MOUSE_LL hooks (NEVER calls send()!)
    std::vector<QueuedKeyEvent> pendingKeys_;
    std::vector<QueuedCursorTouch> pendingCursor_;
    bool hookHeldKeys_[512];
    int routeVkUp_ = 'W', routeVkDown_ = 'S', routeVkLeft_ = 'A', routeVkRight_ = 'D';
    int routeVkSprint_ = 0xA0, routeVkWalk_ = 0xA2, routeFireVk_ = 0x01;
    std::vector<KeyRouteEntry> routeButtons_;

    // Keyboard state table for 2-key combo resolution (worker thread)
    bool heldKeys_[512];

    // Pending raw mouse accumulation (updated at 1000 Hz, drained at pollHz)
    double accumRawDx_ = 0.0;
    double accumRawDy_ = 0.0;
    double smoothDx_ = 0.0;
    double smoothDy_ = 0.0;

    // Dedicated standalone 360° Virtual-Finger FPS View Stack (main camera + Free Look overlay)
    FpsViewStack viewStack_;
    uint32_t lastConfiguredW_ = 0;
    uint32_t lastConfiguredH_ = 0;

    // Scope sensitivity state (NODE_SWITCH_SENS or switchScopeSens on SCOPE node)
    bool scopeSensActive_ = false;

    // Fire state (PTR_FIRE = 3)
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

    // WASD joystick + Free Fire 3-stage stepped glide state (PTR_JOYSTICK = 1)
    bool keyUp_ = false;
    bool keyDown_ = false;
    bool keyLeft_ = false;
    bool keyRight_ = false;
    bool keySprint_ = false;
    bool keyWalk_ = false;
    bool joyDown_ = false;
    double joyCurX_ = 0.0;
    double joyCurY_ = 0.0;
    int32_t joyLastX_ = -1;
    int32_t joyLastY_ = -1;
    bool joyGlideActive_ = false;
    int joyGlideStage_ = 0;
    double joyGlideStartX_ = 0.0;
    double joyGlideStartY_ = 0.0;
    double joyGlideTargetX_ = 0.0;
    double joyGlideTargetY_ = 0.0;
    double joyGlideNextMs_ = 0.0;

    // Action buttons runtime state (PTR_BTN_BASE + idx)
    struct ButtonRuntime {
        bool keyHeld = false;
        bool touchDown = false;
        int32_t lastPx = 0;
        int32_t lastPy = 0;
        double autoReleaseAtMs = 0.0;
        // Swipe interpolation state
        bool swiping = false;
        double swipeStartMs = 0.0;
        double swipeDurMs = 0.0;
        double swipeX1 = 0.0, swipeY1 = 0.0;
        double swipeX2 = 0.0, swipeY2 = 0.0;
    };
    std::vector<ButtonRuntime> btnState_;

    // Direct cursor pass-through state (PTR_CURSOR = 9)
    bool directCursorDown_ = false;

    // Fast deterministic PRNG for subtle tap jitter
    uint32_t rngState_ = 0x13579BDFu;

    // Telemetry counters
    uint32_t packetsThisSec_ = 0;
    uint32_t lastPacketsPerSec_ = 0;
    double lastStatMs_ = 0.0;
};
