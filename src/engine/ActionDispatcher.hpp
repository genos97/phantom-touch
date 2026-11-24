#pragma once

#include "bridge/TouchTypes.hpp"
#include "config/Profile.hpp"

#include <cstdint>
#include <vector>

struct ActionEffects {
    bool pushHoldView = false;
    int holdViewIdx = -1;
    bool popHoldView = false;
    int popHoldViewIdx = -1;
    bool resetMainView = false;
    bool reactivateJoystick = false;
    bool scopeSensChanged = false;
    bool scopeSensValue = false;
};

// Manages 2-level hotkey lookup (2-key combos + single keys), tap jitter,
// timed clicks, hold-to-ADS, directional swipes, and all 10 KeyActionType nodes.
class ActionDispatcher {
public:
    ActionDispatcher();

    void setTouchSink(TouchEmitFn fn, void* ctx);
    void resetForProfile(size_t buttonCount);

    // Executes a key/mouse/wheel press or release across profile.buttons
    // When toggleOnly is true (unlocked mouse buttons), only NODE_CURSOR_TOGGLE fires.
    void dispatchKey(int vk, bool down, bool isAimMode,
                     const Profile& profile, const bool heldKeys[512],
                     uint32_t screenW, uint32_t screenH, double nowMs,
                     bool& ioScopeSensActive, ActionEffects& outEffects,
                     uint32_t& outPacketsSent, bool toggleOnly = false);

    // Steps scheduled auto-release taps and smooth directional swipes
    void tick(const Profile& profile, double nowMs, uint32_t& outPacketsSent);

    // Releases held touches (mouseOnly = true preserves keyboard buttons when unlocking cursor)
    void releaseButtons(const Profile& profile, bool mouseOnly, uint32_t& outPacketsSent);

private:
    struct ButtonRuntime {
        bool keyHeld = false;
        bool touchDown = false;
        int32_t lastPx = 0;
        int32_t lastPy = 0;
        double autoReleaseAtMs = 0.0;
        bool swiping = false;
        double swipeStartMs = 0.0;
        double swipeDurMs = 0.0;
        double swipeX1 = 0.0, swipeY1 = 0.0;
        double swipeX2 = 0.0, swipeY2 = 0.0;
    };

    void emitTouch(TouchAction action, uint64_t ptrId, int32_t px, int32_t py, uint32_t& outPacketsSent);
    void sendTapWithJitter(TouchAction action, uint64_t ptrId, float nx, float ny,
                           int jitterPx, uint32_t screenW, uint32_t screenH,
                           int32_t& ioPx, int32_t& ioPy, uint32_t& outPacketsSent);

    void triggerStart(size_t idx, bool isAimMode, const Profile& profile,
                      uint32_t screenW, uint32_t screenH, double nowMs,
                      bool& ioScopeSensActive, ActionEffects& outEffects,
                      uint32_t& outPacketsSent);
    void triggerStop(size_t idx, const Profile& profile,
                     uint32_t screenW, uint32_t screenH, double nowMs,
                     bool& ioScopeSensActive, ActionEffects& outEffects,
                     uint32_t& outPacketsSent);

    TouchEmitFn emitFn_ = nullptr;
    void* emitCtx_ = nullptr;
    std::vector<ButtonRuntime> states_;
    uint32_t rngState_ = 0x13579BDFu;
};
