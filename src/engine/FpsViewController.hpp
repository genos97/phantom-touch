#pragma once

#include "bridge/TouchTypes.hpp"

#include <cstdint>

// ============================================================================
// Phantom Touch â€” Standalone 360Â° Virtual-Finger FPS View Controller
// ============================================================================
// Clean-room C++11 implementation of a touchscreen virtual-finger camera
// state machine for 360Â° player view control over Android multi-touch:
//
//   1. Lazy Touch-Down: Locking the mouse focuses the view without touching
//      the screen until the first >=1px mouse delta arrives.
//   2. Sub-Pixel Accumulation: Scaled mouse deltas accumulate in double
//      precision; only whole integer pixels emit TOUCH_MOVE while exact
//      fractional remainders carry over (zero lost micro-aim).
//   3. Dual Boundary Modes:
//      - Unbounded (Free Fire): infinite travel rect, exact center landing,
//        optional nonlinear horizontal counter-acceleration.
//      - Boxed (PUBG/CODM/custom): bounded move rect with Â±50px landing
//        jitter; exiting the boundary emits TOUCH_UP, waits >=15ms
//        non-blockingly (touchDownDelay floor), and seamlessly re-plants
//        near center on the next delta for infinite 360Â° rotation.
//   4. Idle Auto-Release: Re-centers after inactivity so the next flick
//      starts near center with full travel room.
//   5. View Stack (FpsViewStack): Supports pushing temporary mouse-linked
//      drag controls (e.g. Free Look Eye) above the main camera finger.
// ============================================================================

struct FpsViewConfig {
    double centerX = 1200.0;
    double centerY = 600.0;

    // Virtual finger movement boundary in native screen pixels
    double rectLeft   = 50.0;
    double rectTop    = 50.0;
    double rectRight  = 1870.0;
    double rectBottom = 1030.0;
    double screenW    = 1920.0;
    double screenH    = 1080.0;

    // Sensitivity & physics
    double sensX = 0.95;
    double sensY = 1.15;
    double ffCounterAccel = 7.0;  // Nonlinear horizontal damping k (0 = pure linear)
    bool invertY = false;
    bool unbounded = true;        // Infinite travel bounds + exact center landing

    // Virtual finger lifecycle policies
    bool clampInsteadOfRelease = false; // Clamp to moveRect (used by linked Free-Look buttons)
    bool edgeReleaseOnly = false;       // Disable idle auto-release timer when true
    int touchDownRandomRangePx = 0;     // 0 in Free Fire; 50 in boxed mode (Â±50px around center)
    int touchDownDelayMs = 15;          // Minimum 15ms separation between UP and next DOWN
    int touchUpDelayMs = 0;             // Optional pause before lifting at boundary
    int idleReleaseMs = 700;            // Inactivity auto-recenter interval (ms)
};

class FpsViewFinger {
public:
    FpsViewFinger(uint64_t primaryPtrId, uint64_t altPtrId);

    void setTouchSink(TouchEmitFn fn, void* ctx);
    void configure(const FpsViewConfig& cfg);
    const FpsViewConfig& config() const { return cfg_; }

    // Focus lifecycle (entering / leaving locked FPS view)
    void onFocusGain(double nowMs);
    void onFocusLose(double nowMs);

    // Immediately places the virtual finger down at center (used by CLAW-LEFT fire & linked Free-Look)
    bool forcePlaceAtCenter(double nowMs);

    // Explicitly lifts the virtual finger (returns true if a TOUCH_UP was emitted)
    bool releaseFinger(double nowMs);

    // Processes scaled/raw mouse deltas with sub-pixel accumulation and boundary management
    // multX / multY allow temporary multipliers (e.g. fireSensX/Y or scopeSensX/Y)
    void onMouseDelta(double rawDx, double rawDy, double multX, double multY,
                      double nowMs, uint32_t& outPacketsSent);

    // Periodic housekeeping: delayed edge-up completion & idle auto-release
    void tick(double nowMs, bool suppressIdleRelease, uint32_t& outPacketsSent);

    // State inspection for HUD telemetry and unit testing
    bool isFocused() const { return focused_; }
    bool isFingerPlaced() const { return fingerPlaced_; }
    uint64_t activePointerId() const { return activePtrId_; }
    double curX() const { return curX_; }
    double curY() const { return curY_; }
    double originX() const { return originX_; }
    double originY() const { return originY_; }

private:
    bool tryPlaceFinger(double nowMs, uint32_t& outPacketsSent);
    void emitTouch(TouchAction action, uint64_t ptrId, int32_t px, int32_t py,
                   bool clampToScreen, uint32_t& outPacketsSent);
    int nextRandRange(int range);

    FpsViewConfig cfg_;
    TouchEmitFn emitFn_ = nullptr;
    void* emitCtx_ = nullptr;

    uint64_t primaryPtrId_ = 2;
    uint64_t altPtrId_ = 4;
    uint64_t activePtrId_ = 2;

    bool focused_ = false;
    bool fingerPlaced_ = false;

    double originX_ = 0.0;
    double originY_ = 0.0;
    double curX_ = 0.0;
    double curY_ = 0.0;
    int32_t lastSentX_ = -1;
    int32_t lastSentY_ = -1;

    // Sub-pixel fractional remainder accumulators
    double accX_ = 0.0;
    double accY_ = 0.0;

    // Timing state for non-blocking >=15ms touchDownDelay & idle auto-release
    double lastMoveMs_ = 0.0;
    double lastReleaseMs_ = -10000.0;
    bool pendingUp_ = false;
    double pendingUpAtMs_ = 0.0;

    uint32_t rngState_ = 0x2468ACE1u;
};

// Manages the 2-level FPS View Stack:
//   Index 0: Main 360Â° camera finger (PTR_AIM_A=2 / PTR_AIM_B=4)
//   Index 1: Optional temporary mouse-linked drag view (e.g. Free Look Eye, PTR_HOLD_VIEW=5)
class FpsViewStack {
public:
    FpsViewStack(uint64_t mainPtrA, uint64_t mainPtrB, uint64_t overlayPtr);

    void setTouchSink(TouchEmitFn fn, void* ctx);
    void configureMain(const FpsViewConfig& cfg);

    void onFocusGain(double nowMs);
    void onFocusLose(double nowMs);

    // Push / pop a temporary mouse-linked drag view (e.g. Free Look Eye button)
    void pushOverlay(int ownerId, const FpsViewConfig& overlayCfg, double nowMs, uint32_t& outPacketsSent);
    void popOverlay(int ownerId, double nowMs, uint32_t& outPacketsSent);
    bool isOverlayActive() const { return overlayActive_; }
    int overlayOwnerId() const { return overlayOwnerId_; }

    // Ensure main camera finger is planted (e.g. when CLAW-LEFT / DUAL-DRAG fire starts)
    void ensureMainFingerDown(double nowMs, uint32_t& outPacketsSent);

    // Cleanly reset the active camera view back to center
    void resetMainView(double nowMs);

    // Route mouse movement to top of stack (overlay if active, else main camera)
    void onMouseDelta(double rawDx, double rawDy, double multX, double multY,
                      double nowMs, uint32_t& outPacketsSent);

    void tick(double nowMs, bool suppressIdleRelease, uint32_t& outPacketsSent);

    const FpsViewFinger& mainView() const { return mainView_; }
    const FpsViewFinger& overlayView() const { return overlayView_; }
    const FpsViewFinger& activeView() const { return overlayActive_ ? overlayView_ : mainView_; }

private:
    FpsViewFinger mainView_;
    FpsViewFinger overlayView_;
    bool overlayActive_ = false;
    int overlayOwnerId_ = -1;
};
