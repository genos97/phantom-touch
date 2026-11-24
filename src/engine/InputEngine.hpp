#pragma once

#include "bridge/ScrcpyBridge.hpp"
#include "config/Profile.hpp"
#include "engine/ActionDispatcher.hpp"
#include "engine/FpsViewController.hpp"
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

    // Non-blocking raw mouse & hook entry points (safe from WM_INPUT / WH_*_LL)
    void onRawMouseDelta(int dx, int dy);
    bool onKeyEvent(int vk, bool down, bool& outRequestToggleAim);
    bool isKeyMapped(int vk, bool aimModeActive) const;
    void onDirectCursorTouch(TouchAction action, float normX, float normY);

    void releaseAllTouches();
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
    static void emitTouchToBridge(void* ctx, TouchAction action, uint64_t pointerId,
                                  int32_t px, int32_t py, bool clampToScreen);

    void runLoop();
    void processTick(double nowMs);
    void rebuildKeyRouteCacheLocked();
    void syncFpsViewConfigLocked(uint32_t w, uint32_t h);
    bool classifyKeyInputLocked(int vk, bool down, bool isAim, bool& outRequestToggleAim);
    void processKeyEventLocked(int vk, bool down, double nowMs);
    void processDirectCursorLocked(TouchAction action, float normX, float normY);

    void pressFireLocked(double nowMs);
    void liftFireFingerLocked();
    void releaseMouseTouchesLocked();
    void releaseAllLocked();

    ScrcpyBridge* bridge_;

    mutable CRITICAL_SECTION lock_;      // Worker thread state & touch dispatch
    mutable CRITICAL_SECTION inputLock_; // Microsecond lock for non-blocking hook queues
    Profile profile_;

    HANDLE thread_ = NULL;
    HANDLE wakeEvent_ = NULL;
    volatile LONG stopFlag_ = 0;
    volatile LONG aimMode_ = 0;

    // Non-blocking hook input queues & route cache (protected by inputLock_)
    std::vector<QueuedKeyEvent> pendingKeys_;
    std::vector<QueuedCursorTouch> pendingCursor_;
    bool hookHeldKeys_[512];
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

    // Modular sub-controllers
    FpsViewStack viewStack_;
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
