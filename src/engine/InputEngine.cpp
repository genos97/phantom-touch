#include "engine/InputEngine.hpp"
#include "engine/ThreadScheduling.hpp"

#include "config/KeyCodes.hpp"

#include <mmsystem.h>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

double getTimeMs() {
    static LARGE_INTEGER freq;
    if (freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (static_cast<double>(now.QuadPart) * 1000.0) / static_cast<double>(freq.QuadPart);
}

double clampd(double v, double lo, double hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

typedef HANDLE (WINAPI *PFN_CreateWaitableTimerExW)(
    LPSECURITY_ATTRIBUTES lpTimerAttributes,
    LPCWSTR lpTimerName,
    DWORD dwFlags,
    DWORD dwDesiredAccess);

HANDLE createHighResTimer() {
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    if (k32) {
        PFN_CreateWaitableTimerExW fn = reinterpret_cast<PFN_CreateWaitableTimerExW>(
            reinterpret_cast<void*>(GetProcAddress(k32, "CreateWaitableTimerExW")));
        if (fn) {
            // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION = 0x00000002 (Win10 1803+)
            HANDLE h = fn(NULL, NULL, 0x00000002u, TIMER_ALL_ACCESS);
            if (h) return h;
        }
    }
    return CreateWaitableTimerW(NULL, TRUE, NULL);
}

} // namespace

void InputEngine::emitTouchToBridge(void* ctx, TouchAction action, uint64_t pointerId,
                                    int32_t px, int32_t py, bool clampToScreen) {
    ScrcpyBridge* br = reinterpret_cast<ScrcpyBridge*>(ctx);
    if (br) {
        br->sendTouchPixels(action, pointerId, px, py, clampToScreen);
    }
}

void InputEngine::emitTouchThroughPool(void* ctx, TouchAction action, uint64_t logicalId,
                                       int32_t px, int32_t py, bool clampToScreen) {
    InputEngine* self = reinterpret_cast<InputEngine*>(ctx);
    if (self) {
        self->fingerPool_.emit(action, logicalId, px, py, clampToScreen);
    }
}

void InputEngine::setTouchSinkForTest(TouchEmitFn fn, void* ctx) {
    EnterCriticalSection(&lock_);
    fingerPool_.setDownstreamSink(fn ? fn : emitTouchToBridge, fn ? ctx : bridge_);
    LeaveCriticalSection(&lock_);
}

InputEngine::InputEngine(ScrcpyBridge* bridge)
    : bridge_(bridge),
      viewStack_(PointerId::AIM_A, PointerId::AIM_B, PointerId::HOLD_VIEW) {
    InitializeCriticalSection(&lock_);
    InitializeCriticalSection(&inputLock_);
    wakeEvent_ = CreateEventA(NULL, FALSE, FALSE, NULL);
    hiResTimer_ = createHighResTimer();
    std::memset(heldKeys_, 0, sizeof(heldKeys_));
    std::memset(hookHeldKeys_, 0, sizeof(hookHeldKeys_));
    std::memset(buttonGlowUntilTick_, 0, sizeof(buttonGlowUntilTick_));

    rawProfile_ = ProfileDefaults::createFreeFire();
    profile_ = rawProfile_;
    InterlockedExchange(&pollHzAtomic_, profile_.pollHz);

    fingerPool_.setDownstreamSink(emitTouchToBridge, bridge_);
    viewStack_.setTouchSink(emitTouchThroughPool, this);
    joystick_.setTouchSink(emitTouchThroughPool, this);
    gamepad_.setTouchSink(emitTouchThroughPool, this);
    actions_.setTouchSink(emitTouchThroughPool, this);
    actions_.resetForProfile(profile_.buttons.size());

    uint32_t w = 1920, h = 1080;
    if (bridge_) bridge_->getScreenSize(w, h);
    syncFpsViewConfigLocked(w, h);

    EnterCriticalSection(&inputLock_);
    rebuildKeyRouteCacheLocked();
    LeaveCriticalSection(&inputLock_);
}

InputEngine::~InputEngine() {
    stop();
    if (hiResTimer_) {
        CloseHandle(hiResTimer_);
        hiResTimer_ = NULL;
    }
    if (wakeEvent_) {
        CloseHandle(wakeEvent_);
        wakeEvent_ = NULL;
    }
    DeleteCriticalSection(&inputLock_);
    DeleteCriticalSection(&lock_);
}

void InputEngine::rebuildKeyRouteCacheLocked() {
    routeKeymapEnabled_ = profile_.keymapEnabled;
    routeVkUp_     = profile_.vkUp;
    routeVkDown_   = profile_.vkDown;
    routeVkLeft_   = profile_.vkLeft;
    routeVkRight_  = profile_.vkRight;
    routeVkSprint_ = profile_.vkSprint;
    routeVkWalk_   = profile_.vkWalk;
    routeFireVk_   = profile_.fireVk;
    routeCastCancelVk_ = profile_.castCancelVk;
    routeHasSkillCast_ = false;
    routeButtons_.clear();
    routeButtons_.reserve(profile_.buttons.size());
    for (size_t i = 0; i < profile_.buttons.size(); ++i) {
        KeyRouteEntry e;
        e.comboVk = profile_.buttons[i].comboVk;
        e.vk      = profile_.buttons[i].vk;
        e.action  = profile_.buttons[i].action;
        if (e.action == NODE_SKILL_CAST) routeHasSkillCast_ = true;
        routeButtons_.push_back(e);
    }
}

void InputEngine::syncFpsViewConfigLocked(uint32_t w, uint32_t h) {
    if (w == 0) w = 1920;
    if (h == 0) h = 1080;
    profile_ = ProfileDefaults::resolveLayout(rawProfile_, w, h);
    double dw = static_cast<double>(w);
    double dh = static_cast<double>(h);

    FpsViewConfig cfg;
    cfg.screenW = dw;
    cfg.screenH = dh;
    cfg.centerX = static_cast<double>(profile_.aimX) * dw;
    cfg.centerY = static_cast<double>(profile_.aimY) * dh;
    cfg.sensX = static_cast<double>(profile_.sensX);
    cfg.sensY = static_cast<double>(profile_.sensY);
    cfg.ffCounterAccel = static_cast<double>(profile_.ffCounterAccel);
    cfg.invertY = profile_.invertY;
    cfg.unbounded = profile_.unboundedAim;
    cfg.clampInsteadOfRelease = false;
    cfg.touchDownDelayMs = (profile_.touchDownDelayMs >= 15) ? profile_.touchDownDelayMs : 15;
    cfg.touchUpDelayMs = profile_.touchUpDelayMs;
    cfg.idleReleaseMs = profile_.idleResetMs;
    cfg.edgeReleaseOnly = (profile_.idleResetMs <= 0);

    if (profile_.unboundedAim) {
        cfg.rectLeft   = -99999999.0;
        cfg.rectTop    = -99999999.0;
        cfg.rectRight  =  99999999.0;
        cfg.rectBottom =  99999999.0;
        cfg.touchDownRandomRangePx = 0;
    } else {
        double margin = 50.0;
        double minX = clampd(cfg.centerX - static_cast<double>(profile_.aimHalfW) * dw, margin, dw - margin);
        double maxX = clampd(cfg.centerX + static_cast<double>(profile_.aimHalfW) * dw, margin, dw - margin);
        double minY = clampd(cfg.centerY - static_cast<double>(profile_.aimHalfH) * dh, margin, dh - margin);
        double maxY = clampd(cfg.centerY + static_cast<double>(profile_.aimHalfH) * dh, margin, dh - margin);
        if (maxX - minX < 120.0) { minX = margin; maxX = dw - margin; }
        if (maxY - minY < 120.0) { minY = margin; maxY = dh - margin; }
        cfg.rectLeft   = minX;
        cfg.rectTop    = minY;
        cfg.rectRight  = maxX;
        cfg.rectBottom = maxY;
        cfg.touchDownRandomRangePx = 0; // P2: deterministic center re-plant (no artificial jitter)
    }

    viewStack_.configureMain(cfg);
    lastConfiguredW_ = w;
    lastConfiguredH_ = h;
}

void InputEngine::start() {
    if (thread_ != NULL) return;
    InterlockedExchange(&stopFlag_, 0);
    thread_ = CreateThread(NULL, 0, tickThreadProc, this, 0, NULL);
    if (thread_) {
        SetThreadPriority(thread_, THREAD_PRIORITY_HIGHEST);
    }
}

void InputEngine::stop() {
    if (thread_ == NULL) return;
    InterlockedExchange(&stopFlag_, 1);
    if (wakeEvent_) SetEvent(wakeEvent_);
    WaitForSingleObject(thread_, 1500);
    CloseHandle(thread_);
    thread_ = NULL;
    releaseAllTouches();
}

void InputEngine::setProfile(const Profile& profile) {
    EnterCriticalSection(&lock_);
    releaseAllLocked();
    rawProfile_ = profile;
    profile_ = profile;
    InterlockedExchange(&pollHzAtomic_, profile_.pollHz);
    actions_.resetForProfile(profile_.buttons.size());
    uint32_t w = 1920, h = 1080;
    bridge_->getScreenSize(w, h);
    syncFpsViewConfigLocked(w, h);
    EnterCriticalSection(&inputLock_);
    pendingKeys_.clear();
    pendingCursor_.clear();
    pendingScroll_.clear();
    std::memset(hookHeldKeys_, 0, sizeof(hookHeldKeys_));
    rebuildKeyRouteCacheLocked();
    LeaveCriticalSection(&inputLock_);
    LeaveCriticalSection(&lock_);
}

void InputEngine::updateTuning(const Profile& profile) {
    // N28: live-update sensitivities, fireMode, HUD flags, pollHz WITHOUT releasing active touches!
    EnterCriticalSection(&lock_);
    auto copyTuning = [&](Profile& dst) {
        dst.sensX              = profile.sensX;
        dst.sensY              = profile.sensY;
        dst.fireSensX          = profile.fireSensX;
        dst.fireSensY          = profile.fireSensY;
        dst.scopeSensX         = profile.scopeSensX;
        dst.scopeSensY         = profile.scopeSensY;
        dst.accelPower         = profile.accelPower;
        dst.accelRefSpeed      = profile.accelRefSpeed;
        dst.accelCap           = profile.accelCap;
        dst.smoothing          = profile.smoothing;
        dst.ffCounterAccel     = profile.ffCounterAccel;
        dst.invertY            = profile.invertY;
        dst.unboundedAim       = profile.unboundedAim;
        dst.fireMode           = profile.fireMode;
        dst.showHudInPlay      = profile.showHudInPlay;
        dst.hudOpacity         = profile.hudOpacity;
        dst.pollHz             = profile.pollHz;
        dst.idleResetMs        = profile.idleResetMs;
        dst.touchDownDelayMs   = profile.touchDownDelayMs;
        dst.touchUpDelayMs     = profile.touchUpDelayMs;
        dst.tapJitterPx        = profile.tapJitterPx;
        dst.rememberCursorPos  = profile.rememberCursorPos;
        dst.cursorScrollStepPx = profile.cursorScrollStepPx;
        dst.joyWalkRadius      = profile.joyWalkRadius;
        dst.joySteppedGlide    = profile.joySteppedGlide;
        dst.fireBoxHalfW       = profile.fireBoxHalfW;
        dst.fireBoxHalfH       = profile.fireBoxHalfH;
        dst.padMode            = profile.padMode;
        dst.padDeadzoneL       = profile.padDeadzoneL;
        dst.padDeadzoneR       = profile.padDeadzoneR;
        dst.padLookSens        = profile.padLookSens;
    };
    copyTuning(rawProfile_);
    copyTuning(profile_);
    InterlockedExchange(&pollHzAtomic_, profile_.pollHz);
    uint32_t w = 1920, h = 1080;
    bridge_->getScreenSize(w, h);
    syncFpsViewConfigLocked(w, h);
    LeaveCriticalSection(&lock_);
}

Profile InputEngine::getProfileCopy() const {
    EnterCriticalSection(&lock_);
    Profile copy = rawProfile_;
    LeaveCriticalSection(&lock_);
    return copy;
}

void InputEngine::setGamepadEnabled(bool enabled) {
    EnterCriticalSection(&lock_);
    gamepad_.setEnabled(enabled);
    if (!enabled) {
        uint32_t w = 1920, h = 1080;
        bridge_->getScreenSize(w, h);
        GamepadFrameOutput gpOut;
        gamepad_.pollAndProcess(profile_, w, h, 0.0, packetsThisSec_, gpOut, true, shellMenuOpen_ != 0);
        for (int i = 0; i < gpOut.keyDeltaCount; ++i) {
            processKeyEventLocked(gpOut.keyDeltas[i].vk, gpOut.keyDeltas[i].down, getTimeMs(), true);
        }
        if (gpOut.uhidNeedDestroy) {
            bridge_->sendUhidDestroy(3);
        }
        if (joyOwner_ == JOY_GAMEPAD) joyOwner_ = JOY_NONE;
        prevGpStickDeflected_ = false;
    }
    LeaveCriticalSection(&lock_);
    if (wakeEvent_) SetEvent(wakeEvent_);
}

void InputEngine::setShellMenuOpen(bool open) {
    LONG val = open ? 1 : 0;
    if (InterlockedExchange(&shellMenuOpen_, val) != val && wakeEvent_) {
        SetEvent(wakeEvent_);
    }
}

void InputEngine::setPadNavCallback(PadNavCallbackFn fn, void* ctx) {
    EnterCriticalSection(&lock_);
    padNavCb_ = fn;
    padNavCtx_ = ctx;
    LeaveCriticalSection(&lock_);
}

void InputEngine::setAppFocused(bool focused) {
    LONG val = focused ? 1 : 0;
    if (InterlockedExchange(&appFocused_, val) != val && wakeEvent_) {
        SetEvent(wakeEvent_);
    }
}

void InputEngine::setEditMode(bool editing) {
    LONG val = editing ? 1 : 0;
    if (InterlockedExchange(&editMode_, val) != val && wakeEvent_) {
        SetEvent(wakeEvent_);
    }
}

void InputEngine::setAimMode(bool enabled) {
    bool wasEnabled = (aimMode_ != 0);
    if (wasEnabled == enabled) return;

    double nowMs = getTimeMs();
    EnterCriticalSection(&lock_);
    releaseMouseTouchesLocked();
    InterlockedExchange(&aimMode_, enabled ? 1 : 0);
    uint32_t w = 1920, h = 1080;
    bridge_->getScreenSize(w, h);
    syncFpsViewConfigLocked(w, h);
    if (enabled) {
        viewStack_.onFocusGain(nowMs);
    } else {
        viewStack_.onFocusLose(nowMs);
    }
    EnterCriticalSection(&inputLock_);
    accumRawDx_ = 0.0;
    accumRawDy_ = 0.0;
    hasPendingAimDelta_ = false;
    pendingCursor_.clear();
    pendingScroll_.clear();
    LeaveCriticalSection(&inputLock_);
    smoothDx_ = 0.0;
    smoothDy_ = 0.0;
    scopeSensActive_ = false;
    LeaveCriticalSection(&lock_);
    if (wakeEvent_) SetEvent(wakeEvent_);
}

void InputEngine::onRawMouseDelta(int dx, int dy) {
    if (!aimMode_ || (dx == 0 && dy == 0)) return;
    EnterCriticalSection(&inputLock_);
    accumRawDx_ += static_cast<double>(dx);
    accumRawDy_ += static_cast<double>(dy);
    hasPendingAimDelta_ = true;
    LeaveCriticalSection(&inputLock_);
    if (wakeEvent_) SetEvent(wakeEvent_);
}

void InputEngine::onCursorHoverNorm(float normX, float normY) {
    if (normX < 0.0f) normX = 0.0f;
    if (normX > 1.0f) normX = 1.0f;
    if (normY < 0.0f) normY = 0.0f;
    if (normY > 1.0f) normY = 1.0f;
    EnterCriticalSection(&inputLock_);
    cursorHoverNormX_ = normX;
    cursorHoverNormY_ = normY;
    LeaveCriticalSection(&inputLock_);
    if (thread_ == NULL) {
        EnterCriticalSection(&lock_);
        actions_.setCursorNorm(normX, normY);
        LeaveCriticalSection(&lock_);
    } else if (actions_.hasActiveSkillCast() && wakeEvent_) {
        SetEvent(wakeEvent_);
    }
}

void InputEngine::onDirectCursorTouch(TouchAction action, float normX, float normY) {
    if (aimMode_) return;
    if (thread_ == NULL) {
        EnterCriticalSection(&lock_);
        processDirectCursorLocked(action, normX, normY);
        LeaveCriticalSection(&lock_);
        return;
    }
    EnterCriticalSection(&inputLock_);
    cursorHoverNormX_ = static_cast<float>(clampd(normX, 0.0, 1.0));
    cursorHoverNormY_ = static_cast<float>(clampd(normY, 0.0, 1.0));
    if (action == TOUCH_MOVE && !pendingCursor_.empty() &&
        pendingCursor_.back().action == TOUCH_MOVE && !pendingCursor_.back().twoFingerPan) {
        pendingCursor_.back().normX = normX;
        pendingCursor_.back().normY = normY;
    } else {
        QueuedCursorTouch q = { action, normX, normY, false };
        pendingCursor_.push_back(q);
    }
    LeaveCriticalSection(&inputLock_);
    if (wakeEvent_) SetEvent(wakeEvent_);
}

void InputEngine::onDirectCursorTwoFingerPan(TouchAction action, float normX, float normY) {
    if (aimMode_) return;
    if (thread_ == NULL) {
        EnterCriticalSection(&lock_);
        processDirectCursorTwoFingerPanLocked(action, normX, normY);
        LeaveCriticalSection(&lock_);
        return;
    }
    EnterCriticalSection(&inputLock_);
    if (action == TOUCH_MOVE && !pendingCursor_.empty() &&
        pendingCursor_.back().action == TOUCH_MOVE && pendingCursor_.back().twoFingerPan) {
        pendingCursor_.back().normX = normX;
        pendingCursor_.back().normY = normY;
    } else {
        QueuedCursorTouch q = { action, normX, normY, true };
        pendingCursor_.push_back(q);
    }
    LeaveCriticalSection(&inputLock_);
    if (wakeEvent_) SetEvent(wakeEvent_);
}

void InputEngine::onDirectCursorScroll(float normX, float normY, int wheelNotches) {
    if (aimMode_ || wheelNotches == 0) return;
    double nowMs = getTimeMs();
    if (thread_ == NULL) {
        EnterCriticalSection(&lock_);
        processDirectCursorScrollLocked(normX, normY, wheelNotches, nowMs);
        LeaveCriticalSection(&lock_);
        return;
    }
    EnterCriticalSection(&inputLock_);
    if (!pendingScroll_.empty() && !pendingScroll_.back().isPinch) {
        pendingScroll_.back().normX = normX;
        pendingScroll_.back().normY = normY;
        pendingScroll_.back().notches += wheelNotches;
    } else {
        QueuedCursorScroll q = { normX, normY, wheelNotches, false };
        pendingScroll_.push_back(q);
    }
    LeaveCriticalSection(&inputLock_);
    if (wakeEvent_) SetEvent(wakeEvent_);
}

void InputEngine::onDirectCursorPinch(float normX, float normY, int wheelNotches) {
    if (aimMode_ || wheelNotches == 0) return;
    double nowMs = getTimeMs();
    if (thread_ == NULL) {
        EnterCriticalSection(&lock_);
        processDirectCursorPinchLocked(normX, normY, wheelNotches, nowMs);
        LeaveCriticalSection(&lock_);
        return;
    }
    EnterCriticalSection(&inputLock_);
    if (!pendingScroll_.empty() && pendingScroll_.back().isPinch) {
        pendingScroll_.back().normX = normX;
        pendingScroll_.back().normY = normY;
        pendingScroll_.back().notches += wheelNotches;
    } else {
        QueuedCursorScroll q = { normX, normY, wheelNotches, true };
        pendingScroll_.push_back(q);
    }
    LeaveCriticalSection(&inputLock_);
    if (wakeEvent_) SetEvent(wakeEvent_);
}

void InputEngine::processDirectCursorLocked(TouchAction action, float normX, float normY) {
    uint32_t w = 1920, h = 1080;
    bridge_->getScreenSize(w, h);
    actions_.setCursorNorm(normX, normY);
    int32_t px = static_cast<int32_t>(normX * static_cast<float>(w) + 0.5f);
    int32_t py = static_cast<int32_t>(normY * static_cast<float>(h) + 0.5f);

    if (action == TOUCH_DOWN) {
        releaseCursorPinchLocked();
        releaseCursorTwoFingerPanLocked();
        directCursorDown_ = true;
        directCursorLastX_ = px;
        directCursorLastY_ = py;
        fingerPool_.emit(TOUCH_DOWN, PointerId::CURSOR, px, py, true);
        ++packetsThisSec_;
    } else if (action == TOUCH_MOVE) {
        if (directCursorDown_) {
            directCursorLastX_ = px;
            directCursorLastY_ = py;
            if (fingerPool_.emit(TOUCH_MOVE, PointerId::CURSOR, px, py, true)) {
                ++packetsThisSec_;
            }
        }
    } else if (action == TOUCH_UP) {
        if (directCursorDown_) {
            directCursorDown_ = false;
            directCursorLastX_ = px;
            directCursorLastY_ = py;
            fingerPool_.emit(TOUCH_UP, PointerId::CURSOR, px, py, true);
            ++packetsThisSec_;
        }
    }
}

void InputEngine::processDirectCursorTwoFingerPanLocked(TouchAction action, float normX, float normY) {
    uint32_t w = 1920, h = 1080;
    bridge_->getScreenSize(w, h);
    int32_t maxX = static_cast<int32_t>(w > 10 ? w - 5 : 1915);
    int32_t maxY = static_cast<int32_t>(h > 10 ? h - 5 : 1075);
    auto clampX = [&](int32_t v) -> int32_t { return (v < 4) ? 4 : (v > maxX ? maxX : v); };
    auto clampY = [&](int32_t v) -> int32_t { return (v < 4) ? 4 : (v > maxY ? maxY : v); };
    int32_t px = clampX(static_cast<int32_t>(normX * static_cast<float>(w) + 0.5f));
    int32_t py = clampY(static_cast<int32_t>(normY * static_cast<float>(h) + 0.5f));
    const int32_t spread = 48;

    if (action == TOUCH_DOWN) {
        releaseCursorPinchLocked();
        releaseCursorScrollLocked();
        twoFingerPanDown_ = true;
        twoFingerPanLastX_ = px;
        twoFingerPanLastY_ = py;
        fingerPool_.emit(TOUCH_DOWN, PointerId::PINCH_A, clampX(px - spread), py, true);
        fingerPool_.emit(TOUCH_DOWN, PointerId::PINCH_B, clampX(px + spread), py, true);
        packetsThisSec_ += 2;
    } else if (action == TOUCH_MOVE) {
        if (twoFingerPanDown_ && (px != twoFingerPanLastX_ || py != twoFingerPanLastY_)) {
            twoFingerPanLastX_ = px;
            twoFingerPanLastY_ = py;
            if (fingerPool_.emit(TOUCH_MOVE, PointerId::PINCH_A, clampX(px - spread), py, true)) ++packetsThisSec_;
            if (fingerPool_.emit(TOUCH_MOVE, PointerId::PINCH_B, clampX(px + spread), py, true)) ++packetsThisSec_;
        }
    } else if (action == TOUCH_UP) {
        releaseCursorTwoFingerPanLocked();
    }
}

void InputEngine::releaseCursorTwoFingerPanLocked() {
    if (twoFingerPanDown_) {
        fingerPool_.releaseLogical(PointerId::PINCH_A);
        fingerPool_.releaseLogical(PointerId::PINCH_B);
        twoFingerPanDown_ = false;
        packetsThisSec_ += 2;
    }
}

void InputEngine::processDirectCursorScrollLocked(float normX, float normY, int wheelNotches, double nowMs) {
    if (wheelNotches == 0) return;
    releaseCursorPinchLocked();
    releaseCursorTwoFingerPanLocked();
    uint32_t w = 1920, h = 1080;
    bridge_->getScreenSize(w, h);
    double dw = static_cast<double>(w > 0 ? w : 1920);
    double dh = static_cast<double>(h > 0 ? h : 1080);

    double anchorX = clampd(static_cast<double>(normX) * dw, 10.0, dw - 11.0);
    double anchorY = clampd(static_cast<double>(normY) * dh, 10.0, dh - 11.0);
    double stepPx = static_cast<double>(profile_.cursorScrollStepPx > 0 ? profile_.cursorScrollStepPx : 95);
    double dy = static_cast<double>(wheelNotches) * stepPx;

    if (cursorScrollDown_) {
        // Re-anchor smoothly if cursor jumped far away or drag hit top/bottom screen edge
        double targetY = cursorScrollCurY_ + dy;
        bool hitEdge = (targetY <= 8.0 && dy < 0.0) || (targetY >= dh - 9.0 && dy > 0.0);
        bool movedFar = (std::fabs(anchorX - cursorScrollCurX_) > dw * 0.15);
        if (hitEdge || movedFar) {
            releaseCursorScrollLocked();
        }
    }

    if (!cursorScrollDown_) {
        cursorScrollCurX_ = anchorX;
        cursorScrollCurY_ = anchorY;
        int32_t sx = static_cast<int32_t>(std::floor(cursorScrollCurX_ + 0.5));
        int32_t sy = static_cast<int32_t>(std::floor(cursorScrollCurY_ + 0.5));
        fingerPool_.emit(TOUCH_DOWN, PointerId::SCROLL, sx, sy, true);
        cursorScrollDown_ = true;
        ++packetsThisSec_;
    }

    cursorScrollCurY_ = clampd(cursorScrollCurY_ + dy, 8.0, dh - 9.0);
    int32_t mx = static_cast<int32_t>(std::floor(cursorScrollCurX_ + 0.5));
    int32_t my = static_cast<int32_t>(std::floor(cursorScrollCurY_ + 0.5));
    if (fingerPool_.emit(TOUCH_MOVE, PointerId::SCROLL, mx, my, true)) {
        ++packetsThisSec_;
    }
    cursorScrollReleaseAtMs_ = nowMs + 80.0;
}

void InputEngine::releaseCursorScrollLocked() {
    if (cursorScrollDown_) {
        int32_t ux = static_cast<int32_t>(std::floor(cursorScrollCurX_ + 0.5));
        int32_t uy = static_cast<int32_t>(std::floor(cursorScrollCurY_ + 0.5));
        fingerPool_.emit(TOUCH_UP, PointerId::SCROLL, ux, uy, true);
        cursorScrollDown_ = false;
        cursorScrollReleaseAtMs_ = 0.0;
        ++packetsThisSec_;
    }
}

void InputEngine::processDirectCursorPinchLocked(float normX, float normY, int wheelNotches, double nowMs) {
    if (wheelNotches == 0) return;
    releaseCursorScrollLocked();
    releaseCursorTwoFingerPanLocked();

    uint32_t w = 1920, h = 1080;
    bridge_->getScreenSize(w, h);
    double dw = static_cast<double>(w > 0 ? w : 1920);
    double dh = static_cast<double>(h > 0 ? h : 1080);

    double cx = clampd(static_cast<double>(normX) * dw, 48.0, dw - 49.0);
    double cy = clampd(static_cast<double>(normY) * dh, 48.0, dh - 49.0);
    double maxR = (dw < dh ? dw : dh) * 0.36;
    if (maxR < 80.0) maxR = 80.0;
    const double minR = 24.0;
    const double stepPx = 30.0;

    if (cursorPinchDown_) {
        double nextR = pinchRadius_ + static_cast<double>(wheelNotches) * stepPx;
        if (nextR < minR || nextR > maxR || std::fabs(cx - pinchCenterX_) > dw * 0.15) {
            releaseCursorPinchLocked();
        }
    }

    if (!cursorPinchDown_) {
        pinchCenterX_ = cx;
        pinchCenterY_ = cy;
        pinchRadius_ = (wheelNotches > 0) ? (maxR * 0.28) : (maxR * 0.72);
        if (pinchRadius_ < minR + 10.0) pinchRadius_ = minR + 10.0;
        double d = pinchRadius_ * 0.7071;
        int32_t ax = static_cast<int32_t>(std::floor(clampd(pinchCenterX_ - d, 4.0, dw - 5.0) + 0.5));
        int32_t ay = static_cast<int32_t>(std::floor(clampd(pinchCenterY_ - d, 4.0, dh - 5.0) + 0.5));
        int32_t bx = static_cast<int32_t>(std::floor(clampd(pinchCenterX_ + d, 4.0, dw - 5.0) + 0.5));
        int32_t by = static_cast<int32_t>(std::floor(clampd(pinchCenterY_ + d, 4.0, dh - 5.0) + 0.5));
        fingerPool_.emit(TOUCH_DOWN, PointerId::PINCH_A, ax, ay, true);
        fingerPool_.emit(TOUCH_DOWN, PointerId::PINCH_B, bx, by, true);
        cursorPinchDown_ = true;
        packetsThisSec_ += 2;
    }

    pinchRadius_ = clampd(pinchRadius_ + static_cast<double>(wheelNotches) * stepPx, minR, maxR);
    double d = pinchRadius_ * 0.7071;
    int32_t ax = static_cast<int32_t>(std::floor(clampd(pinchCenterX_ - d, 4.0, dw - 5.0) + 0.5));
    int32_t ay = static_cast<int32_t>(std::floor(clampd(pinchCenterY_ - d, 4.0, dh - 5.0) + 0.5));
    int32_t bx = static_cast<int32_t>(std::floor(clampd(pinchCenterX_ + d, 4.0, dw - 5.0) + 0.5));
    int32_t by = static_cast<int32_t>(std::floor(clampd(pinchCenterY_ + d, 4.0, dh - 5.0) + 0.5));
    if (fingerPool_.emit(TOUCH_MOVE, PointerId::PINCH_A, ax, ay, true)) ++packetsThisSec_;
    if (fingerPool_.emit(TOUCH_MOVE, PointerId::PINCH_B, bx, by, true)) ++packetsThisSec_;
    pinchReleaseAtMs_ = nowMs + 140.0;
}

void InputEngine::releaseCursorPinchLocked() {
    if (cursorPinchDown_) {
        fingerPool_.releaseLogical(PointerId::PINCH_A);
        fingerPool_.releaseLogical(PointerId::PINCH_B);
        cursorPinchDown_ = false;
        pinchReleaseAtMs_ = 0.0;
        packetsThisSec_ += 2;
    }
}

bool InputEngine::classifyKeyInputLocked(int vk, bool down, bool isAim, bool& outRequestToggleAim) {
    outRequestToggleAim = false;
    if (!routeKeymapEnabled_) return false;
    bool isWheel = (vk == VK_CUSTOM_MWHEEL_UP || vk == VK_CUSTOM_MWHEEL_DOWN);
    bool mouseBtn = KeyCodes::isMouseKey(vk);

    if (!isWheel && vk >= 0 && vk < 512) {
        hookHeldKeys_[vk] = down;
    }

    if (!isAim && mouseBtn) {
        if (down) {
            for (size_t i = 0; i < routeButtons_.size(); ++i) {
                if (routeButtons_[i].action == NODE_CURSOR_TOGGLE &&
                    routeButtons_[i].comboVk == 0 && KeyCodes::matches(vk, routeButtons_[i].vk)) {
                    outRequestToggleAim = true;
                    return true;
                }
            }
        }
        return false;
    }

    bool hasCustomBtn = false;
    bool hasToggleBtn = false;
    for (size_t i = 0; i < routeButtons_.size(); ++i) {
        const KeyRouteEntry& e = routeButtons_[i];
        bool comboActive = (e.comboVk != 0 && KeyCodes::matches(vk, e.vk) && KeyCodes::isKeyHeld(hookHeldKeys_, e.comboVk));
        bool singleMatch = (e.comboVk == 0 && KeyCodes::matches(vk, e.vk));
        bool prefixRelease = (!down && e.comboVk != 0 && KeyCodes::matches(vk, e.comboVk));
        if (comboActive || singleMatch || prefixRelease) {
            if (!isAim && e.action == NODE_HOLD_VIEW) continue;
            hasCustomBtn = true;
            if (down && e.action == NODE_CURSOR_TOGGLE && (comboActive || singleMatch)) {
                hasToggleBtn = true;
            }
        }
    }

    if (hasToggleBtn) {
        outRequestToggleAim = true;
        return true;
    }
    if (KeyCodes::matches(vk, routeVkUp_) || KeyCodes::matches(vk, routeVkDown_) ||
        KeyCodes::matches(vk, routeVkLeft_) || KeyCodes::matches(vk, routeVkRight_)) {
        return true;
    }
    if (!hasCustomBtn && (KeyCodes::matches(vk, routeVkSprint_) ||
                          (routeVkWalk_ != 0 && KeyCodes::matches(vk, routeVkWalk_)))) {
        return true;
    }
    if ((isAim || !KeyCodes::isMouseKey(routeFireVk_)) && KeyCodes::matches(vk, routeFireVk_)) {
        return true;
    }
    if (routeHasSkillCast_ && routeCastCancelVk_ != 0 && KeyCodes::matches(vk, routeCastCancelVk_)) {
        return true;
    }
    return hasCustomBtn;
}

bool InputEngine::isKeyMapped(int vk, bool aimModeActive) const {
    EnterCriticalSection(&inputLock_);
    if (!routeKeymapEnabled_) {
        LeaveCriticalSection(&inputLock_);
        return false;
    }
    bool mouseBtn = KeyCodes::isMouseKey(vk);
    if (!aimModeActive && mouseBtn) {
        for (size_t i = 0; i < routeButtons_.size(); ++i) {
            if (routeButtons_[i].action == NODE_CURSOR_TOGGLE &&
                routeButtons_[i].comboVk == 0 && KeyCodes::matches(vk, routeButtons_[i].vk)) {
                LeaveCriticalSection(&inputLock_);
                return true;
            }
        }
        LeaveCriticalSection(&inputLock_);
        return false;
    }
    if (KeyCodes::matches(vk, routeVkUp_) || KeyCodes::matches(vk, routeVkDown_) ||
        KeyCodes::matches(vk, routeVkLeft_) || KeyCodes::matches(vk, routeVkRight_) ||
        KeyCodes::matches(vk, routeVkSprint_) || (routeVkWalk_ != 0 && KeyCodes::matches(vk, routeVkWalk_))) {
        LeaveCriticalSection(&inputLock_);
        return true;
    }
    if ((aimModeActive || !KeyCodes::isMouseKey(routeFireVk_)) && KeyCodes::matches(vk, routeFireVk_)) {
        LeaveCriticalSection(&inputLock_);
        return true;
    }
    for (size_t i = 0; i < routeButtons_.size(); ++i) {
        if (KeyCodes::matches(vk, routeButtons_[i].vk)) {
            if (!aimModeActive && routeButtons_[i].action == NODE_HOLD_VIEW) continue;
            LeaveCriticalSection(&inputLock_);
            return true;
        }
    }
    LeaveCriticalSection(&inputLock_);
    return false;
}

bool InputEngine::onKeyEvent(int vk, bool down, bool& outRequestToggleAim) {
    outRequestToggleAim = false;
    double nowMs = getTimeMs();
    bool isAim = (aimMode_ != 0);

    EnterCriticalSection(&inputLock_);
    bool consumed = classifyKeyInputLocked(vk, down, isAim, outRequestToggleAim);
    bool shouldQueue = consumed || !down;
    if (shouldQueue && thread_ != NULL) {
        QueuedKeyEvent ev = { vk, down, nowMs };
        pendingKeys_.push_back(ev);
    }
    LeaveCriticalSection(&inputLock_);

    if (thread_ == NULL) {
        EnterCriticalSection(&lock_);
        processKeyEventLocked(vk, down, nowMs);
        LeaveCriticalSection(&lock_);
    } else if (shouldQueue && wakeEvent_) {
        SetEvent(wakeEvent_);
    }
    return consumed;
}

void InputEngine::processKeyEventLocked(int vk, bool down, double nowMs, bool fromGamepad) {
    if (!profile_.keymapEnabled) return;
    bool isAim = (aimMode_ != 0);
    bool isWheel = (vk == VK_CUSTOM_MWHEEL_UP || vk == VK_CUSTOM_MWHEEL_DOWN);
    bool mouseBtn = KeyCodes::isMouseKey(vk);

    if (!isWheel && vk >= 0 && vk < 512) {
        heldKeys_[vk] = down;
    }

    uint32_t w = 1920, h = 1080;
    bridge_->getScreenSize(w, h);

    // Cursor Mode: mouse buttons ONLY trigger NODE_CURSOR_TOGGLE, EXCEPT gamepad triggers (E2)!
    if (!isAim && !fromGamepad && mouseBtn) {
        if (down) {
            ActionEffects eff;
            actions_.dispatchKey(vk, true, false, profile_, heldKeys_, w, h, nowMs,
                                 scopeSensActive_, eff, packetsThisSec_, true);
        }
        return;
    }

    // 1. Check if an explicit button or active 2-key combo owns vk (so BAG=LSHIFT / CROUCH=LCTRL wins over Sprint/Walk)
    bool hasCustomButtonOnVk = false;
    for (size_t i = 0; i < profile_.buttons.size(); ++i) {
        const KeyNode& b = profile_.buttons[i];
        bool keyMatches = KeyCodes::matches(vk, b.vk) &&
                          (b.comboVk == 0 || KeyCodes::isKeyHeld(heldKeys_, b.comboVk));
        if (keyMatches) {
            if (!isAim && !fromGamepad && b.action == NODE_HOLD_VIEW) continue;
            hasCustomButtonOnVk = true;
            break;
        }
    }

    // 2. WASD Virtual Joystick (E3: single joystick owner; last-input-wins over gamepad left stick!)
    if (!fromGamepad && joystick_.handleKey(vk, down, !hasCustomButtonOnVk, profile_, w, h, nowMs, packetsThisSec_)) {
        if (joystick_.isDown()) {
            if (joyOwner_ == JOY_GAMEPAD) {
                gamepad_.releaseJoystickOnly(packetsThisSec_);
            }
            joyOwner_ = JOY_KEYBOARD;
        } else if (joyOwner_ == JOY_KEYBOARD) {
            joyOwner_ = JOY_NONE;
        }
        return;
    }

    // 3. Fire Button (DRAG-FIRE, CLAW-LEFT, DUAL-DRAG; E2: gamepad RT works in cursor mode too)
    if ((isAim || fromGamepad || !KeyCodes::isMouseKey(profile_.fireVk)) && KeyCodes::matches(vk, profile_.fireVk)) {
        if (isWheel) {
            if (down) {
                pressFireLocked(nowMs);
                quickTapFireReleaseAtMs_ = nowMs + 40.0;
            }
            return;
        }
        if (fireHeld_ != down) {
            fireHeld_ = down;
            if (profile_.fireMode == FIRE_MODE_DRAG_RIGHT) {
                if (down) {
                    quickTapFireReleaseAtMs_ = 0.0;
                    if (viewStack_.mainView().isFingerPlaced()) {
                        viewStack_.resetMainView(nowMs);
                        pendingFireDown_ = true;
                        pendingFireAtMs_ = nowMs + static_cast<double>(profile_.touchDownDelayMs > 0 ? profile_.touchDownDelayMs : 12);
                    } else {
                        pendingFireDown_ = false;
                        pressFireLocked(nowMs);
                    }
                } else {
                    if (pendingFireDown_) {
                        quickTapFireReleaseAtMs_ = pendingFireAtMs_ + 25.0;
                    } else {
                        liftFireFingerLocked();
                        aimCooldownUntilMs_ = nowMs + static_cast<double>(profile_.touchDownDelayMs);
                    }
                }
            } else {
                // CLAW-LEFT & DUAL-DRAG
                if (down) {
                    if (isAim && !viewStack_.isOverlayActive()) {
                        viewStack_.ensureMainFingerDown(nowMs, packetsThisSec_);
                    }
                    pressFireLocked(nowMs);
                } else {
                    liftFireFingerLocked();
                }
            }
        }
        return;
    }

    if (down && profile_.castCancelVk != 0 && KeyCodes::matches(vk, profile_.castCancelVk) &&
        actions_.hasActiveSkillCast()) {
        if (actions_.cancelActiveSkillCasts(profile_, w, h, packetsThisSec_)) {
            return;
        }
    }

    // 4. Action Buttons (2-level combo lookup + 11 KeyActionType behaviors)
    ActionEffects eff;
    actions_.dispatchKey(vk, down, (isAim || fromGamepad), profile_, heldKeys_, w, h, nowMs,
                         scopeSensActive_, eff, packetsThisSec_);

    if (down) {
        DWORD glowEnd = GetTickCount() + 120u;
        for (size_t i = 0; i < profile_.buttons.size() && i < 64; ++i) {
            const KeyNode& b = profile_.buttons[i];
            bool keyMatches = KeyCodes::matches(vk, b.vk) &&
                              (b.comboVk == 0 || KeyCodes::isKeyHeld(heldKeys_, b.comboVk));
            if (!keyMatches) continue;
            if (!isAim && !fromGamepad && b.action == NODE_HOLD_VIEW) continue;
            if (!isAim && !fromGamepad && KeyCodes::isMouseKey(b.vk) && b.action != NODE_CURSOR_TOGGLE) continue;
            buttonGlowUntilTick_[i] = glowEnd;
        }
    }

    if (eff.pushHoldView && eff.holdViewIdx >= 0 &&
        eff.holdViewIdx < static_cast<int>(profile_.buttons.size())) {
        const KeyNode& b = profile_.buttons[eff.holdViewIdx];
        FpsViewConfig ovCfg;
        ovCfg.screenW = static_cast<double>(w);
        ovCfg.screenH = static_cast<double>(h);
        ovCfg.centerX = static_cast<double>(b.x) * ovCfg.screenW;
        ovCfg.centerY = static_cast<double>(b.y) * ovCfg.screenH;
        ovCfg.rectLeft = 4.0;
        ovCfg.rectTop = 4.0;
        ovCfg.rectRight = ovCfg.screenW - 5.0;
        ovCfg.rectBottom = ovCfg.screenH - 5.0;
        ovCfg.sensX = static_cast<double>(profile_.sensX);
        ovCfg.sensY = static_cast<double>(profile_.sensY);
        ovCfg.ffCounterAccel = 0.0;
        ovCfg.invertY = profile_.invertY;
        ovCfg.unbounded = profile_.unboundedAim;
        ovCfg.clampInsteadOfRelease = !profile_.unboundedAim;
        ovCfg.edgeReleaseOnly = true;
        ovCfg.touchDownRandomRangePx = 0;
        viewStack_.pushOverlay(eff.holdViewIdx, ovCfg, nowMs, packetsThisSec_);
    }
    if (eff.popHoldView) {
        if (viewStack_.isOverlayActive() && viewStack_.overlayOwnerId() == eff.popHoldViewIdx) {
            viewStack_.popOverlay(eff.popHoldViewIdx, nowMs, packetsThisSec_);
            aimCooldownUntilMs_ = nowMs + static_cast<double>(profile_.touchDownDelayMs);
        }
    }
    if (eff.resetMainView) {
        viewStack_.resetMainView(nowMs);
        aimCooldownUntilMs_ = nowMs + static_cast<double>(profile_.touchDownDelayMs);
    }
    if (eff.reactivateJoystick) {
        joystick_.reactivate(profile_, w, h, nowMs, packetsThisSec_);
    }
}

void InputEngine::pressFireLocked(double /*nowMs*/) {
    uint32_t w = 1920, h = 1080;
    bridge_->getScreenSize(w, h);
    double dw = static_cast<double>(w);
    double dh = static_cast<double>(h);

    if (profile_.fireMode == FIRE_MODE_CLAW_LEFT) {
        fireCurX_ = static_cast<double>(profile_.leftFireX) * dw;
        fireCurY_ = static_cast<double>(profile_.leftFireY) * dh;
    } else {
        fireCurX_ = static_cast<double>(profile_.fireX) * dw;
        fireCurY_ = static_cast<double>(profile_.fireY) * dh;
    }

    int32_t px = static_cast<int32_t>(std::floor(fireCurX_ + 0.5));
    int32_t py = static_cast<int32_t>(std::floor(fireCurY_ + 0.5));
    fingerPool_.emit(TOUCH_DOWN, PointerId::FIRE, px, py, true);
    fireDown_ = true;
    fireStartX_ = px;
    fireStartY_ = py;
    fireLastSentX_ = px;
    fireLastSentY_ = py;
    ++packetsThisSec_;
}

void InputEngine::liftFireFingerLocked() {
    if (fireDown_) {
        int32_t dx = fireLastSentX_ - fireStartX_;
        int32_t dy = fireLastSentY_ - fireStartY_;
        bool unclamped = (profile_.fireMode != FIRE_MODE_CLAW_LEFT && profile_.unboundedAim);
        fingerPool_.emit(TOUCH_UP, PointerId::FIRE, fireLastSentX_, fireLastSentY_, !unclamped);
        fireDown_ = false;
        fireLastSentX_ = -1;
        fireLastSentY_ = -1;
        ++packetsThisSec_;
        const char* mname = (profile_.fireMode == FIRE_MODE_DRAG_RIGHT) ? "DRAG-FIRE"
                          : (profile_.fireMode == FIRE_MODE_CLAW_LEFT)  ? "CLAW-LEFT"
                                                                        : "DUAL-DRAG";
        std::printf("[Fire] %s release | start=(%d,%d) drag=(%+d, %+d px)\n",
                    mname, fireStartX_, fireStartY_, dx, dy);
    }
    pendingFireDown_ = false;
    quickTapFireReleaseAtMs_ = 0.0;
}

void InputEngine::releaseMouseTouchesLocked() {
    double nowMs = getTimeMs();
    viewStack_.onFocusLose(nowMs);
    liftFireFingerLocked();
    fireHeld_ = false;
    scopeSensActive_ = false;
    pendingFireDown_ = false;
    quickTapFireReleaseAtMs_ = 0.0;
    aimCooldownUntilMs_ = 0.0;

    actions_.releaseButtons(profile_, true, packetsThisSec_);

    if (directCursorDown_) {
        fingerPool_.emit(TOUCH_UP, PointerId::CURSOR, directCursorLastX_, directCursorLastY_, true);
        directCursorDown_ = false;
    }
    releaseCursorScrollLocked();
    releaseCursorPinchLocked();
    releaseCursorTwoFingerPanLocked();
}

void InputEngine::releaseAllLocked() {
    releaseMouseTouchesLocked();
    joystick_.release(packetsThisSec_);
    gamepad_.releaseAll(packetsThisSec_);
    if (gamepad_.isUhidCreated()) {
        bridge_->sendUhidDestroy(3);
        gamepad_.markUhidDestroyed();
    }
    joyOwner_ = JOY_NONE;
    prevGpStickDeflected_ = false;
    std::memset(heldKeys_, 0, sizeof(heldKeys_));
    std::memset(buttonGlowUntilTick_, 0, sizeof(buttonGlowUntilTick_));
    actions_.releaseButtons(profile_, false, packetsThisSec_);
    // Final bounded-pool safety sweep: guarantees 0 active touches left on the phone
    fingerPool_.releaseAll();
}

void InputEngine::releaseAllTouches() {
    EnterCriticalSection(&lock_);
    releaseAllLocked();
    EnterCriticalSection(&inputLock_);
    pendingKeys_.clear();
    pendingCursor_.clear();
    pendingScroll_.clear();
    accumRawDx_ = 0.0;
    accumRawDy_ = 0.0;
    hasPendingAimDelta_ = false;
    std::memset(hookHeldKeys_, 0, sizeof(hookHeldKeys_));
    LeaveCriticalSection(&inputLock_);
    LeaveCriticalSection(&lock_);
}

void InputEngine::resetAimView() {
    EnterCriticalSection(&lock_);
    double nowMs = getTimeMs();
    viewStack_.resetMainView(nowMs);
    scopeSensActive_ = false;
    aimCooldownUntilMs_ = nowMs + static_cast<double>(profile_.touchDownDelayMs);
    LeaveCriticalSection(&lock_);
}

bool InputEngine::processTick(double nowMs, bool runPeriodic) {
    std::vector<QueuedKeyEvent> localKeys;
    std::vector<QueuedCursorTouch> localCursor;
    std::vector<QueuedCursorScroll> localScroll;
    float curHoverX = 0.5f, curHoverY = 0.5f;
    EnterCriticalSection(&inputLock_);
    if (!pendingKeys_.empty()) localKeys.swap(pendingKeys_);
    if (!pendingCursor_.empty()) localCursor.swap(pendingCursor_);
    if (!pendingScroll_.empty()) localScroll.swap(pendingScroll_);
    curHoverX = cursorHoverNormX_;
    curHoverY = cursorHoverNormY_;
    LeaveCriticalSection(&inputLock_);

    EnterCriticalSection(&lock_);
    actions_.setCursorNorm(curHoverX, curHoverY);

    if (nowMs - lastStatMs_ >= 1000.0) {
        lastPacketsPerSec_ = packetsThisSec_;
        packetsThisSec_ = 0;
        lastStatMs_ = nowMs;
    }

    uint32_t w = 1920, h = 1080;
    bridge_->getScreenSize(w, h);

    for (size_t i = 0; i < localKeys.size(); ++i) {
        processKeyEventLocked(localKeys[i].vk, localKeys[i].down, nowMs);
    }
    for (size_t i = 0; i < localCursor.size(); ++i) {
        if (!aimMode_) {
            if (localCursor[i].twoFingerPan) {
                processDirectCursorTwoFingerPanLocked(localCursor[i].action, localCursor[i].normX, localCursor[i].normY);
            } else {
                processDirectCursorLocked(localCursor[i].action, localCursor[i].normX, localCursor[i].normY);
            }
        }
    }
    for (size_t i = 0; i < localScroll.size(); ++i) {
        if (!aimMode_) {
            if (localScroll[i].isPinch) {
                processDirectCursorPinchLocked(localScroll[i].normX, localScroll[i].normY,
                                               localScroll[i].notches, nowMs);
            } else {
                processDirectCursorScrollLocked(localScroll[i].normX, localScroll[i].normY,
                                                localScroll[i].notches, nowMs);
            }
        }
    }

    GamepadFrameOutput gpOut;
    uint8_t navMask = 0;
    if (runPeriodic) {
        actions_.tick(profile_, nowMs, packetsThisSec_);
        joystick_.tick(profile_, w, h, nowMs, packetsThisSec_);

        // Stage 3 / F6: XInput Gamepad-to-Touch & UHID polling (E3/E4/E11/shell nav)
        double dtMs = (lastTickMs_ > 0.0) ? clampd(nowMs - lastTickMs_, 0.5, 40.0) : 4.0;
        lastTickMs_ = nowMs;
        bool wantGamepad = profile_.keymapEnabled || (profile_.padMode == PAD_MODE_NATIVE) ||
                           gamepad_.isConnected() || gamepad_.isUhidCreated();
        if (appFocused_ == 0 || editMode_ != 0) {
            gamepad_.suspendForFocusLoss(packetsThisSec_, gpOut);
            if (joyOwner_ == JOY_GAMEPAD) joyOwner_ = JOY_NONE;
            prevGpStickDeflected_ = false;
        } else if (wantGamepad) {
            bool kbOwnsJoy = (joyOwner_ == JOY_KEYBOARD);
            bool menuOpen = (shellMenuOpen_ != 0);
            gamepad_.pollAndProcess(profile_, w, h, dtMs, packetsThisSec_, gpOut, kbOwnsJoy, menuOpen);
            if (gpOut.stickDeflected && !prevGpStickDeflected_ && kbOwnsJoy) {
                joystick_.release(packetsThisSec_);
                joyOwner_ = JOY_GAMEPAD;
            } else if (gpOut.analogJoyActive) {
                joyOwner_ = JOY_GAMEPAD;
            } else if (!gpOut.stickDeflected && joyOwner_ == JOY_GAMEPAD) {
                joyOwner_ = JOY_NONE;
            }
            prevGpStickDeflected_ = gpOut.stickDeflected;
        }
        if (gpOut.uhidNeedCreate) {
            uint16_t dSz = 0;
            const uint8_t* desc = GamepadController::hidReportDescriptor(dSz);
            bridge_->sendUhidCreate(3, 0x045E, 0x028E, "PhantomTouch Gamepad", desc, dSz);
        }
        if (gpOut.uhidReportChanged) {
            bridge_->sendUhidInput(3, gpOut.uhidReport, 15);
        }
        if (gpOut.uhidNeedDestroy) {
            bridge_->sendUhidDestroy(3);
        }
        navMask = gpOut.shellNavMask;
        for (int i = 0; i < gpOut.keyDeltaCount; ++i) {
            processKeyEventLocked(gpOut.keyDeltas[i].vk, gpOut.keyDeltas[i].down, nowMs, true);
        }

        if (cursorScrollDown_ && (aimMode_ || nowMs >= cursorScrollReleaseAtMs_)) {
            releaseCursorScrollLocked();
        }
        if (cursorPinchDown_ && (aimMode_ || nowMs >= pinchReleaseAtMs_)) {
            releaseCursorPinchLocked();
        }
        if (twoFingerPanDown_ && aimMode_) {
            releaseCursorTwoFingerPanLocked();
        }

        if (pendingFireDown_ && nowMs >= pendingFireAtMs_) {
            pendingFireDown_ = false;
            pressFireLocked(nowMs);
        }
        if (fireDown_ && quickTapFireReleaseAtMs_ > 0.0 && nowMs >= quickTapFireReleaseAtMs_) {
            liftFireFingerLocked();
            aimCooldownUntilMs_ = nowMs + static_cast<double>(profile_.touchDownDelayMs);
        }
    }
    bool hasGpLook = (std::fabs(gpOut.lookDx) > 0.001 || std::fabs(gpOut.lookDy) > 0.001);

    auto computeNeedsFastTicks = [&]() -> bool {
        return joystick_.isDown() || gamepad_.isConnected() ||
               fireDown_ || fireHeld_ || pendingFireDown_ ||
               directCursorDown_ || cursorScrollDown_ || cursorPinchDown_ || twoFingerPanDown_ ||
               viewStack_.mainView().isFingerPlaced() || viewStack_.isOverlayActive() ||
               actions_.hasActiveTasks() ||
               std::fabs(smoothDx_) > 0.01 || std::fabs(smoothDy_) > 0.01;
    };

    if ((!aimMode_ && !hasGpLook && !viewStack_.mainView().isFingerPlaced()) || pendingFireDown_) {
        bool fast = computeNeedsFastTicks();
        PadNavCallbackFn cb = padNavCb_;
        void* ctx = padNavCtx_;
        LeaveCriticalSection(&lock_);
        if (cb && navMask != 0) cb(ctx, navMask);
        return fast;
    }

    double rawDx = gpOut.lookDx;
    double rawDy = gpOut.lookDy;
    bool tookRaw = false;
    if (aimMode_) {
        EnterCriticalSection(&inputLock_);
        if (hasPendingAimDelta_ || std::fabs(accumRawDx_) > 0.0001 || std::fabs(accumRawDy_) > 0.0001) {
            rawDx += accumRawDx_;
            rawDy += accumRawDy_;
            accumRawDx_ = 0.0;
            accumRawDy_ = 0.0;
            hasPendingAimDelta_ = false;
            tookRaw = true;
        }
        LeaveCriticalSection(&inputLock_);
    }
    if (tookRaw) {
        lastAimSendMs_ = nowMs;
    }

    if (w != lastConfiguredW_ || h != lastConfiguredH_) {
        syncFpsViewConfigLocked(w, h);
    }
    double dw = static_cast<double>(w);
    double dh = static_cast<double>(h);

    bool firingActive = fireHeld_ || fireDown_;
    double speed = std::sqrt(rawDx * rawDx + rawDy * rawDy);
    double accelGain = 1.0;
    if (profile_.accelPower > 1.001f && speed > static_cast<double>(profile_.accelRefSpeed)) {
        double ratio = speed / static_cast<double>(profile_.accelRefSpeed);
        accelGain = std::pow(ratio, static_cast<double>(profile_.accelPower) - 1.0);
        if (accelGain > static_cast<double>(profile_.accelCap)) {
            accelGain = static_cast<double>(profile_.accelCap);
        }
    }

    double multX = accelGain;
    double multY = accelGain;
    if (scopeSensActive_) {
        multX *= static_cast<double>(profile_.scopeSensX);
        multY *= static_cast<double>(profile_.scopeSensY);
    }
    if (firingActive) {
        multX *= static_cast<double>(profile_.fireSensX);
        multY *= static_cast<double>(profile_.fireSensY);
    }

    double feedDx = rawDx;
    double feedDy = rawDy;
    if (profile_.smoothing > 0.001f) {
        double alpha = clampd(static_cast<double>(profile_.smoothing), 0.0, 0.85);
        smoothDx_ = smoothDx_ * alpha + rawDx * (1.0 - alpha);
        smoothDy_ = smoothDy_ * alpha + rawDy * (1.0 - alpha);
        if (std::fabs(smoothDx_) < 0.02) smoothDx_ = 0.0;
        if (std::fabs(smoothDy_) < 0.02) smoothDy_ = 0.0;
        feedDx = smoothDx_;
        feedDy = smoothDy_;
    }

    bool hasFeed = (std::fabs(feedDx) > 0.0001 || std::fabs(feedDy) > 0.0001);

    if (viewStack_.isOverlayActive()) {
        if (hasFeed) {
            viewStack_.onMouseDelta(feedDx, feedDy, 1.0, 1.0, nowMs, packetsThisSec_);
        }
        viewStack_.tick(nowMs, true, packetsThisSec_);
        PadNavCallbackFn cb = padNavCb_;
        void* ctx = padNavCtx_;
        LeaveCriticalSection(&lock_);
        if (cb && navMask != 0) cb(ctx, navMask);
        return true;
    }

    auto moveFireFinger = [&]() {
        if (!hasFeed || !fireDown_) return;
        double dx = feedDx * static_cast<double>(profile_.sensX) * multX;
        double dy = feedDy * static_cast<double>(profile_.sensY) * multY;
        if (profile_.invertY) dy = -dy;
        if (profile_.ffCounterAccel > 0.01f && dw > 1.0) {
            double r = std::fabs(fireCurX_ - static_cast<double>(fireStartX_)) / dw;
            double k = static_cast<double>(profile_.ffCounterAccel);
            dx *= ((0.98 / (k * r * r + 1.0)) + 0.02);
        }

        if (profile_.unboundedAim) {
            fireCurX_ += clampd(dx, -160.0, 160.0);
            fireCurY_ += clampd(dy, -160.0, 160.0);
        } else {
            double anchorX = static_cast<double>(profile_.fireX) * dw;
            double anchorY = static_cast<double>(profile_.fireY) * dh;
            double minX = clampd(anchorX - static_cast<double>(profile_.fireBoxHalfW) * dw, dw * 0.20, dw - 4.0);
            double maxX = clampd(anchorX + static_cast<double>(profile_.fireBoxHalfW) * dw, dw * 0.20, dw - 4.0);
            double minY = clampd(anchorY - static_cast<double>(profile_.fireBoxHalfH) * dh, dh * 0.02, dh - 4.0);
            double maxY = clampd(anchorY + static_cast<double>(profile_.fireBoxHalfH) * dh, dh * 0.02, dh * 0.98);
            fireCurX_ = clampd(fireCurX_ + clampd(dx, -160.0, 160.0), minX, maxX);
            fireCurY_ = clampd(fireCurY_ + clampd(dy, -160.0, 160.0), minY, maxY);
        }

        int32_t px = static_cast<int32_t>(std::floor(fireCurX_ + 0.5));
        int32_t py = static_cast<int32_t>(std::floor(fireCurY_ + 0.5));
        if (px != fireLastSentX_ || py != fireLastSentY_) {
            fingerPool_.emit(TOUCH_MOVE, PointerId::FIRE, px, py, !profile_.unboundedAim);
            fireLastSentX_ = px;
            fireLastSentY_ = py;
            ++packetsThisSec_;
        }
    };

    auto moveMainCamera = [&]() {
        if (nowMs < aimCooldownUntilMs_) return;
        if (hasFeed) {
            viewStack_.onMouseDelta(feedDx, feedDy, multX, multY, nowMs, packetsThisSec_);
        }
        viewStack_.tick(nowMs, firingActive, packetsThisSec_);
    };

    if (profile_.fireMode == FIRE_MODE_DRAG_RIGHT && fireDown_) {
        moveFireFinger();
    } else if (profile_.fireMode == FIRE_MODE_DUAL_DRAG && fireDown_) {
        moveMainCamera();
        moveFireFinger();
    } else {
        moveMainCamera();
    }

    bool fast = hasFeed || computeNeedsFastTicks();
    PadNavCallbackFn cb = padNavCb_;
    void* ctx = padNavCtx_;
    LeaveCriticalSection(&lock_);
    if (cb && navMask != 0) cb(ctx, navMask);
    return fast;
}

DWORD WINAPI InputEngine::tickThreadProc(LPVOID param) {
    static_cast<InputEngine*>(param)->runLoop();
    return 0;
}

void InputEngine::runLoop() {
    DWORD taskIdx = 0;
    HANDLE avrt = ThreadScheduling::enableMmcssGames(taskIdx);
    timeBeginPeriod(1);
    double nextTickMs = getTimeMs();
    lastAimSendMs_ = nextTickMs - 10.0;

    while (!stopFlag_) {
        int hz = static_cast<int>(pollHzAtomic_);
        if (hz <= 0) hz = 250; // 1.5: 250 Hz periodic for joystick glide & gamepad; aim sends on arrival!
        if (hz < 60) hz = 60;
        if (hz > 500) hz = 500;
        double periodMs = 1000.0 / static_cast<double>(hz);

        double nowMs = getTimeMs();
        bool periodicDue = (nowMs >= nextTickMs);
        bool hasEventQueue = false;
        bool hasPendingAim = false;
        EnterCriticalSection(&inputLock_);
        hasEventQueue = !pendingKeys_.empty() || !pendingCursor_.empty() || !pendingScroll_.empty();
        hasPendingAim = hasPendingAimDelta_;
        LeaveCriticalSection(&inputLock_);

        bool aimDue = (!periodicDue && hasPendingAim && ((nowMs - lastAimSendMs_) >= 1.0));
        bool active = false;
        if (periodicDue || hasEventQueue || aimDue) {
            active = processTick(nowMs, periodicDue);
            if (periodicDue) {
                nextTickMs += periodMs;
                if (nextTickMs <= nowMs) {
                    nextTickMs = nowMs + periodMs;
                }
            }
        }

        // Check immediately if fresh input is ready now (after 1ms aim coalescing floor)
        bool immediateMore = false;
        bool aimWaitingCoalesce = false;
        EnterCriticalSection(&inputLock_);
        immediateMore = !pendingKeys_.empty() || !pendingCursor_.empty() || !pendingScroll_.empty();
        aimWaitingCoalesce = hasPendingAimDelta_;
        LeaveCriticalSection(&inputLock_);

        double afterMs = getTimeMs();
        if (immediateMore || (aimWaitingCoalesce && (afterMs - lastAimSendMs_) >= 1.0)) {
            continue;
        }

        if (!active && !aimWaitingCoalesce) {
            // Idle (0.0% CPU): wakes instantaneously (<0.1ms) on ANY key, click, scroll, or raw mouse delta!
            WaitForSingleObject(wakeEvent_, 20);
            nextTickMs = getTimeMs();
            continue;
        }

        double targetMs = nextTickMs;
        if (aimWaitingCoalesce) {
            double aimReadyMs = lastAimSendMs_ + 1.0;
            if (aimReadyMs < targetMs) targetMs = aimReadyMs;
        }
        double waitMs = targetMs - afterMs;
        if (waitMs >= 0.4) {
            if (hiResTimer_) {
                LARGE_INTEGER due;
                due.QuadPart = -static_cast<LONGLONG>(waitMs * 10000.0);
                if (due.QuadPart >= 0) due.QuadPart = -4000;
                SetWaitableTimer(hiResTimer_, &due, 0, NULL, NULL, FALSE);
                HANDLE handles[2] = { wakeEvent_, hiResTimer_ };
                WaitForMultipleObjects(2, handles, FALSE, 20);
            } else {
                DWORD sleepMs = static_cast<DWORD>(waitMs);
                if (sleepMs < 1) sleepMs = 1;
                WaitForSingleObject(wakeEvent_, sleepMs);
            }
        }
    }

    timeEndPeriod(1);
    ThreadScheduling::disableMmcss(avrt);
}

EngineTelemetry InputEngine::getTelemetry() const {
    EngineTelemetry t;
    EnterCriticalSection(&lock_);
    uint32_t w = 1920, h = 1080;
    bridge_->getScreenSize(w, h);
    t.screenW = w;
    t.screenH = h;
    t.aimMode = (aimMode_ != 0);
    t.scopeActive = scopeSensActive_;
    t.holdViewActive = viewStack_.isOverlayActive();
    bool mainDown = viewStack_.mainView().isFingerPlaced();
    bool ovDown = viewStack_.isOverlayActive() && viewStack_.overlayView().isFingerPlaced();
    t.aimFingerDown = mainDown || ovDown || (profile_.fireMode != FIRE_MODE_CLAW_LEFT && fireDown_);
    t.fireHeld = fireHeld_;
    t.joyDown = joystick_.isDown() || gamepad_.isJoyDown();
    t.gamepadConnected = gamepad_.isConnected();
    if (ovDown) {
        t.aimFingerNormX = static_cast<float>(clampd(viewStack_.overlayView().curX() / static_cast<double>(w), 0.0, 1.0));
        t.aimFingerNormY = static_cast<float>(clampd(viewStack_.overlayView().curY() / static_cast<double>(h), 0.0, 1.0));
    } else if (profile_.fireMode != FIRE_MODE_CLAW_LEFT && fireDown_) {
        t.aimFingerNormX = static_cast<float>(clampd(fireCurX_ / static_cast<double>(w), 0.0, 1.0));
        t.aimFingerNormY = static_cast<float>(clampd(fireCurY_ / static_cast<double>(h), 0.0, 1.0));
    } else if (mainDown) {
        t.aimFingerNormX = static_cast<float>(clampd(viewStack_.mainView().curX() / static_cast<double>(w), 0.0, 1.0));
        t.aimFingerNormY = static_cast<float>(clampd(viewStack_.mainView().curY() / static_cast<double>(h), 0.0, 1.0));
    } else {
        t.aimFingerNormX = profile_.aimX;
        t.aimFingerNormY = profile_.aimY;
    }
    if (joystick_.isDown()) {
        t.joyFingerNormX = static_cast<float>(joystick_.lastX()) / static_cast<float>(w);
        t.joyFingerNormY = static_cast<float>(joystick_.lastY()) / static_cast<float>(h);
    } else {
        t.joyFingerNormX = profile_.joyX;
        t.joyFingerNormY = profile_.joyY;
    }
    t.touchRateHz = lastPacketsPerSec_;
    uint64_t glowMask = 0;
    DWORD nowTick = GetTickCount();
    for (size_t i = 0; i < profile_.buttons.size() && i < 64; ++i) {
        if (buttonGlowUntilTick_[i] != 0 && static_cast<LONG>(nowTick - buttonGlowUntilTick_[i]) < 0) {
            glowMask |= (1ULL << i);
        }
    }
    t.activeButtonMask = glowMask;
    LeaveCriticalSection(&lock_);
    return t;
}
